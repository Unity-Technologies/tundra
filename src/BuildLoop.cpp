#include "BuildQueue.hpp"
#include "DagData.hpp"
#include "MemAllocHeap.hpp"
#include "MemAllocLinear.hpp"
#include "RuntimeNode.hpp"
#include "Scanner.hpp"
#include "FileInfo.hpp"
#include "FileSystem.hpp"
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
#include "InputSignature.hpp"
#include "MakeDirectories.hpp"
#include "BuildLoop.hpp"
#include "RunAction.hpp"
#include "BuildQueue.hpp"
#include "Driver.hpp"
#include "LeafInputSignature.hpp"
#include "CacheClient.hpp"
#include "FileInfoHelper.hpp"
#include <stdarg.h>
#include <algorithm>
#include <stdio.h>

static RuntimeNode *GetRuntimeNodeForDagNodeIndex(BuildQueue *queue, int32_t src_index)
{
    return queue->m_Config.m_RuntimeNodes + src_index;
}

static void WakeWaiters(BuildQueue *queue, int count)
{
    if (count > 1)
        CondBroadcast(&queue->m_WorkAvailable);
    else
        CondSignal(&queue->m_WorkAvailable);
}


static void LogFirstTimeEnqueue(MemAllocLinear* scratch, RuntimeNode* enqueuedNode, RuntimeNode* enqueueingNode)
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

static void LogRunNodeAction(MemAllocLinear* scratch, RuntimeNode* node)
{
    MemAllocLinearScope allocScope(scratch);

    JsonWriter msg;
    JsonWriteInit(&msg, scratch);
    JsonWriteStartObject(&msg);

    JsonWriteKeyName(&msg, "msg");
    JsonWriteValueString(&msg, "runNodeAction");

    JsonWriteKeyName(&msg, "annotation");
    JsonWriteValueString(&msg, node->m_DagNode->m_Annotation);

    JsonWriteKeyName(&msg, "index");
    JsonWriteValueInteger(&msg, node->m_DagNode->m_OriginalIndex);

    JsonWriteEndObject(&msg);
    LogStructured(&msg);
}

static void LogFileSystemWaitUntilFileModificationDateIsInThePast(MemAllocLinear* scratch, const char* inputFile, RuntimeNode* node)
{
    MemAllocLinearScope allocScope(scratch);

    JsonWriter msg;
    JsonWriteInit(&msg, scratch);
    JsonWriteStartObject(&msg);

    JsonWriteKeyName(&msg, "msg");
    JsonWriteValueString(&msg, "fileSystemWaitUntilFileModificationDateIsInThePast");

    JsonWriteKeyName(&msg, "annotation");
    JsonWriteValueString(&msg, node->m_DagNode->m_Annotation);

    JsonWriteKeyName(&msg, "index");
    JsonWriteValueInteger(&msg, node->m_DagNode->m_OriginalIndex);

    JsonWriteKeyName(&msg, "inputfile");
    JsonWriteValueString(&msg, inputFile);

    JsonWriteEndObject(&msg);
    LogStructured(&msg);
}

static void LogNonGeneratedInputFileTimestampIsInTheFuture(MemAllocLinear* scratch, const char* inputFile, RuntimeNode* node)
{
    MemAllocLinearScope allocScope(scratch);

    JsonWriter msg;
    JsonWriteInit(&msg, scratch);
    JsonWriteStartObject(&msg);

    JsonWriteKeyName(&msg, "msg");
    JsonWriteValueString(&msg, "nonGeneratedInputFileTimestampIsInTheFuture");

    JsonWriteKeyName(&msg, "annotation");
    JsonWriteValueString(&msg, node->m_DagNode->m_Annotation);

    JsonWriteKeyName(&msg, "index");
    JsonWriteValueInteger(&msg, node->m_DagNode->m_OriginalIndex);

    JsonWriteKeyName(&msg, "inputfile");
    JsonWriteValueString(&msg, inputFile);

    JsonWriteEndObject(&msg);
    LogStructured(&msg);
}

