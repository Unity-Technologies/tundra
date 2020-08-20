#include "BuildQueue.hpp"
#include "DagData.hpp"
#include "MemAllocHeap.hpp"
#include "MemAllocLinear.hpp"
#include "RuntimeNode.hpp"
#include "Scanner.hpp"
#include "FileInfo.hpp"
#include "AllBuiltNodes.hpp"
#include "SignalHandler.hpp"
#include "Exec.hpp"
#include "Stats.hpp"
#include "StatCache.hpp"
#include "FileSign.hpp"
#include "Hash.hpp"
#include "Atomic.hpp"
#include "Profiler.hpp"
#include "NodeResultPrinting.hpp"
#include "OutputValidation.hpp"
#include "DigestCache.hpp"
#include "SharedResources.hpp"
#include "HumanActivityDetection.hpp"
#include "InputSignature.hpp"
#include "MakeDirectories.hpp"
#include "BuildLoop.hpp"
#include "RunAction.hpp"
#include "BuildQueue.hpp"
#include "Driver.hpp"
#include "LeafInputSignature.hpp"
#include "CacheClient.hpp"
#include <stdarg.h>
#include <algorithm>
#include <stdio.h>
#include <time.h>

static int AvailableNodeCount(BuildQueue *queue)
{
    const uint32_t queue_mask = queue->m_QueueCapacity - 1;
    uint32_t read_index = queue->m_QueueReadIndex;
    uint32_t write_index = queue->m_QueueWriteIndex;

    return (write_index - read_index) & queue_mask;
}

static RuntimeNode *GetRuntimeNodeForDagNodeIndex(BuildQueue *queue, int32_t src_index)
{
    int32_t state_index = queue->m_Config.m_DagNodeIndexToRuntimeNodeIndex_Table[src_index];

    if (state_index == -1)
        return nullptr;

    RuntimeNode *runtime_node = queue->m_Config.m_RuntimeNodes + state_index;

    CHECK(int(runtime_node->m_DagNode - queue->m_Config.m_DagNodes) == src_index);

    return runtime_node;
}

static void WakeWaiters(BuildQueue *queue, int count)
{
    if (count > 1)
        CondBroadcast(&queue->m_WorkAvailable);
    else
        CondSignal(&queue->m_WorkAvailable);
}

static void Enqueue(BuildQueue *queue, RuntimeNode *runtime_node)
{
    uint32_t write_index = queue->m_QueueWriteIndex;
    const uint32_t queue_mask = queue->m_QueueCapacity - 1;
    int32_t *build_queue = queue->m_Queue;

    CHECK(!RuntimeNodeIsQueued(runtime_node));
    CHECK(!RuntimeNodeIsActive(runtime_node));

#if ENABLED(CHECKED_BUILD)
    const int avail_init = AvailableNodeCount(queue);
#endif

    int runtime_node_index = int(runtime_node - queue->m_Config.m_RuntimeNodes);

    build_queue[write_index] = runtime_node_index;
    write_index = (write_index + 1) & queue_mask;
    queue->m_QueueWriteIndex = write_index;

    if (!RuntimeNodeHasEverBeenQueued(runtime_node))
        queue->m_AmountOfNodesEverQueued++;

    RuntimeNodeFlagQueued(runtime_node);

    CHECK(AvailableNodeCount(queue) == 1 + avail_init);
}

static bool AllDependenciesAreFinished(BuildQueue *queue, RuntimeNode *runtime_node)
{
    for (int32_t dep_index : queue->m_Config.m_DagDerived->m_Dependencies[runtime_node->m_DagNodeIndex])
    {
        RuntimeNode *runtime_node = GetRuntimeNodeForDagNodeIndex(queue, dep_index);
        if (!runtime_node->m_Finished)
            return false;
    }
    return true;
}

