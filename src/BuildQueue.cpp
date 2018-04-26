#include "BuildQueue.hpp"
#include "DagData.hpp"
#include "MemAllocHeap.hpp"
#include "MemAllocLinear.hpp"
#include "NodeState.hpp"
#include "Scanner.hpp"
#include "FileInfo.hpp"
#include "StateData.hpp"
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
#include "JsonWriter.hpp"

#include <stdio.h>

namespace t2
{
  namespace BuildResult
  {
    const char* Names[Enum::kCount] =
    {
      "build success",
      "build interrupted",
      "build failed",
      "build failed to setup error"
    };
  }


  static void ThreadStateInit(ThreadState* self, BuildQueue* queue, size_t scratch_size, int index)
  {
    HeapInit(&self->m_LocalHeap);
    LinearAllocInit(&self->m_ScratchAlloc, &self->m_LocalHeap, scratch_size, "thread-local scratch");
    self->m_ThreadIndex = index;
    self->m_Queue       = queue;
    JsonWriteInit(&self->m_StructuredMsg, &self->m_LocalHeap);
  }

  static void ThreadStateDestroy(ThreadState* self)
  {
    JsonWriteDestroy(&self->m_StructuredMsg);
    LinearAllocDestroy(&self->m_ScratchAlloc);
    HeapDestroy(&self->m_LocalHeap);
  }

  static int AvailableNodeCount(BuildQueue* queue)
  {
    const uint32_t queue_mask  = queue->m_QueueCapacity - 1;
    uint32_t       read_index  = queue->m_QueueReadIndex;
    uint32_t       write_index = queue->m_QueueWriteIndex;

    return (write_index - read_index) & queue_mask;
  }

  static NodeState* GetStateForNode(BuildQueue* queue, int32_t src_index)
  {
    int32_t state_index = queue->m_Config.m_NodeRemappingTable[src_index];

    if (state_index == -1)
      return nullptr;

    NodeState* state = queue->m_Config.m_NodeState + state_index;

    CHECK(int(state->m_MmapData - queue->m_Config.m_NodeData) == src_index);

    return state;
  }


  static bool AllDependenciesReady(BuildQueue* queue, const NodeState* state)
  {
    const NodeData *src_node      = state->m_MmapData;

    for (int32_t dep_index : src_node->m_Dependencies)
    {
      NodeState* state = GetStateForNode(queue, dep_index);

      CHECK(state != nullptr);

      if (!NodeStateIsCompleted(state))
      {
        return false;
      }
    }

    return true;
  }


  static void WakeWaiters(BuildQueue* queue, int count)
  {
    if (count > 1)
      CondBroadcast(&queue->m_WorkAvailable);
    else
      CondSignal(&queue->m_WorkAvailable);
  }

  static void Enqueue(BuildQueue* queue, NodeState* state)
  {
    uint32_t       write_index = queue->m_QueueWriteIndex;
    const uint32_t queue_mask  = queue->m_QueueCapacity - 1;
    int32_t*       build_queue = queue->m_Queue;


    CHECK(AllDependenciesReady(queue, state));
    CHECK(!NodeStateIsQueued(state));
    CHECK(!NodeStateIsActive(state));
    CHECK(!NodeStateIsCompleted(state));
    CHECK(state->m_MmapData->m_PassIndex == queue->m_CurrentPassIndex);

#if ENABLED(CHECKED_BUILD)
    const int avail_init = AvailableNodeCount(queue);
#endif

    int state_index = int(state - queue->m_Config.m_NodeState);

    build_queue[write_index] = state_index;
    write_index              = (write_index + 1) & queue_mask;
    queue->m_QueueWriteIndex = write_index;

    NodeStateFlagQueued(state);

    CHECK(AvailableNodeCount(queue) == 1 + avail_init);
  }

  static void ParkExpensiveNode(BuildQueue* queue, NodeState* state)
  {
    NodeStateFlagQueued(state);
    CHECK(queue->m_ExpensiveWaitCount < (int) queue->m_QueueCapacity);
    queue->m_ExpensiveWaitList[queue->m_ExpensiveWaitCount++] = state;
  }

  static void UnparkExpensiveNode(BuildQueue* queue)
  {
    if (queue->m_ExpensiveWaitCount > 0)
    {
      NodeState* node = queue->m_ExpensiveWaitList[--queue->m_ExpensiveWaitCount];
      CHECK(NodeStateIsQueued(node));
      // Really only to avoid tripping up checks in Enqueue()
      NodeStateFlagUnqueued(node);
      NodeStateFlagInactive(node);
      Enqueue(queue, node);
      CondSignal(&queue->m_WorkAvailable);
    }
  }