static void LogModificationDateChangedDuringBuild(MemAllocLinear* scratch, const char* inputFile, RuntimeNode* node, uint64_t oldTimestamp, uint64_t newTimestamp)
{
    MemAllocLinearScope allocScope(scratch);

    JsonWriter msg;
    JsonWriteInit(&msg, scratch);
    JsonWriteStartObject(&msg);

    JsonWriteKeyName(&msg, "msg");
    JsonWriteValueString(&msg, "modificationDateChangedDuringBuild");

    JsonWriteKeyName(&msg, "annotation");
    JsonWriteValueString(&msg, node->m_DagNode->m_Annotation);

    JsonWriteKeyName(&msg, "index");
    JsonWriteValueInteger(&msg, node->m_DagNode->m_OriginalIndex);

    JsonWriteKeyName(&msg, "inputfile");
    JsonWriteValueString(&msg, inputFile);

    JsonWriteKeyName(&msg, "oldTimestamp");
    JsonWriteValueInteger(&msg, oldTimestamp);

    JsonWriteKeyName(&msg, "newTimestamp");
    JsonWriteValueInteger(&msg, newTimestamp);

    JsonWriteEndObject(&msg);
    LogStructured(&msg);
}

static int EnqueueNodeListWithoutWakingAwaiters(BuildQueue* queue, MemAllocLinear* scratch, const FrozenArray<int32_t>& nodesToEnqueue, RuntimeNode* enqueueingNode)
{
    int placed_on_workstack_count = 0;
    for(int32_t depDagIndex : nodesToEnqueue)
    {
        placed_on_workstack_count += EnqueueNodeWithoutWakingAwaiters(queue, scratch, &queue->m_Config.m_RuntimeNodes[depDagIndex], enqueueingNode);
    }
    return placed_on_workstack_count;
}

static bool IsNodeCacheableByLeafInputsAndCachingEnabled(BuildQueue* queue, RuntimeNode* node)
{
    if (!queue->m_Config.m_AttemptCacheReads && !queue->m_Config.m_AttemptCacheWrites)
        return false;
    return 0 != (node->m_DagNode->m_FlagsAndActionType & Frozen::DagNode::kFlagCacheableByLeafInputs);
}

static bool AllDependenciesAreFinished(BuildQueue *queue, RuntimeNode *runtime_node)
{
    for (int32_t dep_index : queue->m_Config.m_DagDerived->m_CombinedDependencies[runtime_node->m_DagNodeIndex])
    {
        RuntimeNode *runtime_node = GetRuntimeNodeForDagNodeIndex(queue, dep_index);
        if (!runtime_node->m_Finished)
            return false;
    }
    return true;
}

static bool AllDependenciesAreSuccesful(BuildQueue *queue, RuntimeNode *runtime_node)
{
    for (int32_t dep_index : queue->m_Config.m_DagDerived->m_CombinedDependencies[runtime_node->m_DagNodeIndex])
    {
        RuntimeNode *runtime_node = GetRuntimeNodeForDagNodeIndex(queue, dep_index);
        CHECK(runtime_node->m_Finished);

        if (runtime_node->m_BuildResult != NodeBuildResult::kRanSuccesfully && runtime_node->m_BuildResult != NodeBuildResult::kUpToDate)
            return false;
    }
    return true;
}

static bool AddNodeToWorkStackIfNotAlreadyPresent(BuildQueue* queue, RuntimeNode* runtime_node)
{
    int runtime_node_index = int(runtime_node - queue->m_Config.m_RuntimeNodes);
    return BufferAppendOneIfNotPresent(&queue->m_WorkStack, queue->m_Config.m_Heap, runtime_node_index);
}