static bool AllDependenciesAreSuccesful(BuildQueue *queue, RuntimeNode *runtime_node)
{
    for (int32_t dep_index : queue->m_Config.m_DagDerived->m_Dependencies[runtime_node->m_DagNodeIndex])
    {
        RuntimeNode *runtime_node = GetRuntimeNodeForDagNodeIndex(queue, dep_index);
        CHECK(runtime_node->m_Finished);

        if (runtime_node->m_BuildResult != NodeBuildResult::kRanSuccesfully && runtime_node->m_BuildResult != NodeBuildResult::kUpToDate)
            return false;
    }
    return true;
}

static void EnqueueDependeesWhoMightNowHaveBecomeReadyToRun(BuildQueue *queue, RuntimeNode *node)
{
    int enqueue_count = 0;

    const FrozenArray<uint32_t>& backLinks = queue->m_Config.m_DagDerived->m_NodeBacklinks[node->m_DagNodeIndex];

    for (int32_t link : backLinks)
    {
        if (RuntimeNode *waiter = GetRuntimeNodeForDagNodeIndex(queue, link))
        {
            // Did someone else get to the node first?
            if (RuntimeNodeIsQueued(waiter) || RuntimeNodeIsActive(waiter) || waiter->m_Finished)
                continue;

            //we should only enqueue nodes that depend on us that we are actually trying to build
            if (!RuntimeNodeHasEverBeenQueued(waiter))
                continue;

            // If the node isn't ready, skip it.
            if (!AllDependenciesAreFinished(queue, waiter))
                continue;

            Enqueue(queue, waiter);
            ++enqueue_count;
        }
    }

    //if we're enqueing only one thing, this thread will immediately pick that up in the next loop iteration.
    //if we're enqueing more than one node, let's wake up enough threads so that they can be immediately picked up
    if (enqueue_count > 1)
        WakeWaiters(queue, enqueue_count-1);
}

static void SignalMainThreadToStartCleaningUp(BuildQueue *queue)
{
    //There are three ways for a build to end:
    //1) aborted by a signal.  The signal will end up CondSignal()-ing the m_BuildFinishedConditionalVariable that the mainthread is waiting on.  Mainthread will iniate teardown.
    //2) by a node failing to build. In this case we will ask the main thread to initiate teardown also by signaling m_BuildFinishedConditionalVariable
    //3) by the build being succesfully finished.  Same as #2, we also signal, and ask the mainthread to initiate a cleanup

    MutexLock(&queue->m_BuildFinishedMutex);
    queue->m_BuildFinishedConditionalVariableSignaled = true;
    CondSignal(&queue->m_BuildFinishedConditionalVariable);
    MutexUnlock(&queue->m_BuildFinishedMutex);
}

static void FinishNode(BuildQueue* queue, RuntimeNode* node)
{
    node->m_Finished = true;
    queue->m_FinishedNodeCount++;

    if (RuntimeNodeIsExplicitlyRequested(node))
        queue->m_FinishedRequestedNodeCount++;

    RuntimeNodeFlagInactive(node);
    if (queue->m_FinishedRequestedNodeCount == queue->m_Config.m_AmountOfRuntimeNodesSpecificallyRequested)
        SignalMainThreadToStartCleaningUp(queue);

    EnqueueDependeesWhoMightNowHaveBecomeReadyToRun(queue, node);
}


static bool IsNodeCacheableByLeafInputs(RuntimeNode* node)
{
    return 0 != (node->m_DagNode->m_Flags & Frozen::DagNode::kFlagCacheableByLeafInputs);
}