  static BuildProgress::Enum SetupDependencies(BuildQueue* queue, NodeState* node)
  {
    const NodeData *src_node         = node->m_MmapData;
    int             dep_waits_needed = 0;
    int             enqueue_count    = 0;

    // Go through all dependencies and see how those nodes are doing.  If any
    // of them are not finished, we'll have to wait before this node can continue
    // to advance its state machine.
    for (int32_t dep_index : src_node->m_Dependencies)
    {
      NodeState* state = GetStateForNode(queue, dep_index);

      CHECK(state != nullptr);

      CHECK(state->m_MmapData->m_PassIndex <= src_node->m_PassIndex);

      if (NodeStateIsCompleted(state))
        continue;

      ++dep_waits_needed;

      if (!NodeStateIsQueued(state) && !NodeStateIsActive(state) && !NodeStateIsBlocked(state))
      {
        Enqueue(queue, state);
        ++enqueue_count;
      }
    }

    if (enqueue_count > 0)
      WakeWaiters(queue, enqueue_count);

    // We're waiting on dependencies to be ready.
    if (dep_waits_needed > 0)
      return BuildProgress::kBlocked;

    return BuildProgress::kUnblocked;
  }

  static bool OutputFilesDiffer(const NodeData* node_data, const NodeStateData* prev_state)
  {
    int file_count = node_data->m_OutputFiles.GetCount();

    if (file_count != prev_state->m_OutputFiles.GetCount())
      return true;

    for (int i = 0; i < file_count; ++i)
    {
      if (0 != strcmp(node_data->m_OutputFiles[i].m_Filename, prev_state->m_OutputFiles[i]))
        return true;
    }

    return false;
  }

  static bool OutputFilesMissing(StatCache* stat_cache, const NodeData* node)
  {
    for (const FrozenFileAndHash& f : node->m_OutputFiles)
    {
      FileInfo i = StatCacheStat(stat_cache, f.m_Filename, f.m_FilenameHash);

      if (!i.Exists())
        return true;
    }

    return false;
  }

  static bool MakeDirectoriesRecursive(StatCache* stat_cache, const PathBuffer& dir)
  {
    PathBuffer parent_dir = dir;
    PathStripLast(&parent_dir);

    // Can't go any higher.
    if (dir == parent_dir)
      return true;

    if (!MakeDirectoriesRecursive(stat_cache, parent_dir))
      return false;

    char path[kMaxPathLength];
    PathFormat(path, &dir);

    FileInfo info = StatCacheStat(stat_cache, path);

    if (info.Exists())
    {
      // Just asssume this is a directory. We could check it - but there's currently no way via _stat64() calls
      // on Windows to check if a file is a symbolic link (to a directory).
      return true;
    }
    else
    {
      Log(kSpam, "create dir \"%s\"", path);
      bool success = MakeDirectory(path);
      StatCacheMarkDirty(stat_cache, path, Djb2HashPath(path));
      return success;
    }
  }

  static bool MakeDirectoriesForFile(StatCache* stat_cache, const PathBuffer& buffer)
  {
    PathBuffer path = buffer;
    PathStripLast(&path);
    return MakeDirectoriesRecursive(stat_cache, path);
  }