int EnqueueNodeWithoutWakingAwaiters(BuildQueue *queue, MemAllocLinear* scratch, RuntimeNode *runtime_node, RuntimeNode* queueing_node)
{
    if (RuntimeNodeHasEverBeenQueued(runtime_node))
        return 0;

    LogFirstTimeEnqueue(scratch, runtime_node, queueing_node);
    queue->m_AmountOfNodesEverQueued++;
    RuntimeNodeFlagQueued(runtime_node);

    //enqueueing a node means that we know we need it to complete our build. Some nodes
    //we know can be processed immediately:
    //1) those whose dependencies have all been completed,
    //2) those who are marked as leaf input cacheable && leaf input caching is turned on.
    //
    //we will only put nodes from these two categories on the workstack. Any other nodes
    //we will only mark them as queued, but we don't actually put them on the workstack, since we already
    //know they cannot yet immediately be acted upon. They will be put on the workstack when their dependencies finish.
    int placed_on_workstack_count = 0;
    if (AllDependenciesAreFinished(queue,runtime_node) || IsNodeCacheableByLeafInputsAndCachingEnabled(queue,runtime_node))
    {
        if (AddNodeToWorkStackIfNotAlreadyPresent(queue, runtime_node))
            placed_on_workstack_count++;
    } else {
        //ok, so in this case the queued node is not immediately actionable. so we don't put it on the workstack, but instead queue up all our tobuild dependencies,
        //so that our node becomes immediately actionable in the future. We don't blindly always do this, because in the case where this node is leaf input cacheable
        //we might get a cache hit, and it won't be necessary at all to build any of the dependencies.
        placed_on_workstack_count += EnqueueNodeListWithoutWakingAwaiters(queue, scratch, runtime_node->m_DagNode->m_ToBuildDependencies, runtime_node);
    }

    //leaf input cache hit or not: we always need the toUseDependencies to be produced.
    placed_on_workstack_count += EnqueueNodeListWithoutWakingAwaiters(queue, scratch, runtime_node->m_DagNode->m_ToUseDependencies, runtime_node);

    return placed_on_workstack_count;
}


//many operations add nodes to the working stack.  They just append the nodes at the end, not caring about sorting.
//after all adds have been done, they're supposed to call SortWorkingStack() once at the end which will make sure to sort
//the nodes based on how many points each nodes has. Nodes with more points should be preferred to start sooner than nodes with
//less points if we have a choice to make. Today the amount of points are baked into the dag data, and calculated by seeing
//how many other nodes end up directly or indirectly depending on this node. Maybe in the future we'll expose a bee setting where
//you can manually specify points per node, but right now this seems to lead to optimal scheduling for most buildgraphs, so good enough for now.
void SortWorkingStack(BuildQueue* queue)
{
    const auto& nodePoints = queue->m_Config.m_DagDerived->m_NodePoints;
    //we want to have the nodes with the highest amount of points at the end, since that's where they'll be popped from
    std::sort(queue->m_WorkStack.begin(), queue->m_WorkStack.end(), [&](int nodeIndexA, int nodeIndexB)
    {
        return nodePoints[nodeIndexA] < nodePoints[nodeIndexB];
    });
}

