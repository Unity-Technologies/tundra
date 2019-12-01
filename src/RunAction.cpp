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
#include <stdarg.h>
#include <algorithm>
#include <stdio.h>



struct SlowCallbackData
{
    Mutex *queue_lock;
    const Frozen::DagNode *node_data;
    uint64_t time_of_start;
    const BuildQueue *build_queue;
};

static int SlowCallback(void *user_data)
{
    SlowCallbackData *data = (SlowCallbackData *)user_data;
    MutexLock(data->queue_lock);
    int sendNextCallbackIn = PrintNodeInProgress(data->node_data, data->time_of_start, data->build_queue);
    MutexUnlock(data->queue_lock);
    return sendNextCallbackIn;
}

static ExecResult WriteTextFile(const char *payload, const char *target_file, MemAllocHeap *heap)
{
    ExecResult result;
    char tmpBuffer[1024];

    memset(&result, 0, sizeof(result));

    FILE *f = fopen(target_file, "wb");
    if (!f)
    {
        InitOutputBuffer(&result.m_OutputBuffer, heap);

        snprintf(tmpBuffer, sizeof(tmpBuffer), "Error opening for writing the file: %s, error: %s", target_file, strerror(errno));
        EmitOutputBytesToDestination(&result, tmpBuffer, strlen(tmpBuffer));

        result.m_ReturnCode = 1;
        return result;
    }
    int length = strlen(payload);
    int written = fwrite(payload, sizeof(char), length, f);
    fclose(f);

    if (written == length)
        return result;

    InitOutputBuffer(&result.m_OutputBuffer, heap);

    snprintf(tmpBuffer, sizeof(tmpBuffer), "fwrite was supposed to write %d bytes to %s, but wrote %d bytes", length, target_file, written);
    EmitOutputBytesToDestination(&result, tmpBuffer, strlen(tmpBuffer));

    result.m_ReturnCode = 1;
    return result;
}