  static void ReportInputSignatureChangeCause(JsonWriter* msg, NodeState* node, const uint64_t node_index, const NodeStateData* prev_state, HashComponentLog* hashComponentLog)
  {
    JsonWriteReset(msg);
    JsonWriteStartObject(msg);
    JsonWriteKeyName(msg, "msg");
    JsonWriteValueString(msg, "inputSignatureChanged");
    JsonWriteKeyName(msg, "annotation");
    JsonWriteValueString(msg, node->m_MmapData->m_Annotation.Get());
    JsonWriteKeyName(msg, "index");
    JsonWriteValueInteger(msg, node_index);

    if (prev_state->m_InputSignatureComponents.GetCount() != node->m_ComponentLogRange.m_Count)
    {
      JsonWriteKeyName(msg, "oldKeyCount");
      JsonWriteValueInteger(msg, prev_state->m_InputSignatureComponents.GetCount());
      JsonWriteKeyName(msg, "newKeyCount");
      JsonWriteValueInteger(msg, node->m_ComponentLogRange.m_Count);
      goto endObjectAndLog;
    }
    
    MutexLock(&hashComponentLog->mutex);

    JsonWriteKeyName(msg, "changes");
    JsonWriteStartArray(msg);

    for (int i = 0; i < node->m_ComponentLogRange.m_Count; ++i)
    {
      HashComponent& component = hashComponentLog->components[node->m_ComponentLogRange.m_Index + i];

      const char* key = &hashComponentLog->strings[component.m_Key];
      const char* prevKey = prev_state->m_InputSignatureComponents[i].m_Key.Get();
      const bool keyChanged = (0 != strcmp(key, prevKey));

      const char* value = &hashComponentLog->strings[component.m_Value];
      const char* prevValue = prev_state->m_InputSignatureComponents[i].m_Value.Get();
      const bool valueChanged = (0 != strcmp(value, prevValue));

      if (!keyChanged && !valueChanged)
        continue;

      JsonWriteStartObject(msg);

      bool emitKeyAsFilePath = false;

      if (component.m_Kind != HashComponent::kGeneric)
      {
        JsonWriteKeyName(msg, "kind");
        switch(component.m_Kind)
        {
          case HashComponent::kFilePath:
            JsonWriteValueString(msg, "path");
            break;
          case HashComponent::kFileSHA1:
            JsonWriteValueString(msg, "sha1");
            emitKeyAsFilePath = true;
            break;
          case HashComponent::kFileTimestamp:
            JsonWriteValueString(msg, "timestamp");
            emitKeyAsFilePath = true;
            break;
          default:
            JsonWriteValueString(msg, "unknown");
            break;
        }
      }

      JsonWriteKeyName(msg, emitKeyAsFilePath ? "path" : "key");
      JsonWriteValueString(msg, key);

      JsonWriteKeyName(msg, "value");
      JsonWriteValueString(msg, value);

      if (keyChanged)
      {
        JsonWriteKeyName(msg, "oldkey");
        JsonWriteValueString(msg, prevKey);
      }

      if (valueChanged)
      {
        JsonWriteKeyName(msg, "oldvalue");
        JsonWriteValueString(msg, prevValue);
      }

      JsonWriteEndObject(msg);
    }

    JsonWriteEndArray(msg);

    MutexUnlock(&hashComponentLog->mutex);

  endObjectAndLog:
    JsonWriteEndObject(msg);
    LogStructured(msg);
  }
    
