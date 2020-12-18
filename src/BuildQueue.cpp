#include "BuildQueue.hpp"
#include "DagData.hpp"
#include "Profiler.hpp"
#include "SignalHandler.hpp"
#include "SharedResources.hpp"
#include "NodeResultPrinting.hpp"
#include "RuntimeNode.hpp"
#include "BuildLoop.hpp"
#include "Driver.hpp"
#include "NodeResultPrinting.hpp"
#include <stdarg.h>
#include <algorithm>

#include <stdio.h>


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
    LinearAllocDestroy(&self->m_ScratchAlloc, true);
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



void BuildQueueInit(BuildQueue *queue, const BuildQueueConfig *config, const char** targets, int target_count)
{
    ProfilerScope prof_scope("Tundra BuildQueueInit", 0);

    MutexInit(&queue->m_Lock);
    CondInit(&queue->m_WorkAvailable);
    CondInit(&queue->m_MaxJobsChangedConditionalVariable);
    CondInit(&queue->m_BuildFinishedConditionalVariable);
    MutexInit(&queue->m_BuildFinishedMutex);

    MemAllocHeap *heap = config->m_Heap;

    BufferInitWithCapacity(&queue->m_WorkStack, heap, 1024);
    queue->m_Config = *config;
    queue->m_FinalBuildResult = BuildResult::kOk;
    queue->m_OutOfDateSignaturePath = nullptr;
    queue->m_FinishedNodeCount = 0;
    queue->m_MainThreadWantsToCleanUp = false;
    queue->m_BuildFinishedConditionalVariableSignaled = false;
    queue->m_AmountOfNodesEverQueued = 0;
    queue->m_SharedResourcesCreated = HeapAllocateArrayZeroed<uint32_t>(heap, config->m_SharedResourcesCount);
    MutexInit(&queue->m_SharedResourcesLock);

    BufferInitWithCapacity(&queue->m_Config.m_RequestedNodes, queue->m_Config.m_Heap, 32);
    DriverSelectNodes(queue->m_Config.m_Dag, targets, target_count, &queue->m_Config.m_RequestedNodes,  queue->m_Config.m_Heap);

    SignalHandlerSetCondition(&queue->m_BuildFinishedConditionalVariable);

    // Create build threads.
    for (int i = 0, thread_count = queue->m_Config.m_DriverOptions->m_ThreadCount; i < thread_count; ++i)
    {
        ThreadState *thread_state = &queue->m_ThreadState[i];

        //the profiler thread id here is "i+1",  since if we have 4 buildthreads, we'll have 5 total threads, as the main thread doesn't participate in building, but only sleeps
        //and pumps the OS messageloop.
        ThreadStateInit(thread_state, queue, MB(32), i, i + 1);

        Log(kDebug, "starting build thread %d", i);
        queue->m_Threads[i] = ThreadStart(BuildThreadRoutine, thread_state, "Build Thread");
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

    for (int i = 0, thread_count = config->m_DriverOptions->m_ThreadCount; i < thread_count; ++i)
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


    MemAllocHeap *heap = queue->m_Config.m_Heap;
    BufferDestroy(&queue->m_Config.m_RequestedNodes, heap);

    // Deallocate storage.
    BufferDestroy(&queue->m_WorkStack, heap);
    HeapFree(heap, queue->m_SharedResourcesCreated);
    MutexDestroy(&queue->m_SharedResourcesLock);

    CondDestroy(&queue->m_WorkAvailable);
    CondDestroy(&queue->m_MaxJobsChangedConditionalVariable);

    MutexDestroy(&queue->m_Lock);
    MutexDestroy(&queue->m_BuildFinishedMutex);

    // Unblock all signals on the main thread.
    SignalHandlerSetCondition(nullptr);
}

BuildResult::Enum BuildQueueBuild(BuildQueue *queue, MemAllocLinear* scratch)
{
    // Make sure none of the build threads see in-progress state due to a spurious wakeup.
    MutexLock(&queue->m_Lock);
    MutexLock(&queue->m_BuildFinishedMutex);

    // Initialize build queue with index range to build
    RuntimeNode *runtime_nodes = queue->m_Config.m_RuntimeNodes;

    for (auto requestedNode:  queue->m_Config.m_RequestedNodes)
    {
        RuntimeNode *runtime_node = runtime_nodes + requestedNode;
        EnqueueNodeWithoutWakingAwaiters(queue, queue->m_Config.m_LinearAllocator, runtime_node, nullptr);
    }
    SortWorkingStack(queue);

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
        CondWait(&queue->m_BuildFinishedConditionalVariable, &queue->m_BuildFinishedMutex);
    }
    MutexUnlock(&queue->m_BuildFinishedMutex);

    if (SignalGetReason())
        return BuildResult::kInterrupted;

    return queue->m_FinalBuildResult;
}