static NodeBuildResult::Enum ExecuteNode(BuildQueue* queue, RuntimeNode* node, Mutex *queue_lock, ThreadState* thread_state, StatCache* stat_cache, const Frozen::DagDerived* dagDerived)
{
    bool haveToRunAction = CheckInputSignatureToSeeNodeNeedsExecuting(queue, thread_state, node);
    if (!haveToRunAction)
        return NodeBuildResult::kUpToDate;

    if (IsNodeCacheableByLeafInputs(node))
    {
        if (!VerifyAllVersionedFilesIncludedByGeneratedHeaderFilesWereAlreadyPartOfTheLeafInputs(queue, thread_state, node, dagDerived))
            return NodeBuildResult::kRanFailed;
    }

    NodeBuildResult::Enum runActionResult = RunAction(queue, thread_state, node, queue_lock);

    if (runActionResult == NodeBuildResult::kRanSuccesfully && queue->m_Config.m_AttemptCacheWrites && IsNodeCacheableByLeafInputs(node))
    {
        uint64_t time_exec_started = TimerGet();
        auto writeResult = CacheClient::AttemptWrite(queue->m_Config.m_Dag, node->m_DagNode, node->m_CurrentLeafInputSignature, stat_cache, queue_lock, thread_state);
        uint64_t now = TimerGet();
        double duration = TimerDiffSeconds(time_exec_started, now);

        MutexLock(&queue->m_Lock);
        char digestString[kDigestStringSize];
        DigestToString(digestString, node->m_CurrentLeafInputSignature);

        PrintMessage(writeResult == CacheResult::Success ? MessageStatusLevel::Success : MessageStatusLevel::Warning, duration, "%s [CacheWrite %s]", node->m_DagNode->m_Annotation.Get(), digestString);
        MutexUnlock(&queue->m_Lock);
    }

    return runActionResult;
}


static bool AttemptToMakeConsistentWithoutNeedingDependenciesBuilt(RuntimeNode* node, BuildQueue* queue, ThreadState* thread_state)
{
    if (RuntimeNodeHasAttemptedCacheLookup(node))
        return false;

    if (node->m_BuiltNode)
    {
        if (node->m_BuiltNode->m_LeafInputSignature == node->m_CurrentLeafInputSignature && !OutputFilesMissingFor(node->m_BuiltNode, queue->m_Config.m_StatCache) && node->m_BuiltNode->m_WasBuiltSuccessfully)
        {
            node->m_BuildResult = NodeBuildResult::kUpToDate;
            FinishNode(queue, node);
            return true;
        }
    }

    RuntimeNodeSetAttemptedCacheLookup(node);

    uint64_t time_exec_started = TimerGet();
    MutexUnlock(&queue->m_Lock);
    auto cacheReadResult = CacheClient::AttemptRead(queue->m_Config.m_Dag, node->m_DagNode, node->m_CurrentLeafInputSignature, queue->m_Config.m_StatCache, &queue->m_Lock, thread_state);
    MutexLock(&queue->m_Lock);

    uint64_t now = TimerGet();
    double duration = TimerDiffSeconds(time_exec_started, now);
    char digestString[kDigestStringSize];
    DigestToString(digestString, node->m_CurrentLeafInputSignature);

    auto printMsg = [=](MessageStatusLevel::Enum statusLevel, const char* msg)
    {
        PrintMessage(statusLevel
                , duration
                , "%s [%s %s]"
                , node->m_DagNode->m_Annotation.Get()
                , msg
                , digestString);
    };

    switch (cacheReadResult)
    {
        case CacheResult::Failure:
            printMsg(MessageStatusLevel::Warning, "CacheRead");
            break;
        case CacheResult::Success:
            PrintCacheHit(queue, thread_state, duration, node);
            node->m_BuildResult = NodeBuildResult::kRanSuccesfully;
            FinishNode(queue, node);

            return true;

        case CacheResult::CacheMiss:
            PrintCacheMissIntoStructuredLog(thread_state,node);
            break;
    }

    return false;
}