  static BuildProgress::Enum CheckInputSignature(BuildQueue* queue, ThreadState* thread_state, NodeState* node, Mutex* queue_lock)
  {
    CHECK(AllDependenciesReady(queue, node));

    MutexUnlock(queue_lock);

    const BuildQueueConfig& config = queue->m_Config;
    StatCache* stat_cache = config.m_StatCache;
    DigestCache* digest_cache = config.m_DigestCache;
    HashComponentLog* component_log = config.m_InputSignatureHashLog;

    const NodeData* node_data = node->m_MmapData;
    const uint64_t node_index = node_data->m_OriginalIndex;

    HashState sighash;
    FILE* debug_log = (FILE*) queue->m_Config.m_FileSigningLog;

    // TODO: Give each BuildQueue thread a separate hashComponentLog so that we don't need the mutex.
    // As long as they're all around when we're saving the build state, and we know which log to use
    // for each node, we can just pull the values from each thread's log instance as we go along.
    if (component_log)
      MutexLock(&component_log->mutex);

    if (debug_log)
    {
      MutexLock(queue->m_Config.m_FileSigningLogMutex);
      fprintf(debug_log, "input_sig(\"%s\"):\n", node_data->m_Annotation.Get());
      HashInitDebug(&sighash, debug_log);
    }
    else
    {
      HashInit(&sighash);
    }

    node->m_ComponentLogRange.m_Index = component_log->components.m_Size;

    // Start with command line action. If that changes, we'll definitely have to rebuild.
    HashAddStringLogged(&sighash, node_data->m_Action, component_log, HashComponent::kGeneric, "Action");
    HashAddSeparator(&sighash);

    if (const char* pre_action = node_data->m_PreAction)
    {
      HashAddStringLogged(&sighash, pre_action, component_log, HashComponent::kGeneric, "PreAction");
      HashAddSeparator(&sighash);
    }

    const ScannerData* scanner = node_data->m_Scanner;

    for (const FrozenFileAndHash& input : node_data->m_InputFiles)
    {
      // Add path and timestamp of every direct input file.
      HashAddPathLogged(&sighash, input.m_Filename, component_log);
      ComputeFileSignature(&sighash, stat_cache, digest_cache, input.m_Filename, input.m_FilenameHash, config.m_ShaDigestExtensions, config.m_ShaDigestExtensionCount, component_log);

      if (scanner)
      {
        // Roll back scratch allocator between scans
        MemAllocLinearScope alloc_scope(&thread_state->m_ScratchAlloc);

        ScanInput scan_input;
        scan_input.m_ScannerConfig = scanner;
        scan_input.m_ScratchAlloc  = &thread_state->m_ScratchAlloc;
        scan_input.m_ScratchHeap   = &thread_state->m_LocalHeap;
        scan_input.m_FileName      = input.m_Filename;
        scan_input.m_ScanCache     = queue->m_Config.m_ScanCache;

        ScanOutput scan_output;

        if (ScanImplicitDeps(stat_cache, &scan_input, &scan_output))
        {
          for (int i = 0, count = scan_output.m_IncludedFileCount; i < count; ++i)
          {
            // Add path and timestamp of every indirect input file (#includes)
            const FileAndHash& path = scan_output.m_IncludedFiles[i];
            HashAddPathLogged(&sighash, path.m_Filename, component_log);
            ComputeFileSignature(&sighash, stat_cache, digest_cache, path.m_Filename, path.m_FilenameHash, config.m_ShaDigestExtensions, config.m_ShaDigestExtensionCount, component_log);
          }
        }
      }
    }

    for (const FrozenString& input : node_data->m_AllowedOutputSubstrings)
    {
      HashAddStringLogged(&sighash, (const char*)input, component_log, HashComponent::kGeneric, "AllowedOutputSubstring");
    }

    HashAddIntegerLogged(&sighash, (node_data->m_Flags & NodeData::kFlagAllowUnexpectedOutput) ? 1 : 0, component_log, HashComponent::kGeneric, "AllowUnexpectedOutput");

    HashFinalize(&sighash, &node->m_InputSignature);

    node->m_ComponentLogRange.m_Count = component_log->components.m_Size - node->m_ComponentLogRange.m_Index;
      
    if (component_log)
      MutexUnlock(&component_log->mutex);

    if (debug_log)
    {
      char sig[kDigestStringSize];
      DigestToString(sig, node->m_InputSignature);
      fprintf(debug_log, "  => %s\n", sig);
      MutexUnlock(queue->m_Config.m_FileSigningLogMutex);
    }

    // Figure out if we need to rebuild this node.
    const NodeStateData* prev_state = node->m_MmapState;

    BuildProgress::Enum next_state;

    if (!prev_state)
    {
      // This is a new node - we must built it
      Log(kSpam, "T=%d: building %s - new node", thread_state->m_ThreadIndex, node_data->m_Annotation.Get());

      JsonWriter* msg = &thread_state->m_StructuredMsg;
      JsonWriteReset(msg);
      JsonWriteStartObject(msg);
      JsonWriteKeyName(msg, "msg");
      JsonWriteValueString(msg, "newNode");
      JsonWriteKeyName(msg, "annotation");
      JsonWriteValueString(msg, node_data->m_Annotation);
      JsonWriteKeyName(msg, "index");
      JsonWriteValueInteger(msg, node_index);
      JsonWriteEndObject(msg);
      LogStructured(msg);

      next_state = BuildProgress::kRunAction;
    }
    else if (prev_state->m_InputSignature != node->m_InputSignature)
    {
      // The input signature has changed (either direct inputs or includes)
      // We need to rebuild this node.
      char oldDigest[kDigestStringSize];
      char newDigest[kDigestStringSize];
      DigestToString(oldDigest, prev_state->m_InputSignature);
      DigestToString(newDigest, node->m_InputSignature);

      Log(kSpam, "T=%d: building %s - input signature changed. was:%s now:%s", thread_state->m_ThreadIndex, node_data->m_Annotation.Get(), oldDigest, newDigest);

      if (component_log != nullptr)
        ReportInputSignatureChangeCause(&thread_state->m_StructuredMsg, node, node_index, prev_state, component_log);

      next_state = BuildProgress::kRunAction;
    }
    else if (prev_state->m_BuildResult != 0)
    {
      // The build progress failed the last time around - we need to retry it.
      Log(kSpam, "T=%d: building %s - previous build failed", thread_state->m_ThreadIndex, node_data->m_Annotation.Get());
      next_state = BuildProgress::kRunAction;
    }
    else if (OutputFilesDiffer(node_data, prev_state))
    {
      // The output files are different - need to rebuild.
      Log(kSpam, "T=%d: building %s - output files have changed", thread_state->m_ThreadIndex, node_data->m_Annotation.Get());
      next_state = BuildProgress::kRunAction;
    }
    else if (OutputFilesMissing(stat_cache, node_data))
    {
      // One or more output files are missing - need to rebuild.
      Log(kSpam, "T=%d: building %s - output files are missing", thread_state->m_ThreadIndex, node_data->m_Annotation.Get());
      next_state = BuildProgress::kRunAction;
    }
    else
    {
      // Everything is up to date
      Log(kSpam, "T=%d: %s - up to date", thread_state->m_ThreadIndex, node_data->m_Annotation.Get());
      next_state = BuildProgress::kUpToDate;
    }

    MutexLock(queue_lock);
    if (BuildProgress::kUpToDate == next_state)
      queue->m_ProcessedNodeCount++;
    
    return next_state;
  }