NodeBuildResult::Enum RunAction(BuildQueue *queue, ThreadState *thread_state, RuntimeNode *node, Mutex *queue_lock)
{
    const Frozen::DagNode *node_data = node->m_DagNode;
    const bool isWriteFileAction = node->m_DagNode->m_Flags & Frozen::DagNode::kFlagIsWriteTextFileAction;
    const char *cmd_line = node_data->m_Action;

    if (!isWriteFileAction && (!cmd_line || cmd_line[0] == '\0'))
        return NodeBuildResult::kRanSuccesfully;

    StatCache *stat_cache = queue->m_Config.m_StatCache;
    const char *annotation = node_data->m_Annotation;
    int job_id = thread_state->m_ThreadIndex;
    int profiler_thread_id = thread_state->m_ProfilerThreadId;
    bool echo_cmdline = 0 != (queue->m_Config.m_Flags & BuildQueueConfig::kFlagEchoCommandLines);
    const char *last_cmd_line = nullptr;
    // Repack frozen env to pointers on the stack.
    int env_count = node_data->m_EnvVars.GetCount();
    EnvVariable *env_vars = (EnvVariable *)alloca(env_count * sizeof(EnvVariable));
    for (int i = 0; i < env_count; ++i)
    {
        env_vars[i].m_Name = node_data->m_EnvVars[i].m_Name;
        env_vars[i].m_Value = node_data->m_EnvVars[i].m_Value;
    }

    for (int i = 0; i < node_data->m_SharedResources.GetCount(); ++i)
    {
        if (!SharedResourceAcquire(queue, &thread_state->m_LocalHeap, node_data->m_SharedResources[i]))
        {
            Log(kError, "failed to create shared resource %s", queue->m_Config.m_SharedResources[node_data->m_SharedResources[i]].m_Annotation.Get());
            MutexLock(queue_lock);
            return NodeBuildResult::kRanFailed;
        }
    }

    auto EnsureParentDirExistsFor = [=](const FrozenFileAndHash &fileAndHash) -> bool {
        PathBuffer output;
        PathInit(&output, fileAndHash.m_Filename);

        if (!MakeDirectoriesForFile(stat_cache, output))
        {
            Log(kError, "failed to create output directories for %s", fileAndHash.m_Filename.Get());
            return false;
        }
        return true;
    };

    for (const FrozenFileAndHash &output_file : node_data->m_AuxOutputFiles)
        if (!EnsureParentDirExistsFor(output_file))
            return NodeBuildResult::kRanFailed;

    for (const FrozenFileAndHash &output_dir : node_data->m_OutputDirectories)
    {
        PathBuffer path;
        PathInit(&path, output_dir.m_Filename);
        if (!MakeDirectoriesRecursive(stat_cache, path))
            return NodeBuildResult::kRanFailed;
    }

    for (const FrozenFileAndHash &output_file : node_data->m_OutputFiles)
        if (!EnsureParentDirExistsFor(output_file))
            return NodeBuildResult::kRanFailed;

    ExecResult result = {0, false};

    // See if we need to remove the output files before running anything.
    if (0 == (node_data->m_Flags & Frozen::DagNode::kFlagOverwriteOutputs))
    {
        for (const FrozenFileAndHash &output : node_data->m_OutputFiles)
        {
            Log(kDebug, "Removing output file %s before running action", output.m_Filename.Get());
            remove(output.m_Filename);
            StatCacheMarkDirty(stat_cache, output.m_Filename, output.m_FilenameHash);
        }
    }

    uint64_t time_of_start = TimerGet();

    SlowCallbackData slowCallbackData;
    slowCallbackData.node_data = node_data;
    slowCallbackData.time_of_start = time_of_start;
    slowCallbackData.queue_lock = queue_lock;
    slowCallbackData.build_queue = thread_state->m_Queue;

    size_t n_outputs = (size_t)node_data->m_OutputFiles.GetCount();

    bool *untouched_outputs = (bool *)LinearAllocate(&thread_state->m_ScratchAlloc, n_outputs, (size_t)sizeof(bool));
    memset(untouched_outputs, 0, n_outputs * sizeof(bool));

    bool requireFrontendRerun = false;

    ValidationResult passedOutputValidation = ValidationResult::Pass;
    if (0 == result.m_ReturnCode)
    {
        Log(kSpam, "Launching process");
        TimingScope timing_scope(&g_Stats.m_ExecCount, &g_Stats.m_ExecTimeCycles);
        ProfilerScope prof_scope(annotation, profiler_thread_id);

        uint64_t *pre_timestamps = (uint64_t *)LinearAllocate(&thread_state->m_ScratchAlloc, n_outputs, (size_t)sizeof(uint64_t));

        bool allowUnwrittenOutputFiles = (node_data->m_Flags & Frozen::DagNode::kFlagAllowUnwrittenOutputFiles);
        if (!allowUnwrittenOutputFiles)
            for (int i = 0; i < n_outputs; i++)
            {
                FileInfo info = GetFileInfo(node_data->m_OutputFiles[i].m_Filename);
                pre_timestamps[i] = info.m_Timestamp;
            }

        if (isWriteFileAction)
            result = WriteTextFile(node_data->m_Action, node_data->m_OutputFiles[0].m_Filename, thread_state->m_Queue->m_Config.m_Heap);
        else
        {
            last_cmd_line = cmd_line;
            result = ExecuteProcess(cmd_line, env_count, env_vars, thread_state->m_Queue->m_Config.m_Heap, job_id, false, SlowCallback, &slowCallbackData);
            passedOutputValidation = ValidateExecResultAgainstAllowedOutput(&result, node_data);
        }

        if (passedOutputValidation == ValidationResult::Pass && !allowUnwrittenOutputFiles)
        {
            for (int i = 0; i < n_outputs; i++)
            {
                FileInfo info = GetFileInfo(node_data->m_OutputFiles[i].m_Filename);
                bool untouched = pre_timestamps[i] == info.m_Timestamp;
                untouched_outputs[i] = untouched;
                if (untouched)
                    passedOutputValidation = ValidationResult::UnwrittenOutputFileFail;
            }
        }

        auto VerifyNodeGlobSignatures = [=]() -> bool {
            for (const Frozen::DagGlobSignature &sig : node->m_DagNode->m_GlobSignatures)
            {
                HashDigest digest = CalculateGlobSignatureFor(sig.m_Path, sig.m_Filter, sig.m_Recurse, thread_state->m_Queue->m_Config.m_Heap, &thread_state->m_ScratchAlloc);

                // Compare digest with the one stored in the signature block
                if (0 != memcmp(&digest, &sig.m_Digest, sizeof digest))
                    return false;
            }
            return true;
        };

        auto VerifyFileSignatures = [=]() -> bool {
            // Check timestamps of frontend files used to produce the DAG
            for (const Frozen::DagFileSignature &sig : node->m_DagNode->m_FileSignatures)
            {
                const char *path = sig.m_Path;

                uint64_t timestamp = sig.m_Timestamp;
                FileInfo info = GetFileInfo(path);

                if (info.m_Timestamp != timestamp)
                    return false;
            }
            return true;
        };

        if (!VerifyNodeGlobSignatures())
            requireFrontendRerun = true;
        if (!VerifyFileSignatures())
            requireFrontendRerun = true;

        Log(kSpam, "Process return code %d", result.m_ReturnCode);
    }

    for (const FrozenFileAndHash &output : node_data->m_OutputFiles)
    {
        StatCacheMarkDirty(stat_cache, output.m_Filename, output.m_FilenameHash);
    }

    //maybe consider changing this to use a dedicated lock for printing, instead of using the queuelock.
    MutexLock(queue_lock);
    PrintNodeResult(&result, node_data, last_cmd_line, thread_state->m_Queue, thread_state, echo_cmdline, time_of_start, passedOutputValidation, untouched_outputs);
    MutexUnlock(queue_lock);

    ExecResultFreeMemory(&result);

    if (result.m_WasAborted)
    {
        SignalSet("child processes was aborted");
    }

    if (0 == result.m_ReturnCode && passedOutputValidation < ValidationResult::UnexpectedConsoleOutputFail)
        return requireFrontendRerun ? NodeBuildResult::kRanSuccessButDependeesRequireFrontendRerun : NodeBuildResult::kRanSuccesfully;

    // Clean up output files after a failed build unless they are precious,
    // or unless the failure was from failing to write one of them
    if (0 == (Frozen::DagNode::kFlagPreciousOutputs & node_data->m_Flags) && !(0 == result.m_ReturnCode && passedOutputValidation == ValidationResult::UnwrittenOutputFileFail))
    {
        for (const FrozenFileAndHash &output : node_data->m_OutputFiles)
        {
            Log(kDebug, "Removing output file %s from failed build", output.m_Filename.Get());
            remove(output.m_Filename);
            StatCacheMarkDirty(stat_cache, output.m_Filename, output.m_FilenameHash);
        }
    }

    return NodeBuildResult::kRanFailed;
}