void LogEnqueue(MemAllocLinear* scratch, RuntimeNode* enqueuedNode, RuntimeNode* enqueueingNode)
{
    MemAllocLinearScope allocScope(scratch);

    JsonWriter msg;
    JsonWriteInit(&msg, scratch);
    JsonWriteStartObject(&msg);

    JsonWriteKeyName(&msg, "msg");
    JsonWriteValueString(&msg, "enqueueNode");

    JsonWriteKeyName(&msg, "enqueuedNodeAnnotation");
    JsonWriteValueString(&msg, enqueuedNode->m_DagNode->m_Annotation);

    JsonWriteKeyName(&msg, "enqueuedNodeIndex");
    JsonWriteValueInteger(&msg, enqueuedNode->m_DagNode->m_OriginalIndex);

    if (enqueueingNode != nullptr)
    {
        JsonWriteKeyName(&msg, "enqueueingNodeAnnotation");
        JsonWriteValueString(&msg, enqueueingNode->m_DagNode->m_Annotation);

        JsonWriteKeyName(&msg, "enqueueingNodeIndex");
        JsonWriteValueInteger(&msg, enqueueingNode->m_DagNode->m_OriginalIndex);
    }
    JsonWriteEndObject(&msg);
    LogStructured(&msg);
}

static void EnqueueDependencies(BuildQueue *queue, ThreadState *thread_state, RuntimeNode *node)
{
    int enqueue_count = 0;

    for (int32_t depDagIndex : queue->m_Config.m_DagDerived->m_Dependencies[node->m_DagNodeIndex])
    {
        if (RuntimeNode *dependentNode = GetRuntimeNodeForDagNodeIndex(queue, depDagIndex))
        {
            // Did someone else get to the node first?
            if (RuntimeNodeHasEverBeenQueued(dependentNode))
                continue;

            LogEnqueue(&thread_state->m_ScratchAlloc, dependentNode, node);

            Enqueue(queue, dependentNode);
            ++enqueue_count;
        }
    }
    if (enqueue_count > 1)
        WakeWaiters(queue, enqueue_count-1);

    RuntimeNodeFlagInactive(node);
}

static void ProcessNode(BuildQueue *queue, ThreadState *thread_state, RuntimeNode *node, Mutex *queue_lock)
{
    Log(kSpam, "T=%d, Advancing %s\n", thread_state->m_ThreadIndex, node->m_DagNode->m_Annotation.Get());



    CHECK(!node->m_Finished);
    CHECK(RuntimeNodeIsActive(node));
    CHECK(!RuntimeNodeIsQueued(node));

    if (IsNodeCacheableByLeafInputs(node))
    {
        if (!RuntimeNodeHasAttemptedCacheLookup(node))
        {
            HashDigest currentLeafInputSignature = CalculateLeafInputSignature(queue, thread_state, node);
            node->m_CurrentLeafInputSignature = currentLeafInputSignature;
        }

        if (queue->m_Config.m_AttemptCacheReads)
            if (AttemptToMakeConsistentWithoutNeedingDependenciesBuilt(node, queue, thread_state))
                return;
    }

    if (!AllDependenciesAreFinished(queue,node))
    {
        EnqueueDependencies(queue,thread_state,node);
        return;
    }

    if (AllDependenciesAreSuccesful(queue, node))
    {
        MutexUnlock(queue_lock);
        NodeBuildResult::Enum nodeBuildResult = ExecuteNode(queue, node, queue_lock, thread_state, thread_state->m_Queue->m_Config.m_StatCache, queue->m_Config.m_DagDerived);
        MutexLock(queue_lock);

        switch (node->m_BuildResult = nodeBuildResult)
        {
            case NodeBuildResult::kRanFailed:
                queue->m_FinalBuildResult = BuildResult::kBuildError;
                SignalMainThreadToStartCleaningUp(queue);
                break;
            case NodeBuildResult::kRanSuccessButDependeesRequireFrontendRerun:
                if (queue->m_FinalBuildResult == BuildResult::kOk)
                    queue->m_FinalBuildResult = BuildResult::kRequireFrontendRerun;
                break;
            default:
                break;
        }
    }
    FinishNode(queue, node);
}