  struct SlowCallbackData
  {
    Mutex* queue_lock;
    const NodeData* node_data;
    uint64_t time_of_start;
    const BuildQueue* build_queue;
  };

  static int SlowCallback(void* user_data)
  {
      SlowCallbackData* data = (SlowCallbackData*) user_data;
      MutexLock(data->queue_lock);
      int sendNextCallbackIn = PrintNodeInProgress(data->node_data, data->time_of_start, data->build_queue);
      MutexUnlock(data->queue_lock);
      return sendNextCallbackIn;
  }

  static ExecResult WriteTextFile(const char* payload, const char* target_file)
  {
    ExecResult result;
    memset(&result, 0, sizeof(result));

    FILE* f = fopen(target_file, "wb");
    if (!f)
    {
      result.m_ReturnCode = 1;
      return result;
    }
    int length = strlen(payload);
    int written = fwrite(payload, sizeof(char), length, f);
    fclose(f);

    result.m_ReturnCode = length == written ? 0 : 1;
    return result;
  }

  static BuildProgress::Enum RunAction(BuildQueue* queue, ThreadState* thread_state, NodeState* node, Mutex* queue_lock)
  {
    const NodeData    *node_data    = node->m_MmapData;
    const bool        isWriteFileAction = node->m_MmapData->m_Flags & NodeData::kFlagIsWriteTextFileAction;
    const char        *cmd_line     = node_data->m_Action;
    const char        *pre_cmd_line = node_data->m_PreAction;

    if (!isWriteFileAction && (!cmd_line || cmd_line[0] == '\0'))
    {
      queue->m_ProcessedNodeCount++;
      return BuildProgress::kSucceeded;
    }

    if (node->m_MmapData->m_Flags & NodeData::kFlagExpensive)
    {
      if (queue->m_ExpensiveRunning == queue->m_Config.m_MaxExpensiveCount)
      {
        ParkExpensiveNode(queue, node);
        return BuildProgress::kRunAction;
      }
      else
      {
        ++queue->m_ExpensiveRunning;
      }
    }

    MutexUnlock(queue_lock);

    StatCache         *stat_cache   = queue->m_Config.m_StatCache;
    const char        *annotation   = node_data->m_Annotation;
    int                job_id       = thread_state->m_ThreadIndex;
    bool               echo_cmdline = 0 != (queue->m_Config.m_Flags & BuildQueueConfig::kFlagEchoCommandLines);
    const char        *last_cmd_line = nullptr;
    // Repack frozen env to pointers on the stack.
    int                env_count    = node_data->m_EnvVars.GetCount();
    EnvVariable*       env_vars     = (EnvVariable*) alloca(env_count * sizeof(EnvVariable));
    for (int i = 0; i < env_count; ++i)
    {
      env_vars[i].m_Name  = node_data->m_EnvVars[i].m_Name;
      env_vars[i].m_Value = node_data->m_EnvVars[i].m_Value;
    }

    for (const FrozenFileAndHash& output_file : node_data->m_OutputFiles)
    {
      PathBuffer output;
      PathInit(&output, output_file.m_Filename);

      if (!MakeDirectoriesForFile(stat_cache, output))
      {
        Log(kError, "failed to create output directories for %s", output_file.m_Filename.Get());
        MutexLock(queue_lock);
        return BuildProgress::kFailed;
      }
    }

    ExecResult result = { 0, false };

    // See if we need to remove the output files before running anything.
    if (0 == (node_data->m_Flags & NodeData::kFlagOverwriteOutputs))
    {
      for (const FrozenFileAndHash& output : node_data->m_OutputFiles)
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

    if (pre_cmd_line)
    {
      Log(kSpam, "Launching pre-action process");
      TimingScope timing_scope(&g_Stats.m_ExecCount, &g_Stats.m_ExecTimeCycles);
      ProfilerScope prof_scope("Pre-build", job_id);
      last_cmd_line = pre_cmd_line;
      result = ExecuteProcess(pre_cmd_line, env_count, env_vars, thread_state->m_Queue->m_Config.m_Heap, job_id, false, SlowCallback, &slowCallbackData, 1);
      Log(kSpam, "Process return code %d", result.m_ReturnCode);
    }

    ValidationResult passedOutputValidation = ValidationResult::Pass;
    if (0 == result.m_ReturnCode)
    {
      Log(kSpam, "Launching process");
      TimingScope timing_scope(&g_Stats.m_ExecCount, &g_Stats.m_ExecTimeCycles);
      ProfilerScope prof_scope(annotation, job_id);
      if (isWriteFileAction)
        result = WriteTextFile(node_data->m_Action, node_data->m_OutputFiles[0].m_Filename);
      else
      {
        last_cmd_line = cmd_line;
        result = ExecuteProcess(cmd_line, env_count, env_vars, thread_state->m_Queue->m_Config.m_Heap, job_id, false, SlowCallback, &slowCallbackData);
        passedOutputValidation = ValidateExecResultAgainstAllowedOutput(&result, node_data);
      }
      Log(kSpam, "Process return code %d", result.m_ReturnCode);
    }

    for (const FrozenFileAndHash& output : node_data->m_OutputFiles)
    {
      StatCacheMarkDirty(stat_cache, output.m_Filename, output.m_FilenameHash);
    }

    MutexLock(queue_lock);
    PrintNodeResult(&result, node_data, last_cmd_line, thread_state->m_Queue, echo_cmdline, time_of_start, passedOutputValidation);
    ExecResultFreeMemory(&result);

    if (result.m_WasAborted)
    {
      SignalSet("child processes was aborted");
    }

    if (0 == result.m_ReturnCode && passedOutputValidation != ValidationResult::Fail)
    {
      return BuildProgress::kSucceeded;
    }
    else
    {
      // Clean up output files after a failed build unless they are precious.
      if (0 == (NodeData::kFlagPreciousOutputs & node_data->m_Flags))
      {
        for (const FrozenFileAndHash& output : node_data->m_OutputFiles)
        {
          Log(kDebug, "Removing output file %s from failed build", output.m_Filename.Get());
          remove(output.m_Filename);
          StatCacheMarkDirty(stat_cache, output.m_Filename, output.m_FilenameHash);
        }
      }

      return BuildProgress::kFailed;
    }
  }

  static void UnblockWaiters(BuildQueue* queue, NodeState* node)
  {
    const NodeData *src_node       = node->m_MmapData;
    int             enqueue_count  = 0;

    for (int32_t link : src_node->m_BackLinks)
    {
      if (NodeState* waiter = GetStateForNode(queue, link))
      {
        // Only wake nodes in our current pass
        if (waiter->m_MmapData->m_PassIndex != queue->m_CurrentPassIndex)
          continue;

        // If the node isn't ready, skip it.
        if (!AllDependenciesReady(queue, waiter))
          continue;

        // Did someone else get to the node first?
        if (NodeStateIsQueued(waiter) || NodeStateIsActive(waiter))
          continue;

        //printf("%s is ready to go\n", GetSourceNode(queue, waiter)->m_Annotation);
        Enqueue(queue, waiter);
        ++enqueue_count;
      }
    }

    if (enqueue_count > 0)
      WakeWaiters(queue, enqueue_count);
  }

  static void AdvanceNode(BuildQueue* queue, ThreadState* thread_state, NodeState* node, Mutex* queue_lock)
  {
    Log(kSpam, "T=%d, [%d] Advancing %s\n",
        thread_state->m_ThreadIndex, node->m_Progress, node->m_MmapData->m_Annotation.Get());

    CHECK(!NodeStateIsCompleted(node));
    CHECK(NodeStateIsActive(node));
    CHECK(!NodeStateIsQueued(node));

    for (;;)
    {
      switch (node->m_Progress)
      {
        case BuildProgress::kInitial:
          node->m_Progress = SetupDependencies(queue, node);

          if (BuildProgress::kBlocked == node->m_Progress)
          {
            // Set ourselves as inactive until our dependencies are ready.
            NodeStateFlagInactive(node);
            return;
          }
          else
            break;

        case BuildProgress::kBlocked:
          CHECK(AllDependenciesReady(queue, node));
          node->m_Progress = BuildProgress::kUnblocked;
          break;

        case BuildProgress::kUnblocked:
          node->m_Progress = CheckInputSignature(queue, thread_state, node, queue_lock);
          break;

        case BuildProgress::kRunAction:
          node->m_Progress = RunAction(queue, thread_state, node, queue_lock);

          // If we couldn't make progress, we're a parked expensive node.
          // Another expensive job will put us back on the queue later when it
          // has finshed.
          if (BuildProgress::kRunAction == node->m_Progress)
            return;

          // Otherwise, we just ran our action. If we were an expensive node,
          // make sure to let other expensive nodes on to the cores now.
          if (node->m_MmapData->m_Flags & NodeData::kFlagExpensive)
          {
            --queue->m_ExpensiveRunning;
            CHECK(queue->m_ExpensiveRunning >= 0);

            // We were an expensive job. We can unpark another expensive job if
            // anything is waiting.
            UnparkExpensiveNode(queue);
          }
          break;

        case BuildProgress::kUpToDate:
        case BuildProgress::kSucceeded:
          node->m_BuildResult = 0;
          node->m_Progress    = BuildProgress::kCompleted;
          break;

        case BuildProgress::kFailed:
          queue->m_FailedNodeCount++;

          CondBroadcast(&queue->m_WorkAvailable);

          node->m_BuildResult = 1;
          node->m_Progress    = BuildProgress::kCompleted;
          break;

        case BuildProgress::kCompleted:
          queue->m_PendingNodeCount--;

          UnblockWaiters(queue, node);

          CondBroadcast(&queue->m_WorkAvailable);
          return;

        default:
          Croak("invalid node state progress");
          break;
      }
    }
  }

  static NodeState* NextNode(BuildQueue* queue)
  {
    int avail_count = AvailableNodeCount(queue);

    if (0 == avail_count)
      return nullptr;

    uint32_t read_index = queue->m_QueueReadIndex;

    int32_t node_index = queue->m_Queue[read_index];

    // Update read index
    queue->m_QueueReadIndex = (read_index + 1) & (queue->m_QueueCapacity - 1);

    NodeState* state = queue->m_Config.m_NodeState + node_index;

    CHECK(NodeStateIsQueued(state));
    CHECK(!NodeStateIsActive(state));

    NodeStateFlagUnqueued(state);
    NodeStateFlagActive(state);

    return state;
  }

  static bool ShouldKeepBuilding(BuildQueue* queue, int thread_index)
  {
    // Stop running if we were signalled
    if (nullptr != SignalGetReason())
      return false;

    // Stop running if there are errors and we're stopping on the first error.
    if (queue->m_FailedNodeCount > 0)
    {
      if (0 == (queue->m_Config.m_Flags & BuildQueueConfig::kFlagContinueOnError))
      {
        return false;
      }
    }

    // If we're quitting, definitely stop building.
    if (queue->m_QuitSignalled)
      return false;

    // If we're a worker thread, keep running until we quit.
    if (0 != thread_index)
      return true;

    // We're the main thread. Just loop until there's no more nodes and then move on to the next pass.
    return queue->m_PendingNodeCount > 0;
  }

  static void BuildLoop(ThreadState* thread_state)
  {
    BuildQueue        *queue = thread_state->m_Queue;
    ConditionVariable *cv    = &queue->m_WorkAvailable;
    Mutex             *mutex = &queue->m_Lock;

    MutexLock(mutex);

    while (ShouldKeepBuilding(queue, thread_state->m_ThreadIndex))
    {
      if (NodeState* node = NextNode(queue))
      {
        AdvanceNode(queue, thread_state, node, mutex);
      }
      else
      {
        CondWait(cv, mutex);
      }
    }

    MutexUnlock(mutex);

    Log(kSpam, "build thread %d exiting\n", thread_state->m_ThreadIndex);
  }

  static ThreadRoutineReturnType TUNDRA_STDCALL BuildThreadRoutine(void* param)
  {
    ThreadState *thread_state = static_cast<ThreadState*>(param);

    LinearAllocSetOwner(&thread_state->m_ScratchAlloc, ThreadCurrent());

    BuildLoop(thread_state);

    return 0;
  }

  void BuildQueueInit(BuildQueue* queue, const BuildQueueConfig* config)
  {
    ProfilerScope prof_scope("Tundra BuildQueueInit", 0);
    CHECK(config->m_MaxExpensiveCount > 0 && config->m_MaxExpensiveCount <= config->m_ThreadCount);

    MutexInit(&queue->m_Lock);
    CondInit(&queue->m_WorkAvailable);

    // Compute queue capacity. Allocate space for a power of two number of
    // indices that's at least one larger than the max number of nodes. Because
    // the queue is treated as a ring buffer, we want W=R to mean an empty
    // buffer.
    uint32_t capacity = NextPowerOfTwo(config->m_MaxNodes + 1);

    MemAllocHeap* heap = config->m_Heap;

    queue->m_Queue              = HeapAllocateArray<int32_t>(heap, capacity);
    queue->m_QueueReadIndex     = 0;
    queue->m_QueueWriteIndex    = 0;
    queue->m_QueueCapacity      = capacity;
    queue->m_Config             = *config;
    queue->m_PendingNodeCount   = 0;
    queue->m_FailedNodeCount    = 0;
    queue->m_ProcessedNodeCount = 0;
    queue->m_QuitSignalled      = false;
    queue->m_ExpensiveRunning   = 0;
    queue->m_ExpensiveWaitCount = 0;
    queue->m_ExpensiveWaitList  = HeapAllocateArray<NodeState*>(heap, capacity);

    CHECK(queue->m_Queue);

    if (queue->m_Config.m_ThreadCount > kMaxBuildThreads)
    {
      Log(kWarning, "too many build threads (%d) - clamping to %d",
          queue->m_Config.m_ThreadCount, kMaxBuildThreads);

      queue->m_Config.m_ThreadCount = kMaxBuildThreads;
    }

    Log(kDebug, "build queue initialized; ring buffer capacity = %u", queue->m_QueueCapacity);

    // Block all signals on the main thread.
    SignalBlockThread(true);
    SignalHandlerSetCondition(&queue->m_WorkAvailable);

    // Create build threads.
    for (int i = 0, thread_count = config->m_ThreadCount; i < thread_count; ++i)
    {
      ThreadState* thread_state = &queue->m_ThreadState[i];

      ThreadStateInit(thread_state, queue, MB(32), i);

      if (i > 0)
      {
        Log(kDebug, "starting build thread %d", i);
        queue->m_Threads[i] = ThreadStart(BuildThreadRoutine, thread_state);
      }
    }
  }

  void BuildQueueDestroy(BuildQueue* queue)
  {
    ProfilerScope prof_scope("Tundra BuildQueueDestroy", 0);
    Log(kDebug, "destroying build queue");
    const BuildQueueConfig* config = &queue->m_Config;

    MutexLock(&queue->m_Lock);
    queue->m_QuitSignalled = true;
    MutexUnlock(&queue->m_Lock);

    CondBroadcast(&queue->m_WorkAvailable);

    for (int i = 0, thread_count = config->m_ThreadCount; i < thread_count; ++i)
    {
      if (i > 0)
      {
        Log(kDebug, "joining with build thread %d", i);
        ThreadJoin(queue->m_Threads[i]);
      }

      ThreadStateDestroy(&queue->m_ThreadState[i]);
    }

    // Deallocate storage.
    MemAllocHeap* heap = queue->m_Config.m_Heap;
    HeapFree(heap, queue->m_ExpensiveWaitList);
    HeapFree(heap, queue->m_Queue);

    CondDestroy(&queue->m_WorkAvailable);
    MutexDestroy(&queue->m_Lock);

    // Unblock all signals on the main thread.
    SignalHandlerSetCondition(nullptr);
    SignalBlockThread(false);
  }

  BuildResult::Enum BuildQueueBuildNodeRange(BuildQueue* queue, int start_index, int count, int pass_index)
  {
    // Make sure none of the build threads see in-progress state due to a spurious wakeup.
    MutexLock(&queue->m_Lock);

    CHECK(start_index + count <= queue->m_Config.m_MaxNodes);

    queue->m_CurrentPassIndex = pass_index;

    // Initialize build queue with index range to build
    int32_t   *build_queue = queue->m_Queue;
    NodeState *node_states = queue->m_Config.m_NodeState;

    for (int i = 0; i < count; ++i)
    {
      NodeState* state = node_states + start_index + i;

      NodeStateFlagQueued(state);

      // Verify node hasn't been touched already
      CHECK(state->m_Progress == BuildProgress::kInitial);

      build_queue[i] = start_index + i;
    }

    queue->m_PendingNodeCount = count;
    queue->m_FailedNodeCount  = 0;
    queue->m_QueueWriteIndex  = count;
    queue->m_QueueReadIndex   = 0;

    MutexUnlock(&queue->m_Lock);

    CondBroadcast(&queue->m_WorkAvailable);

    // This thread is thread 0.
    BuildLoop(&queue->m_ThreadState[0]);

    if (SignalGetReason())
      return BuildResult::kInterrupted;
    else if (queue->m_FailedNodeCount)
      return BuildResult::kBuildError;
    else
      return BuildResult::kOk;
  }
}