static void EnqueueDependeesWhoMightNowHaveBecomeReadyToRun(BuildQueue *queue, ThreadState* thread_state, RuntimeNode *node)
{
    int placed_on_workstack_count = 0;

    const FrozenArray<uint32_t>& backLinks = queue->m_Config.m_DagDerived->m_NodeBacklinks[node->m_DagNodeIndex];

    for (int32_t link : backLinks)
    {
        if (RuntimeNode *waiter = GetRuntimeNodeForDagNodeIndex(queue, link))
        {
            //we should only enqueue nodes that depend on us that we are actually trying to build
            if (!RuntimeNodeHasEverBeenQueued(waiter))
                continue;

            // If the node isn't ready, skip it.
            if (!AllDependenciesAreFinished(queue, waiter))
                continue;

            if (AddNodeToWorkStackIfNotAlreadyPresent(queue,waiter))
                placed_on_workstack_count++;
        }
    }

    if (placed_on_workstack_count > 0)
        SortWorkingStack(queue);

    //if we're enqueing only one thing, this thread will immediately pick that up in the next loop iteration.
    //if we're enqueing more than one node, let's wake up enough threads so that they can be immediately picked up
    if (placed_on_workstack_count > 1)
        WakeWaiters(queue, placed_on_workstack_count-1);
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

static void FinishNode(BuildQueue* queue, ThreadState* thread_state, RuntimeNode* node)
{
    CheckHasLock(&queue->m_Lock);

    node->m_Finished = true;
    queue->m_FinishedNodeCount++;

    RuntimeNodeFlagInactive(node);
    if (queue->m_FinishedNodeCount == queue->m_AmountOfNodesEverQueued)
        SignalMainThreadToStartCleaningUp(queue);

    EnqueueDependeesWhoMightNowHaveBecomeReadyToRun(queue, thread_state, node);
}

static void AttemptCacheWrite(BuildQueue* queue, ThreadState* thread_state, RuntimeNode* node)
{
    CheckDoesNotHaveLock(&queue->m_Lock);

    uint64_t time_exec_started = TimerGet();

    char digestString[kDigestStringSize];
    DigestToString(digestString, node->m_CurrentLeafInputSignature->digest);
    FILE *sig = fopen(digestString, "w");
    if (sig == NULL)
    {
        printf("Failed to open file for signature ingredient writing. Skipping CacheWrite.\n");
        return;
    }

    //we already calculated the leaf input signature before, but we'll do it again because now we want to have the ingredient stream written out to disk.
    CalculateLeafInputSignature(queue, node->m_DagNode, node, &thread_state->m_ScratchAlloc, thread_state->m_ProfilerThreadId, sig);

    fclose(sig);

    auto writeResult = CacheClient::AttemptWrite(queue->m_Config.m_Dag, node->m_DagNode, node->m_CurrentLeafInputSignature->digest, queue->m_Config.m_StatCache, &queue->m_Lock, thread_state, digestString);
    remove(digestString);

    uint64_t now = TimerGet();
    double duration = TimerDiffSeconds(time_exec_started, now);

    MutexLock(&queue->m_Lock);
    PrintMessage(writeResult == CacheResult::Success ? MessageStatusLevel::Success : MessageStatusLevel::Warning, duration, "%s [CacheWrite %s]", node->m_DagNode->m_Annotation.Get(), digestString);
    MutexUnlock(&queue->m_Lock);
}

static void StoreTimestampsOfNonGeneratedInputFiles(Buffer<uint64_t>& timeStampStorage, MemAllocHeap* timestampStorageHeap, BuildQueue* queue, RuntimeNode* node, uint64_t* latestTimestampSeenForNonGeneratedInputFile = nullptr, const char** nonGeneratedInputFileWithTimestamp = nullptr)
{
    auto& nonGeneratedInputIndices = queue->m_Config.m_DagDerived->m_NodeNonGeneratedInputIndicies[node->m_DagNodeIndex];
    BufferClear(&timeStampStorage);
    BufferAlloc(&timeStampStorage, timestampStorageHeap, nonGeneratedInputIndices.GetCount());
    
    for (int i = 0; i < nonGeneratedInputIndices.GetCount(); i++)
    {
        int inputIndex = nonGeneratedInputIndices[i];
        auto &non_generated_input_file = node->m_DagNode->m_InputFiles[inputIndex];

        uint64_t timeStamp = StatCacheStat(queue->m_Config.m_StatCache, non_generated_input_file.m_Filename, non_generated_input_file.m_FilenameHash).m_Timestamp;
        timeStampStorage[i] = timeStamp;
        if (latestTimestampSeenForNonGeneratedInputFile && timeStamp > *latestTimestampSeenForNonGeneratedInputFile)
        {
            *nonGeneratedInputFileWithTimestamp = non_generated_input_file.m_Filename.Get();
            *latestTimestampSeenForNonGeneratedInputFile = timeStamp;
        }
    }
};

static bool ValidateTimestampsOfNonGeneratedInputFiles(const Buffer<uint64_t>& timeStampStorage, BuildQueue* queue, RuntimeNode* node, const char** out_fileWhoseModificationDateChangedDuringBuild, uint64_t* out_oldTimestamp, uint64_t* out_newTimestamp)
{
    auto& nonGeneratedInputIndices = queue->m_Config.m_DagDerived->m_NodeNonGeneratedInputIndicies[node->m_DagNodeIndex];
    for (int i = 0; i < nonGeneratedInputIndices.GetCount(); i++)
    {
        int inputIndex = nonGeneratedInputIndices[i];
        auto &non_generated_input_file = node->m_DagNode->m_InputFiles[inputIndex];
        StatCacheMarkDirty(queue->m_Config.m_StatCache, non_generated_input_file.m_Filename, non_generated_input_file.m_FilenameHash);

        uint64_t timestamp = StatCacheStat(queue->m_Config.m_StatCache, non_generated_input_file.m_Filename, non_generated_input_file.m_FilenameHash).m_Timestamp;
        uint64_t oldTimestamp = timeStampStorage[i];
        if (oldTimestamp != timestamp)
        {
            *out_fileWhoseModificationDateChangedDuringBuild = non_generated_input_file.m_Filename.Get();
            *out_newTimestamp = timestamp;
            *out_oldTimestamp = oldTimestamp;
            return false;
        }
    }

    return true;
};

static bool AreNodeFileAndGlobSignaturesStillValid(RuntimeNode* node, ThreadState* thread_state)
{
    auto VerifyNodeGlobSignatures = [=]() -> bool {
        for (const Frozen::DagGlobSignature &sig : node->m_DagNode->m_GlobSignatures)
        {
            HashDigest digest = CalculateGlobSignatureFor(sig.m_Path, sig.m_Filter, sig.m_Recurse, thread_state->m_Queue->m_Config.m_Heap, &thread_state->m_ScratchAlloc);

            // Compare digest with the one stored in the signature block
            if (0 != memcmp(&digest, &sig.m_Digest, sizeof digest))
            {
                thread_state->m_GlobCausingFrontendRerun = &sig;
                return false;
            }
        }
        return true;
    };

    auto VerifyNodeStatSignatures = [=]() -> bool {
        // Check timestamps of frontend files used to produce the DAG
        for (const Frozen::DagStatSignature &sig : node->m_DagNode->m_StatSignatures)
        {
            const char *path = sig.m_Path;
            FileInfo info = GetFileInfo(path);

            Frozen::DagStatSignature::Enum value = GetStatSignatureStatusFor(info);
            if (value != sig.m_StatResult)
            {
                thread_state->m_FileCausingFrontendRerun = &sig.m_Path;
                return false;
            }
        }
        return true;
    };

    auto VerifyNodeFileSignatures = [=]() -> bool {
        // Check timestamps of frontend files used to produce the DAG
        for (const Frozen::DagFileSignature &sig : node->m_DagNode->m_FileSignatures)
        {
            const char *path = sig.m_Path;

            uint64_t timestamp = sig.m_Timestamp;
            FileInfo info = GetFileInfo(path);

            if (info.m_Timestamp != timestamp)
            {
                thread_state->m_FileCausingFrontendRerun = &sig.m_Path;
                return false;
            }
        }
        return true;
    };

    return VerifyNodeGlobSignatures() && VerifyNodeFileSignatures() && VerifyNodeStatSignatures();
}

static NodeBuildResult::Enum ExecuteNode(BuildQueue* queue, RuntimeNode* node, Mutex *queue_lock, ThreadState* thread_state, StatCache* stat_cache, const Frozen::DagDerived* dagDerived)
{
    CheckDoesNotHaveLock(&queue->m_Lock);

    // Compute timestamps and record the latest file modification date we find for any input file not generated by the graph.
    uint64_t latestTimestampSeenForNonGeneratedInputFile = 0;
    const char* nonGeneratedInputFileWithTimestamp = nullptr;
    StoreTimestampsOfNonGeneratedInputFiles(thread_state->m_TimestampStorage, &thread_state->m_LocalHeap, queue, node, &latestTimestampSeenForNonGeneratedInputFile, &nonGeneratedInputFileWithTimestamp);

    // Check latest file timestamp against filesystem "now"
    bool thereIsAtLeastOneInputFileDatedInTheFuture = false;
    if (latestTimestampSeenForNonGeneratedInputFile >= FileSystem::g_LastSeenFileSystemTime)
    {
        // Make sure we are in sync with current file system "now"
        auto fileSystemTimeNow = FileSystemUpdateLastSeenFileSystemTime();
        if (latestTimestampSeenForNonGeneratedInputFile == fileSystemTimeNow)
        {
            // If the latest modification time of any non generated input file matches now, then we wait for the next file system mtime tick.
            // The reason we do this is so we can safely detect changes done by the user either during graph execution or in between
            // two tundra executions happening within the same file system mtime frame.
            MutexLock(&queue->m_Lock);
            PrintMessage(MessageStatusLevel::Info, "Waiting until the timestamp of `%s` is in the past.", nonGeneratedInputFileWithTimestamp);
            LogFileSystemWaitUntilFileModificationDateIsInThePast(&thread_state->m_ScratchAlloc, nonGeneratedInputFileWithTimestamp, node);
            MutexUnlock(&queue->m_Lock);

            FileSystemWaitUntilFileModificationDateIsInThePast(latestTimestampSeenForNonGeneratedInputFile);
        }
        else if (latestTimestampSeenForNonGeneratedInputFile > fileSystemTimeNow)
        {
            // If any file not generated by the graph is dated in the future we can't wait. Or we don't want to wait, since that could potentially
            // lock up tundra for a very long time. Instead we record this specific state and flag input signature as "might be incorrect".
            MutexLock(&queue->m_Lock);
            PrintMessage(MessageStatusLevel::Info, "Cannot trust contents of `%s` because its timestamp is in the future.", nonGeneratedInputFileWithTimestamp);
            LogNonGeneratedInputFileTimestampIsInTheFuture(&thread_state->m_ScratchAlloc, nonGeneratedInputFileWithTimestamp, node);
            MutexUnlock(&queue->m_Lock);

            thereIsAtLeastOneInputFileDatedInTheFuture = true;
        }
    }

    bool haveToRunAction = CheckInputSignatureToSeeNodeNeedsExecuting(queue, thread_state, node);
    if (!haveToRunAction)
    {
        return AreNodeFileAndGlobSignaturesStillValid(node, thread_state)
            ? NodeBuildResult::kUpToDate
            : NodeBuildResult::kUpToDateButDependeesRequireFrontendRerun;
    }

    LogRunNodeAction(&thread_state->m_ScratchAlloc, node);

    NodeBuildResult::Enum runActionResult = RunAction(queue, thread_state, node, queue_lock);

    if (runActionResult == NodeBuildResult::kRanSuccesfully && !AreNodeFileAndGlobSignaturesStillValid(node,thread_state))
        runActionResult = NodeBuildResult::kRanSuccessButDependeesRequireFrontendRerun;

    // If we see any file not generated by the graph dated in the future we will always treat input signature as incorrect.
    // Meaning for every build we will rebuild this node until filesystem mtime catches up with the file mtime date.
    if (thereIsAtLeastOneInputFileDatedInTheFuture)
    {
        RuntimeNodeSetInputSignatureMightBeIncorrect(node);
        return runActionResult;
    }

    // If signatures don't match, someone touched files on disk while we were checking input signature or action was executing.
    const char* fileWhoseModificationDateChangedDuringBuild = nullptr;
    uint64_t oldTimestamp, newTimestamp;
    if (!ValidateTimestampsOfNonGeneratedInputFiles(thread_state->m_TimestampStorage, queue, node, &fileWhoseModificationDateChangedDuringBuild, &oldTimestamp, &newTimestamp))
    {
        MutexLock(&queue->m_Lock);
        PrintMessage(MessageStatusLevel::Info, "Modification date of `%s` changed while running `%s`. Old timestamp: %llu, new timestamp: %llu", fileWhoseModificationDateChangedDuringBuild, node->m_DagNode->m_Annotation.Get(), oldTimestamp, newTimestamp);
        LogModificationDateChangedDuringBuild(&thread_state->m_ScratchAlloc, nonGeneratedInputFileWithTimestamp, node, oldTimestamp, newTimestamp);
        MutexUnlock(&queue->m_Lock);

        RuntimeNodeSetInputSignatureMightBeIncorrect(node);
        return runActionResult;
    }

    if (runActionResult == NodeBuildResult::kRanSuccesfully
        && queue->m_Config.m_AttemptCacheWrites
        && IsNodeCacheableByLeafInputsAndCachingEnabled(queue,node))
    {
        CHECK(node->m_CurrentLeafInputSignature != nullptr);
        if (!VerifyAllVersionedFilesIncludedByGeneratedHeaderFilesWereAlreadyPartOfTheLeafInputs(queue, thread_state, node, dagDerived))
            return NodeBuildResult::kRanFailed;

        AttemptCacheWrite(queue,thread_state,node);
    }

    return runActionResult;
}

static bool AttemptToMakeConsistentWithoutNeedingDependenciesBuilt(RuntimeNode* node, BuildQueue* queue, ThreadState* thread_state)
{
    CheckHasLock(&queue->m_Lock);

    if (node->m_BuiltNode)
    {
        auto wasSuccessfulWithGuaranteedCorrectInputSignature = node->m_BuiltNode->m_Result == Frozen::BuiltNodeResult::kRanSuccessfullyWithGuaranteedCorrectInputSignature;
        if (wasSuccessfulWithGuaranteedCorrectInputSignature && node->m_BuiltNode->m_LeafInputSignature == node->m_CurrentLeafInputSignature->digest && !OutputFilesMissingFor(node->m_BuiltNode, queue->m_Config.m_StatCache))
        {
            node->m_BuildResult = NodeBuildResult::kUpToDate;
            FinishNode(queue, thread_state, node);
            return true;
        }
    }

    RuntimeNodeSetAttemptedCacheLookup(node);

    uint64_t time_exec_started = TimerGet();
    MutexUnlock(&queue->m_Lock);
    auto cacheReadResult = CacheClient::AttemptRead(queue->m_Config.m_Dag, node->m_DagNode, node->m_CurrentLeafInputSignature->digest, queue->m_Config.m_StatCache, &queue->m_Lock, thread_state);
    MutexLock(&queue->m_Lock);

    uint64_t now = TimerGet();
    double duration = TimerDiffSeconds(time_exec_started, now);
    char digestString[kDigestStringSize];
    DigestToString(digestString, node->m_CurrentLeafInputSignature->digest);

    switch (cacheReadResult)
    {
        case CacheResult::DidNotTry:
            break;

        case CacheResult::Failure:
            PrintMessage(MessageStatusLevel::Warning, duration, "%s [CacheRead %s]", node->m_DagNode->m_Annotation.Get(), digestString);
            break;

        case CacheResult::Success:
            PostRunActionBookkeeping(node, thread_state);
            PrintCacheHit(queue, thread_state, duration, node);
            node->m_BuildResult = NodeBuildResult::kRanSuccesfully;
            FinishNode(queue, thread_state, node);
            return true;

        case CacheResult::CacheMiss:
            PrintCacheMissIntoStructuredLog(thread_state,node);
            break;

        default:
            Croak("Unexpected cache read result %d", cacheReadResult);
    }

    return false;
}

static void EnqueueToBuildDependencies(BuildQueue *queue, ThreadState *thread_state, RuntimeNode *node)
{
    CheckHasLock(&queue->m_Lock);

    auto& dependencies = node->m_DagNode->m_ToBuildDependencies;
    int placed_on_workstack_count = EnqueueNodeListWithoutWakingAwaiters(queue,&thread_state->m_ScratchAlloc, dependencies, node);

    if (placed_on_workstack_count > 0)
        SortWorkingStack(queue);

    if (placed_on_workstack_count > 1)
        WakeWaiters(queue, placed_on_workstack_count-1);
}

static void LogOutOfDateSignaturePath(RuntimeNode* node, const char* signaturePath, MemAllocLinear* scratch)
{
    MemAllocLinearScope allocScope(scratch);

    JsonWriter msg;
    JsonWriteInit(&msg, scratch);
    JsonWriteStartObject(&msg);

    JsonWriteKeyName(&msg, "msg");
    JsonWriteValueString(&msg, "outofdatesignature");

    JsonWriteKeyName(&msg, "annotation");
    JsonWriteValueString(&msg, node->m_DagNode->m_Annotation);

    JsonWriteKeyName(&msg, "path");
    JsonWriteValueString(&msg, signaturePath);

    JsonWriteEndObject(&msg);
    LogStructured(&msg);
}

static void ProcessNode(BuildQueue *queue, ThreadState *thread_state, RuntimeNode *node, Mutex *queue_lock)
{
    CheckHasLock(&queue->m_Lock);

    Log(kSpam, "T=%d, Advancing %s\n", thread_state->m_ThreadIndex, node->m_DagNode->m_Annotation.Get());

    CHECK(!node->m_Finished);
    CHECK(RuntimeNodeIsActive(node));
    CHECK(!RuntimeNodeIsQueued(node));

    if (IsNodeCacheableByLeafInputsAndCachingEnabled(queue,node))
    {
        if (!RuntimeNodeHasAttemptedCacheLookup(node))
        {
            // Maybe the node's signature was already calculated as part of a parent's signature, then we can skip.
            if (node->m_CurrentLeafInputSignature == nullptr)
            {
                MutexUnlock(queue_lock);
                CalculateLeafInputSignature(queue, node->m_DagNode, node, &thread_state->m_ScratchAlloc, thread_state->m_ProfilerThreadId, nullptr);
                MutexLock(queue_lock);
            }

            if (queue->m_Config.m_AttemptCacheReads)
                if (AttemptToMakeConsistentWithoutNeedingDependenciesBuilt(node, queue, thread_state))
                    return;
        }
    }

    if (!AllDependenciesAreFinished(queue,node))
    {
        EnqueueToBuildDependencies(queue,thread_state,node);
        RuntimeNodeFlagInactive(node);
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
                if (!queue->m_Config.m_DriverOptions->m_ContinueOnFailure)
                    SignalMainThreadToStartCleaningUp(queue);
                break;
            case NodeBuildResult::kRanSuccessButDependeesRequireFrontendRerun:
            case NodeBuildResult::kUpToDateButDependeesRequireFrontendRerun:
                if (queue->m_FinalBuildResult == BuildResult::kOk)
                {
                    queue->m_FinalBuildResult = BuildResult::kRequireFrontendRerun;
                    if (thread_state->m_GlobCausingFrontendRerun)
                        LogOutOfDateSignaturePath(node, thread_state->m_GlobCausingFrontendRerun->m_Path.Get(), &thread_state->m_ScratchAlloc);
                    if (thread_state->m_FileCausingFrontendRerun)
                        LogOutOfDateSignaturePath(node, thread_state->m_FileCausingFrontendRerun->Get(), &thread_state->m_ScratchAlloc);
                }
                break;
            default:
                break;
        }
    }
    FinishNode(queue, thread_state, node);
}