static RuntimeNode *NextNode(BuildQueue *queue)
{
    int avail_count = AvailableNodeCount(queue);

    if (0 == avail_count)
        return nullptr;

    uint32_t read_index = queue->m_QueueReadIndex;

    int32_t node_index = queue->m_Queue[read_index];

    // Update read index
    queue->m_QueueReadIndex = (read_index + 1) & (queue->m_QueueCapacity - 1);

    RuntimeNode *runtime_node = queue->m_Config.m_RuntimeNodes + node_index;

    CHECK(RuntimeNodeIsQueued(runtime_node));
    CHECK(!RuntimeNodeIsActive(runtime_node));

    RuntimeNodeFlagUnqueued(runtime_node);
    RuntimeNodeFlagActive(runtime_node);

    return runtime_node;
}

static bool ShouldKeepBuilding(BuildQueue *queue)
{
    return !queue->m_MainThreadWantsToCleanUp;
}

void BuildLoop(ThreadState *thread_state)
{
    BuildQueue *queue = thread_state->m_Queue;
    ConditionVariable *cv = &queue->m_WorkAvailable;
    Mutex *mutex = &queue->m_Lock;

    MutexLock(mutex);
    bool waitingForWork = false;

    auto HibernateForThrottlingIfRequired = [=]() {
        //check if dynamic max jobs amount has been reduced to a point where we need this thread to hibernate.
        //Don't take a mutex lock for this check, as this if check will almost never hit and it's in a perf critical loop.
        if (thread_state->m_ThreadIndex < (int)queue->m_DynamicMaxJobs)
            return false;

        ProfilerScope profiler_scope("HibernateForThrottling", thread_state->m_ProfilerThreadId, nullptr, "thread_state_sleeping");

        CondWait(&thread_state->m_Queue->m_MaxJobsChangedConditionalVariable, mutex);
        return true;
    };

    //This is the main build loop that build threads go through. The mutex/threading policy is that only one buildthread at a time actually goes through this loop
    //figures out what the next task is to do etc. When that thread has figured out what to do,  it will return the queue->m_Lock mutex while the job it has to execute
    //is executing. Another build thread can take its turn to pick up a new task at that point. In a sense it's a single threaded system, except that it happens on multiple threads :).
    //great care must be taken around the queue->m_Lock mutex though. You _have_ to hold it while you interact with the buildsystem datastructures, but you _cannot_ have it when
    //you go and do something that will take non trivial amount of time.

    //lock is taken here
    while (ShouldKeepBuilding(queue))
    {
        //if this function decides to hibernate, it will release the lock, and re-aquire it before it returns
        if (HibernateForThrottlingIfRequired())
            continue;

        if (RuntimeNode *node = NextNode(queue))
        {
            if (waitingForWork)
            {
                ProfilerEnd(thread_state->m_ProfilerThreadId);
                waitingForWork = false;
            }
            ProcessNode(queue, thread_state, node, mutex);
            continue;
        }

        //ok, there is nothing to do at this very moment, let's go to sleep.
        if (!waitingForWork)
        {
            ProfilerBegin("WaitingForWork", thread_state->m_ProfilerThreadId, nullptr, "thread_state_sleeping");
            waitingForWork = true;
        }

        //This API call will release our lock. The api contract is that this function will sleep until CV is triggered from another thread
        //and during that sleep the mutex will be released,  and before CondWait returns, the lock will be re-aquired
        CondWait(cv, mutex);
    }

    if (waitingForWork)
        ProfilerEnd(thread_state->m_ProfilerThreadId);

    MutexUnlock(mutex);
    {
        ProfilerScope profiler_scope("Exiting BuildLoop", thread_state->m_ProfilerThreadId);
        //add a tiny 10ms profiler entry at the end of a buildloop, to facilitate diagnosing when threads end in the json profiler.  This is not a per problem,
        //as it happens in parallel with the mainthread doing DestroyBuildQueue() which is always slower than this.
#if TUNDRA_WIN32
        Sleep(10);
#endif
    }

    Log(kSpam, "build thread %d exiting\n", thread_state->m_ThreadIndex);
}

