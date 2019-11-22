#include "BuildQueue.hpp"
#include "DagData.hpp"
#include "Profiler.hpp"
#include "SignalHandler.hpp"
#include "SharedResources.hpp"
#include "NodeResultPrinting.hpp"
#include "HumanActivityDetection.hpp"
#include "NodeState.hpp"
#include "BuildLoop.hpp"
#include <stdarg.h>
#include <algorithm>

#include <stdio.h>

namespace t2
{
namespace BuildResult
{
const char *Names[Enum::kCount] =
    {
        "build success",
        "build interrupted",
        "build failed",
        "build failed to setup error",
        "requires additional run"};
}

static void ThreadStateInit(ThreadState *self, BuildQueue *queue, size_t scratch_size, int index, int profiler_thread_id)
{
    HeapInit(&self->m_LocalHeap);
    LinearAllocInit(&self->m_ScratchAlloc, &self->m_LocalHeap, scratch_size, "thread-local scratch");
    self->m_ThreadIndex = index;
    self->m_Queue = queue;
    self->m_ProfilerThreadId = profiler_thread_id;
}

static void ThreadStateDestroy(ThreadState *self)
{
    LinearAllocDestroy(&self->m_ScratchAlloc);
    HeapDestroy(&self->m_LocalHeap);
}

static void WakeupAllBuildThreadsSoTheyCanExit(BuildQueue *queue)
{
    //build threads are either waiting on m_WorkAvailable signal, or on m_MaxJobsChangedConditionalVariable. Let's send 'm both.
    CondBroadcast(&queue->m_WorkAvailable);
    CondBroadcast(&queue->m_MaxJobsChangedConditionalVariable);
}

static ThreadRoutineReturnType TUNDRA_STDCALL BuildThreadRoutine(void *param)
{
    ThreadState *thread_state = static_cast<ThreadState *>(param);

    LinearAllocSetOwner(&thread_state->m_ScratchAlloc, ThreadCurrent());

    BuildLoop(thread_state);

    return 0;
}

void BuildQueueInit(BuildQueue *queue, const BuildQueueConfig *config)
{
    ProfilerScope prof_scope("Tundra BuildQueueInit", 0);

    MutexInit(&queue->m_Lock);
    CondInit(&queue->m_WorkAvailable);
    CondInit(&queue->m_MaxJobsChangedConditionalVariable);
    CondInit(&queue->m_BuildFinishedConditionalVariable);
    MutexInit(&queue->m_BuildFinishedMutex);
    MutexLock(&queue->m_BuildFinishedMutex);

    // Compute queue capacity. Allocate space for a power of two number of
    // indices that's at least one larger than the max number of nodes. Because
    // the queue is treated as a ring buffer, we want W=R to mean an empty
    // buffer.
    uint32_t capacity = NextPowerOfTwo(config->m_MaxNodes + 1);

    MemAllocHeap *heap = config->m_Heap;

    queue->m_Queue = HeapAllocateArray<int32_t>(heap, capacity);
    queue->m_QueueReadIndex = 0;
    queue->m_QueueWriteIndex = 0;
    queue->m_QueueCapacity = capacity;
    queue->m_Config = *config;
    queue->m_FinalBuildResult = BuildResult::kOk;
    queue->m_FinishedNodeCount = 0;
    queue->m_MainThreadWantsToCleanUp = false;
    queue->m_BuildFinishedConditionalVariableSignaled = false;
    queue->m_SharedResourcesCreated = HeapAllocateArrayZeroed<uint32_t>(heap, config->m_SharedResourcesCount);
    MutexInit(&queue->m_SharedResourcesLock);

    CHECK(queue->m_Queue);

    if (queue->m_Config.m_ThreadCount > kMaxBuildThreads)
    {
        Log(kWarning, "too many build threads (%d) - clamping to %d",
            queue->m_Config.m_ThreadCount, kMaxBuildThreads);

        queue->m_Config.m_ThreadCount = kMaxBuildThreads;
    }
    queue->m_DynamicMaxJobs = queue->m_Config.m_ThreadCount;

    Log(kDebug, "build queue initialized; ring buffer capacity = %u", queue->m_QueueCapacity);

    // Block all signals on the main thread.
    SignalBlockThread(true);
    SignalHandlerSetCondition(&queue->m_BuildFinishedConditionalVariable);

    // Create build threads.
    for (int i = 0, thread_count = queue->m_Config.m_ThreadCount; i < thread_count; ++i)
    {
        ThreadState *thread_state = &queue->m_ThreadState[i];

        //the profiler thread id here is "i+1",  since if we have 4 buildthreads, we'll have 5 total threads, as the main thread doesn't participate in building, but only sleeps
        //and pumps the OS messageloop.
        ThreadStateInit(thread_state, queue, MB(32), i, i + 1);

        Log(kDebug, "starting build thread %d", i);
        queue->m_Threads[i] = ThreadStart(BuildThreadRoutine, thread_state);
    }
}

void BuildQueueDestroy(BuildQueue *queue)
{
    Log(kDebug, "destroying build queue");
    const BuildQueueConfig *config = &queue->m_Config;

    //We need to take the m_Lock while setting the m_MainThreadWantsToCleanUp boolean, so that we are sure that when we wake up all buildthreads right after,  they will all be in a state where they
    //are guaranteed to go and check if they should quit.  possible states the buildthread can be in: waiting for a signal so they can do more work,  or actually doing build work.
    MutexLock(&queue->m_Lock);
    queue->m_MainThreadWantsToCleanUp = true;
    WakeupAllBuildThreadsSoTheyCanExit(queue);
    MutexUnlock(&queue->m_Lock);

    for (int i = 0, thread_count = config->m_ThreadCount; i < thread_count; ++i)
    {
        {
            ProfilerScope profile_scope("JoinBuildThread", 0);
            ThreadJoin(queue->m_Threads[i]);
        }
        ThreadStateDestroy(&queue->m_ThreadState[i]);
    }

    {
        ProfilerScope profile_scope("SharedResourceDestroy", 0);
        // Destroy any shared resources that were created
        for (int i = 0; i < config->m_SharedResourcesCount; ++i)
            if (queue->m_SharedResourcesCreated[i] > 0)
                SharedResourceDestroy(queue, config->m_Heap, i);
    }

    // Output any deferred error messages.
    MutexLock(&queue->m_Lock);
    PrintDeferredMessages(queue);
    MutexUnlock(&queue->m_Lock);

    // Deallocate storage.
    MemAllocHeap *heap = queue->m_Config.m_Heap;
    HeapFree(heap, queue->m_Queue);
    HeapFree(heap, queue->m_SharedResourcesCreated);
    MutexDestroy(&queue->m_SharedResourcesLock);

    CondDestroy(&queue->m_WorkAvailable);
    CondDestroy(&queue->m_MaxJobsChangedConditionalVariable);

    MutexDestroy(&queue->m_Lock);
    MutexDestroy(&queue->m_BuildFinishedMutex);

    // Unblock all signals on the main thread.
    SignalHandlerSetCondition(nullptr);
    SignalBlockThread(false);
}

static void SetNewDynamicMaxJobs(BuildQueue *queue, int maxJobs, const char *formatString, ...)
{
    queue->m_DynamicMaxJobs = maxJobs;
    CondBroadcast(&queue->m_MaxJobsChangedConditionalVariable);

    char buffer[2000];
    va_list args;
    va_start(args, formatString);
    vsnprintf(buffer, sizeof(buffer), formatString, args);
    va_end(args);

    PrintNonNodeActionResult(0, queue->m_Config.m_MaxNodes, MessageStatusLevel::Warning, buffer);
}

static bool throttled = false;

static void ProcessThrottling(BuildQueue *queue)
{
    if (!queue->m_Config.m_ThrottleOnHumanActivity)
        return;

    double t = TimeSinceLastDetectedHumanActivityOnMachine();

    //in case we've not seen any activity at all (which is what happens if you just started the build), we don't want to do any throttling.
    if (t == -1)
        return;

    int throttleInactivityPeriod = queue->m_Config.m_ThrottleInactivityPeriod;

    if (!throttled)
    {
        //if the last time we saw activity was a long time ago, we can stay unthrottled
        if (t >= throttleInactivityPeriod)
            return;

        //if we see activity just now, we want to throttle, but let's not do it in the first few seconds, otherwise when a user manually aborts the build,
        //right before aborting she'll see a throttling message.
        if (t < 1)
            return;

        //ok, let's actually throttle;
        int maxJobs = queue->m_Config.m_ThrottledThreadsAmount;
        if (maxJobs == 0)
            maxJobs = std::max(1, (int)(queue->m_Config.m_ThreadCount * 0.6));
        SetNewDynamicMaxJobs(queue, maxJobs, "Human activity detected, throttling to %d simultaneous jobs to leave system responsive", maxJobs);
        throttled = true;
    }

    //so we are throttled.  if there has been recent user activity, that's fine, we want to continue to be throttled.
    if (t < throttleInactivityPeriod)
        return;

    //if we're throttled but haven't seen any user interaction with the machine for a while, we'll unthrottle.
    int maxJobs = queue->m_Config.m_ThreadCount;
    SetNewDynamicMaxJobs(queue, maxJobs, "No human activity detected on this machine for %d seconds, unthrottling back up to %d simultaneous jobs", throttleInactivityPeriod, maxJobs);
    throttled = false;
}

BuildResult::Enum BuildQueueBuildNodeRange(BuildQueue *queue, int start_index, int count)
{
    // Make sure none of the build threads see in-progress state due to a spurious wakeup.
    MutexLock(&queue->m_Lock);

    CHECK(start_index + count <= queue->m_Config.m_MaxNodes);

    // Initialize build queue with index range to build
    int32_t *build_queue = queue->m_Queue;
    NodeState *node_states = queue->m_Config.m_NodeState;

    int amountQueued = 0;
    for (int i = 0; i < count; ++i)
    {
        NodeState *state = node_states + start_index + i;

        //to start up, let's enqueue all nodes that have 0 dependencies.
        if (state->m_MmapData->m_Dependencies.GetCount() == 0)
        {
            NodeStateFlagQueued(state);
            build_queue[amountQueued++] = start_index + i;
        }
    }

    queue->m_QueueWriteIndex = amountQueued;
    queue->m_QueueReadIndex = 0;

    CondBroadcast(&queue->m_WorkAvailable);

    auto ShouldContinue = [=]() {
        if (queue->m_BuildFinishedConditionalVariableSignaled)
            return false;
        if (SignalGetReason() != nullptr)
            return false;

        return true;
    };

    MutexUnlock(&queue->m_Lock);
    while (ShouldContinue())
    {
        PumpOSMessageLoop();

        ProcessThrottling(queue);

        //we need a timeout version of CondWait so that we ensure we continue to pump the OS message loop from time to time.
        //Turns out that's not super trivial to implement on osx without clock_gettime() which is 10.12 and up.  Since we only
        //really support throttling and os message pumps on windows today, let's postpone this problem to another day, and use
        //the non-timing out version on non windows platforms

#if WIN32
        CondWait(&queue->m_BuildFinishedConditionalVariable, &queue->m_BuildFinishedMutex, 100);
#else
        CondWait(&queue->m_BuildFinishedConditionalVariable, &queue->m_BuildFinishedMutex);
#endif
    }
    MutexUnlock(&queue->m_BuildFinishedMutex);

    if (SignalGetReason())
        return BuildResult::kInterrupted;
    return queue->m_FinalBuildResult;
}
} // namespace t2