static RuntimeNode *NextNode(BuildQueue *queue)
{
    CheckHasLock(&queue->m_Lock);

    Buffer<int32_t>* workStack = &queue->m_WorkStack;

    while(workStack->GetCount() > 0)
    {
        int32_t node_index = BufferPopOne(workStack);

        RuntimeNode *runtime_node = queue->m_Config.m_RuntimeNodes + node_index;

        if (RuntimeNodeIsActive(runtime_node) || runtime_node->m_Finished)
        {
            //this can happen in legit situations. we allow nodes to appear on the workstack more than once. This happens in situations where
            //a node gets queued as not-very-urgent (aka at the end of a dependency list). But later, the same node is also a dependency of something
            //that was queued at the top of the stack. In this case, we enqueue it again, ensuring it gets processed as fast as possible. We do not
            //bother deleting the older entry from the workstack, and instead have this check & continue.
            continue;
        }
        CHECK(RuntimeNodeIsQueued(runtime_node));

        RuntimeNodeFlagUnqueued(runtime_node);
        RuntimeNodeFlagActive(runtime_node);
        return runtime_node;
    }
    return nullptr;
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

    //This is the main build loop that build threads go through. The mutex/threading policy is that only one buildthread at a time actually goes through this loop
    //figures out what the next task is to do etc. When that thread has figured out what to do,  it will return the queue->m_Lock mutex while the job it has to execute
    //is executing. Another build thread can take its turn to pick up a new task at that point. In a sense it's a single threaded system, except that it happens on multiple threads :).
    //great care must be taken around the queue->m_Lock mutex though. You _have_ to hold it while you interact with the buildsystem datastructures, but you _cannot_ have it when
    //you go and do something that will take non trivial amount of time.

    //lock is taken here
    while (ShouldKeepBuilding(queue))
    {
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

