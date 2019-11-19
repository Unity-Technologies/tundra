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
#include "DigestCache.hpp"
#include "SharedResources.hpp"
#include "HumanActivityDetection.hpp"
#include <stdarg.h>

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
      "build failed to setup error",
      "requires additional run"
    };
  }

  static void ThreadStateInit(ThreadState* self, BuildQueue* queue, size_t scratch_size, int index, int profiler_thread_id)
  {
    HeapInit(&self->m_LocalHeap);
    LinearAllocInit(&self->m_ScratchAlloc, &self->m_LocalHeap, scratch_size, "thread-local scratch");
    self->m_ThreadIndex = index;
    self->m_Queue       = queue;
    self->m_ProfilerThreadId = profiler_thread_id;
  }

  static void ThreadStateDestroy(ThreadState* self)
  {
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

  static bool IsNodeReadyToBeAdvanced(BuildQueue* queue, const NodeState* state)
  {
    const NodeData *src_node      = state->m_MmapData;

    for (int32_t dep_index : src_node->m_Dependencies)
    {
      NodeState* state = GetStateForNode(queue, dep_index);
      CHECK(state != nullptr);

      const char* name = state->m_MmapData->m_Annotation.Get();
      Log(LogLevel::kInfo, "jaja %s\n", name);


      switch(state->m_Progress)
      {
         case BuildProgress::kInitial:
           Croak("Should never happen!?");
           break;
         case BuildProgress::kWaitingForDependencies:
         case BuildProgress::kRunAction:
            return false;
         case BuildProgress::kPermanentlyBlocked:
         case BuildProgress::kAllDependencesSucceeded:
         case BuildProgress::kSucceeded:
         case BuildProgress::kSucceededButDependendeesRequireFrontendRerun:
         case BuildProgress::kUpToDate:
         case BuildProgress::kFailed:
         case BuildProgress::kCompleted:
           break;
         default:
         Croak("Unexpected progress");
      }
    }

    return true;
  }

  static BuildProgress::Enum NextProgressForNodesWhoseDependenciesFinished(BuildQueue* queue, const NodeState* state)
  {
    for (int32_t dep_index : state->m_MmapData->m_Dependencies)
    {
      NodeState* depState = GetStateForNode(queue, dep_index);

      BuildProgress::Enum depProgress = depState->m_Progress;

      switch(depProgress)
      {
        case BuildProgress::kPermanentlyBlocked:
        case BuildProgress::kFailed:
        case BuildProgress::kSucceededButDependendeesRequireFrontendRerun:
          return BuildProgress::kPermanentlyBlocked;
        case BuildProgress::kSucceeded:
        case BuildProgress::kCompleted:
          continue;
        case BuildProgress::kWaitingForDependencies:
        case BuildProgress::kAllDependencesSucceeded:
          return BuildProgress::kWaitingForDependencies;
        default:
          Croak("Unexpected node state for dependency %d", depProgress);
      }
    }

    return BuildProgress::kAllDependencesSucceeded;
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

    CHECK(IsNodeReadyToBeAdvanced(queue, state));
    CHECK(!NodeStateIsQueued(state));
    CHECK(!NodeStateIsActive(state));
    CHECK(state->m_Progress <= BuildProgress::kAllDependencesSucceeded);
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

      if (!NodeStateIsQueued(state) && !NodeStateIsActive(state) && !NodeStateIsWaitingForDependencies(state))
      {
        Enqueue(queue, state);
        ++enqueue_count;
      }
    }

    if (enqueue_count > 0)
      WakeWaiters(queue, enqueue_count);

    // We're waiting on dependencies to be ready.
    if (dep_waits_needed > 0)
      return BuildProgress::kWaitingForDependencies;

    return BuildProgress::kAllDependencesSucceeded;
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
      // Just assume this is a directory. We could check it - but there's currently no way via _stat64() calls
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

  static void CheckAndReportChangedInputFile(
    JsonWriter* msg,
    const char* filename,
    uint32_t filenameHash,
    uint64_t lastTimestamp,
    const char* dependencyType,
    DigestCache* digest_cache,
    StatCache* stat_cache,
    const uint32_t sha_extension_hashes[],
    uint32_t sha_extension_hash_count,
    bool force_use_timestamp)
  {
    if (!force_use_timestamp && ShouldUseSHA1SignatureFor(filename, sha_extension_hashes, sha_extension_hash_count))
    {
        // The file signature was computed from SHA1 digest, so look in the digest cache to see if we computed a new
        // hash for it that doesn't match the frozen data
        if (DigestCacheHasChanged(digest_cache, filename, filenameHash))
        {
          JsonWriteStartObject(msg);

          JsonWriteKeyName(msg, "key");
          JsonWriteValueString(msg, "InputFileDigest");

          JsonWriteKeyName(msg, "path");
          JsonWriteValueString(msg, filename);

          JsonWriteKeyName(msg, "dependency");
          JsonWriteValueString(msg, dependencyType);

          JsonWriteEndObject(msg);
        }
      }
      else
      {
        // The file signature was computed from timestamp alone, so we only need to examine the stat cache
        FileInfo fileInfo = StatCacheStat(stat_cache, filename, filenameHash);

        uint64_t timestamp = 0;
        if (fileInfo.Exists())
          timestamp = fileInfo.m_Timestamp;

        if (timestamp != lastTimestamp)
        {
          JsonWriteStartObject(msg);

          JsonWriteKeyName(msg, "key");
          JsonWriteValueString(msg, "InputFileTimestamp");

          JsonWriteKeyName(msg, "path");
          JsonWriteValueString(msg, filename);

          JsonWriteKeyName(msg, "dependency");
          JsonWriteValueString(msg, dependencyType);

          JsonWriteEndObject(msg);
        }
      }
  }

  static void ReportChangedInputFiles(JsonWriter* msg, const FrozenArray<NodeInputFileData>& files, const char* dependencyType, DigestCache* digest_cache, StatCache* stat_cache, const uint32_t sha_extension_hashes[], uint32_t sha_extension_hash_count, bool force_use_timestamp)
  {
    for (const NodeInputFileData& input : files)
    {
      uint32_t filenameHash = Djb2HashPath(input.m_Filename);

      CheckAndReportChangedInputFile(msg,
        input.m_Filename,
        filenameHash,
        input.m_Timestamp,
        dependencyType,
        digest_cache,
        stat_cache,
        sha_extension_hashes,
        sha_extension_hash_count,
        force_use_timestamp);
    }
  }

  static void ReportValueWithOptionalTruncation(JsonWriter* msg, const char* keyName, const char* truncatedKeyName, const FrozenString& value)
  {
    size_t len = strlen(value);
    const size_t maxLen = KB(64);
    JsonWriteKeyName(msg, keyName);
    JsonWriteValueString(msg, value, maxLen);
    if (len > maxLen)
    {
      JsonWriteKeyName(msg, truncatedKeyName);
      JsonWriteValueInteger(msg, 1);
    }
  }

  static void ReportInputSignatureChanges(
    JsonWriter* msg,
    NodeState* node,
    const NodeData* node_data,
    const NodeStateData* prev_state,
    StatCache* stat_cache,
    DigestCache* digest_cache,
    ScanCache* scan_cache,
    const uint32_t sha_extension_hashes[],
    int sha_extension_hash_count,
    ThreadState* thread_state)
  {
    if (strcmp(node_data->m_Action, prev_state->m_Action) != 0)
    {
      JsonWriteStartObject(msg);

      JsonWriteKeyName(msg, "key");
      JsonWriteValueString(msg, "Action");

      ReportValueWithOptionalTruncation(msg, "value", "value_truncated", node_data->m_Action);
      ReportValueWithOptionalTruncation(msg, "oldvalue", "oldvalue_truncated", prev_state->m_Action);

      JsonWriteEndObject(msg);
    }

    if (node_data->m_PreAction.Get() || prev_state->m_PreAction.Get())
    {
      if (!node_data->m_PreAction.Get() || !prev_state->m_PreAction.Get() || strcmp(node_data->m_PreAction, prev_state->m_PreAction) != 0)
      {
        JsonWriteStartObject(msg);

        JsonWriteKeyName(msg, "key");
        JsonWriteValueString(msg, "PreAction");

        ReportValueWithOptionalTruncation(msg, "value", "value_truncated", node_data->m_PreAction);
        ReportValueWithOptionalTruncation(msg, "oldvalue", "oldvalue_truncated", prev_state->m_PreAction);

        JsonWriteEndObject(msg);
      }
    }

    bool explicitInputFilesListChanged = node_data->m_InputFiles.GetCount() != prev_state->m_InputFiles.GetCount();
    for (int32_t i = 0; i < node_data->m_InputFiles.GetCount() && !explicitInputFilesListChanged; ++i)
    {
      const char* filename = node_data->m_InputFiles[i].m_Filename;
      const char* oldFilename = prev_state->m_InputFiles[i].m_Filename;
      explicitInputFilesListChanged |= (strcmp(filename, oldFilename) != 0);
    }
    bool force_use_timestamp = node->m_Flags & NodeData::kFlagBanContentDigestForInputs;
    if (explicitInputFilesListChanged)
    {
      JsonWriteStartObject(msg);

      JsonWriteKeyName(msg, "key");
      JsonWriteValueString(msg, "InputFileList");

      JsonWriteKeyName(msg, "value");
      JsonWriteStartArray(msg);
      for (const FrozenFileAndHash& input : node_data->m_InputFiles)
        JsonWriteValueString(msg, input.m_Filename);
      JsonWriteEndArray(msg);

      JsonWriteKeyName(msg, "oldvalue");
      JsonWriteStartArray(msg);
      for (const NodeInputFileData& input : prev_state->m_InputFiles)
        JsonWriteValueString(msg, input.m_Filename);
      JsonWriteEndArray(msg);

      JsonWriteKeyName(msg, "dependency");
      JsonWriteValueString(msg, "explicit");

      JsonWriteEndObject(msg);

      // We also want to catch if any of the input files (common to both old + new lists) have changed themselves,
      // because a common reason for the input list changing is the command changing, and the part of the
      // command that is different may be in response file(s).
      for (const NodeInputFileData& oldInput : prev_state->m_InputFiles)
      {
        const FrozenFileAndHash* newInput;
        for (newInput = node_data->m_InputFiles.begin(); newInput != node_data->m_InputFiles.end(); ++newInput)
        {
          if (strcmp(newInput->m_Filename, oldInput.m_Filename) == 0)
            break;
        }

        if (newInput == node_data->m_InputFiles.end())
          continue;

        CheckAndReportChangedInputFile(msg,
          oldInput.m_Filename,
          newInput->m_FilenameHash,
          oldInput.m_Timestamp,
          "explicit",
          digest_cache,
          stat_cache,
          sha_extension_hashes,
          sha_extension_hash_count,
          force_use_timestamp
        );
      }

      // Don't do any further checking for changes, there's little point scanning implicit dependencies
      return;
    }

    ReportChangedInputFiles(msg, prev_state->m_InputFiles, "explicit", digest_cache, stat_cache, sha_extension_hashes, sha_extension_hash_count, force_use_timestamp);

    if (node_data->m_Scanner)
    {
      HashTable<bool, kFlagPathStrings> implicitDependencies;
      HashTableInit(&implicitDependencies, &thread_state->m_LocalHeap);

      for (const FrozenFileAndHash& input : node_data->m_InputFiles)
      {
        // Roll back scratch allocator between scans
        MemAllocLinearScope alloc_scope(&thread_state->m_ScratchAlloc);

        ScanInput scan_input;
        scan_input.m_ScannerConfig = node_data->m_Scanner;
        scan_input.m_ScratchAlloc = &thread_state->m_ScratchAlloc;
        scan_input.m_ScratchHeap = &thread_state->m_LocalHeap;
        scan_input.m_FileName = input.m_Filename;
        scan_input.m_ScanCache = scan_cache;

        ScanOutput scan_output;

        if (ScanImplicitDeps(stat_cache, &scan_input, &scan_output))
        {
          for (int i = 0, count = scan_output.m_IncludedFileCount; i < count; ++i)
          {
            const FileAndHash& path = scan_output.m_IncludedFiles[i];
            if (HashTableLookup(&implicitDependencies, path.m_FilenameHash, path.m_Filename) == nullptr)
              HashTableInsert(&implicitDependencies, path.m_FilenameHash, path.m_Filename, false);
          }
        }
      }

      bool implicitFilesListChanged = implicitDependencies.m_RecordCount != prev_state->m_ImplicitInputFiles.GetCount();
      if (!implicitFilesListChanged)
      {
        for (const NodeInputFileData& implicitInput : prev_state->m_ImplicitInputFiles)
        {
          bool* visited = HashTableLookup(&implicitDependencies, Djb2HashPath(implicitInput.m_Filename), implicitInput.m_Filename);
          if (!visited)
          {
            implicitFilesListChanged = true;
            break;
          }

          *visited = true;
        }

        HashTableWalk(&implicitDependencies, [&](int32_t index, uint32_t hash, const char* filename, bool visited)
        {
          if (!visited)
            implicitFilesListChanged = true;
        });
      }

      if (implicitFilesListChanged)
      {
        JsonWriteStartObject(msg);

        JsonWriteKeyName(msg, "key");
        JsonWriteValueString(msg, "InputFileList");

        JsonWriteKeyName(msg, "value");
        JsonWriteStartArray(msg);
        HashTableWalk(&implicitDependencies, [=](int32_t index, uint32_t hash, const char* filename, bool visited) {
          JsonWriteValueString(msg, filename);
        });
        JsonWriteEndArray(msg);

        JsonWriteKeyName(msg, "oldvalue");
        JsonWriteStartArray(msg);
        for (const NodeInputFileData& input : prev_state->m_ImplicitInputFiles)
          JsonWriteValueString(msg, input.m_Filename);
        JsonWriteEndArray(msg);

        JsonWriteKeyName(msg, "dependency");
        JsonWriteValueString(msg, "implicit");

        JsonWriteEndObject(msg);
      }

      HashTableDestroy(&implicitDependencies);
      if (implicitFilesListChanged)
        return;

      ReportChangedInputFiles(msg, prev_state->m_ImplicitInputFiles, "implicit", digest_cache, stat_cache, sha_extension_hashes, sha_extension_hash_count, force_use_timestamp);
    }
  }

  static bool OutputFileInTargetDirectoryMissing(const NodeData* node_data, ThreadState* thread_state)
  {
    for (int i = 0; i < node_data->m_OutputDirectories.GetCount(); i++)
    {
        HashDigest hd = CalculateGlobSignatureFor(node_data->m_OutputDirectories[i].m_Filename, "*", true, &thread_state->m_LocalHeap, &thread_state->m_ScratchAlloc);      
        HashDigest node_digest = node_data->m_OutputDirectoryGlobSignatures[i];
        if (hd != node_digest)
          return true;
    }
    return false;
  }

  static BuildProgress::Enum CheckInputSignature(BuildQueue* queue, ThreadState* thread_state, NodeState* node, Mutex* queue_lock)
  {
    CHECK(IsNodeReadyToBeAdvanced(queue, node));

    MutexUnlock(queue_lock);
    const NodeData* node_data = node->m_MmapData;

    ProfilerScope prof_scope("CheckInputSignature", thread_state->m_ProfilerThreadId, node_data->m_Annotation);

    const BuildQueueConfig& config = queue->m_Config;
    StatCache* stat_cache = config.m_StatCache;
    DigestCache* digest_cache = config.m_DigestCache;


    HashState sighash;
    FILE* debug_log = (FILE*) queue->m_Config.m_FileSigningLog;

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

    // Start with command line action. If that changes, we'll definitely have to rebuild.
    HashAddString(&sighash, node_data->m_Action);
    HashAddSeparator(&sighash);

    if (const char* pre_action = node_data->m_PreAction)
    {
      HashAddString(&sighash, pre_action);
      HashAddSeparator(&sighash);
    }

    const ScannerData* scanner = node_data->m_Scanner;

    // TODO: The input files are not guaranteed to be in a stably sorted order. If the order changes then the input
    // TODO: signature might change, giving us a false-positive for the node needing to be rebuilt. We should look into
    // TODO: enforcing a stable ordering, probably when we compile the DAG.

    // We have a similar problem for implicit dependencies, but we cannot sort them at DAG compilation time because we
    // don't know them then. We also might have duplicate dependencies - not when scanning a single file, but when we
    // have multiple inputs for a single node (e.g. a cpp + a header which is being force-included) then we can end up
    // with the same implicit dependency coming from multiple files. Conceptually it's not good to be adding the same
    // file to the signature multiple times, so we would also like to deduplicate. We use a HashSet to collect all the
    // implicit inputs, both to ensure we have no duplicate entries, and also so we can sort all the inputs before we
    // add them to the signature.
    HashSet<kFlagPathStrings> implicitDeps;
    if (scanner)
      HashSetInit(&implicitDeps, &thread_state->m_LocalHeap);

    bool force_use_timestamp = node_data->m_Flags & NodeData::kFlagBanContentDigestForInputs;

    // Roll back scratch allocator after all file scans
    MemAllocLinearScope alloc_scope(&thread_state->m_ScratchAlloc);

    for (const FrozenFileAndHash& input : node_data->m_InputFiles)
    {
      // Add path and timestamp of every direct input file.
      HashAddPath(&sighash, input.m_Filename);
      ComputeFileSignature(
        &sighash,
        stat_cache,
        digest_cache,
        input.m_Filename,
        input.m_FilenameHash,
        config.m_ShaDigestExtensions,
        config.m_ShaDigestExtensionCount,
        force_use_timestamp);

      if (scanner)
      {
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
            const FileAndHash& path = scan_output.m_IncludedFiles[i];
            if (!HashSetLookup(&implicitDeps, path.m_FilenameHash, path.m_Filename))
              HashSetInsert(&implicitDeps, path.m_FilenameHash, path.m_Filename);
          }
        }
      }
    }

    if (scanner)
    {
      // Add path and timestamp of every indirect input file (#includes).
      // This will walk all the implicit dependencies in hash order.
      HashSetWalk(&implicitDeps, [&](uint32_t, uint32_t hash, const char* filename)
      {
        HashAddPath(&sighash, filename);
        ComputeFileSignature(
          &sighash,
          stat_cache,
          digest_cache,
          filename,
          hash,
          config.m_ShaDigestExtensions,
          config.m_ShaDigestExtensionCount,
          force_use_timestamp
        );
      });

      HashSetDestroy(&implicitDeps);
    }

    for (const FrozenString& input : node_data->m_AllowedOutputSubstrings)
      HashAddString(&sighash, (const char*)input);

    HashAddInteger(&sighash, (node_data->m_Flags & NodeData::kFlagAllowUnexpectedOutput) ? 1 : 0);
    HashAddInteger(&sighash, (node_data->m_Flags & NodeData::kFlagAllowUnwrittenOutputFiles) ? 1 : 0);

    HashFinalize(&sighash, &node->m_InputSignature);


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

      if (IsStructuredLogActive())
      {
        MemAllocLinearScope allocScope(&thread_state->m_ScratchAlloc);

        JsonWriter msg;
        JsonWriteInit(&msg, &thread_state->m_ScratchAlloc);
        JsonWriteStartObject(&msg);

        JsonWriteKeyName(&msg, "msg");
        JsonWriteValueString(&msg, "newNode");

        JsonWriteKeyName(&msg, "annotation");
        JsonWriteValueString(&msg, node_data->m_Annotation);

        JsonWriteKeyName(&msg, "index");
        JsonWriteValueInteger(&msg, node_data->m_OriginalIndex);

        JsonWriteEndObject(&msg);
        LogStructured(&msg);
      }

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

      if (IsStructuredLogActive())
      {
        MemAllocLinearScope allocScope(&thread_state->m_ScratchAlloc);

        JsonWriter msg;
        JsonWriteInit(&msg, &thread_state->m_ScratchAlloc);
        JsonWriteStartObject(&msg);

        JsonWriteKeyName(&msg, "msg");
        JsonWriteValueString(&msg, "inputSignatureChanged");

        JsonWriteKeyName(&msg, "annotation");
        JsonWriteValueString(&msg, node_data->m_Annotation);

        JsonWriteKeyName(&msg, "index");
        JsonWriteValueInteger(&msg, node_data->m_OriginalIndex);

        JsonWriteKeyName(&msg, "changes");
        JsonWriteStartArray(&msg);

        ReportInputSignatureChanges(&msg, node, node_data, prev_state, stat_cache, digest_cache, queue->m_Config.m_ScanCache, config.m_ShaDigestExtensions, config.m_ShaDigestExtensionCount, thread_state);

        JsonWriteEndArray(&msg);
        JsonWriteEndObject(&msg);
        LogStructured(&msg);
      }

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

      if (IsStructuredLogActive())
      {
        MemAllocLinearScope allocScope(&thread_state->m_ScratchAlloc);

        JsonWriter msg;
        JsonWriteInit(&msg, &thread_state->m_ScratchAlloc);
        JsonWriteStartObject(&msg);

        JsonWriteKeyName(&msg, "msg");
        JsonWriteValueString(&msg, "nodeOutputsMissing");

        JsonWriteKeyName(&msg, "annotation");
        JsonWriteValueString(&msg, node_data->m_Annotation);

        JsonWriteKeyName(&msg, "index");
        JsonWriteValueInteger(&msg, node_data->m_OriginalIndex);

        JsonWriteKeyName(&msg, "files");
        JsonWriteStartArray(&msg);
        for (auto& f : node_data->m_OutputFiles)
        {
          FileInfo i = StatCacheStat(stat_cache, f.m_Filename, f.m_FilenameHash);
          if (!i.Exists())
            JsonWriteValueString(&msg, f.m_Filename);
        }
        JsonWriteEndArray(&msg);

        JsonWriteEndObject(&msg);
        LogStructured(&msg);
      }

      next_state = BuildProgress::kRunAction;
    }
    else if (OutputFileInTargetDirectoryMissing(node_data, thread_state))
    {
      if (IsStructuredLogActive())
      {
        MemAllocLinearScope allocScope(&thread_state->m_ScratchAlloc);

        JsonWriter msg;
        JsonWriteInit(&msg, &thread_state->m_ScratchAlloc);
        JsonWriteStartObject(&msg);

        JsonWriteKeyName(&msg, "msg");
        JsonWriteValueString(&msg, "nodeOutputInTargetDirectoryMissing");

        JsonWriteKeyName(&msg, "annotation");
        JsonWriteValueString(&msg, node_data->m_Annotation);

        JsonWriteKeyName(&msg, "index");
        JsonWriteValueInteger(&msg, node_data->m_OriginalIndex);

        JsonWriteEndObject(&msg);
        LogStructured(&msg);
      }

      next_state = BuildProgress::kRunAction;
    } else {
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

  static ExecResult WriteTextFile(const char* payload, const char* target_file, MemAllocHeap* heap)
  {
    ExecResult result;
    char tmpBuffer[1024];
    
    memset(&result, 0, sizeof(result));
    
    FILE* f = fopen(target_file, "wb");
    if (!f)
    {
      InitOutputBuffer(&result.m_OutputBuffer, heap);
      
      snprintf(tmpBuffer, sizeof(tmpBuffer), "Error opening for writing the file: %s, error: %s", target_file, strerror( errno ));
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

  static BuildProgress::Enum RunAction(BuildQueue* queue, ThreadState* thread_state, NodeState* node, Mutex* queue_lock)
  {
    const NodeData    *node_data    = node->m_MmapData;
    const bool        isWriteFileAction = node->m_MmapData->m_Flags & NodeData::kFlagIsWriteTextFileAction;
    const bool        dry_run       = (queue->m_Config.m_Flags & BuildQueueConfig::kFlagDryRun) != 0;
    const char        *cmd_line     = node_data->m_Action;
    const char        *pre_cmd_line = node_data->m_PreAction;

    if (!isWriteFileAction && (!cmd_line || cmd_line[0] == '\0'))
    {
      queue->m_ProcessedNodeCount++;
      return BuildProgress::kSucceeded;
    }

    if (node->m_MmapData->m_Flags & NodeData::kFlagExpensive && !dry_run)
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
    int                profiler_thread_id = thread_state->m_ProfilerThreadId;
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

    for (int i = 0; i < node_data->m_SharedResources.GetCount(); ++i)
    {
      if (!SharedResourceAcquire(queue, &thread_state->m_LocalHeap, node_data->m_SharedResources[i]))
      {
        Log(kError, "failed to create shared resource %s", queue->m_Config.m_SharedResources[node_data->m_SharedResources[i]].m_Annotation.Get());
        MutexLock(queue_lock);
        return BuildProgress::kFailed;
      }
    }


    if (IsStructuredLogActive())
    {
      MemAllocLinearScope allocScope(&thread_state->m_ScratchAlloc);

      JsonWriter msg;
      JsonWriteInit(&msg, &thread_state->m_ScratchAlloc);
      JsonWriteStartObject(&msg);

      JsonWriteKeyName(&msg, "msg");
      JsonWriteValueString(&msg, "nodestarted");

      JsonWriteKeyName(&msg, "annotation");
      JsonWriteValueString(&msg, node_data->m_Annotation);

      JsonWriteKeyName(&msg, "index");
      JsonWriteValueInteger(&msg, node_data->m_OriginalIndex);

      JsonWriteEndObject(&msg);
      LogStructured(&msg);
    }

    if (!dry_run)
    {
      auto EnsureParentDirExistsFor = [=](const FrozenFileAndHash& fileAndHash) -> bool {
          PathBuffer output;
          PathInit(&output, fileAndHash.m_Filename);

          if (!MakeDirectoriesForFile(stat_cache, output))
          {
            Log(kError, "failed to create output directories for %s", fileAndHash.m_Filename.Get());
            MutexLock(queue_lock);
            return false;
          }
          return true;
      };

      for (const FrozenFileAndHash& output_file : node_data->m_OutputFiles)
        if (!EnsureParentDirExistsFor(output_file))
          return BuildProgress::kFailed;

      for (const FrozenFileAndHash& output_file : node_data->m_AuxOutputFiles)
        if (!EnsureParentDirExistsFor(output_file))
          return BuildProgress::kFailed;
    
      for (const FrozenFileAndHash& output_dir : node_data->m_OutputDirectories)
      {
        PathBuffer path;
        PathInit(&path, output_dir.m_Filename);
        if (!MakeDirectoriesRecursive(stat_cache, path))
          return BuildProgress::kFailed;
      }
    }

    ExecResult result = { 0, false };

    // See if we need to remove the output files before running anything.
    if (0 == (node_data->m_Flags & NodeData::kFlagOverwriteOutputs) && !dry_run)
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

    size_t n_outputs = (size_t)node_data->m_OutputFiles.GetCount();

    bool* untouched_outputs = (bool*)LinearAllocate(&thread_state->m_ScratchAlloc, n_outputs, (size_t)sizeof(bool));
    memset(untouched_outputs, 0, n_outputs * sizeof(bool));

    bool requireFrontendRerun = false;

    if (pre_cmd_line)
    {
      Log(kSpam, "Launching pre-action process");
      TimingScope timing_scope(&g_Stats.m_ExecCount, &g_Stats.m_ExecTimeCycles);
      ProfilerScope prof_scope("Pre-build", profiler_thread_id);
      last_cmd_line = pre_cmd_line;
      if (!dry_run)
      {
        result = ExecuteProcess(pre_cmd_line, env_count, env_vars, thread_state->m_Queue->m_Config.m_Heap, job_id, false, SlowCallback, &slowCallbackData, 1);
        Log(kSpam, "Process return code %d", result.m_ReturnCode);
      }
    }

    ValidationResult passedOutputValidation = ValidationResult::Pass;
    if (0 == result.m_ReturnCode)
    {
      Log(kSpam, "Launching process");
      TimingScope timing_scope(&g_Stats.m_ExecCount, &g_Stats.m_ExecTimeCycles);
      ProfilerScope prof_scope(annotation, profiler_thread_id);

      if (!dry_run)
      {
        uint64_t* pre_timestamps = (uint64_t*)LinearAllocate(&thread_state->m_ScratchAlloc, n_outputs, (size_t)sizeof(uint64_t));

        bool allowUnwrittenOutputFiles = (node_data->m_Flags & NodeData::kFlagAllowUnwrittenOutputFiles);
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

        for (int i = 0; i < node_data->m_OutputDirectories.GetCount(); i++)
        {
          HashDigest hd = CalculateGlobSignatureFor(node_data->m_OutputDirectories[i].m_Filename, "*", true, &thread_state->m_LocalHeap, &thread_state->m_ScratchAlloc);
          
           HashDigest node_digest = node_data->m_OutputDirectoryGlobSignatures[i];
           if (hd != node_digest)
                requireFrontendRerun = true;
        }

        Log(kSpam, "Process return code %d", result.m_ReturnCode);
      }
    }

    for (const FrozenFileAndHash& output : node_data->m_OutputFiles)
    {
      StatCacheMarkDirty(stat_cache, output.m_Filename, output.m_FilenameHash);
    }

    MutexLock(queue_lock);
    PrintNodeResult(&result, node_data, last_cmd_line, thread_state->m_Queue, thread_state, echo_cmdline, time_of_start, passedOutputValidation, untouched_outputs);

    ExecResultFreeMemory(&result);

    if (result.m_WasAborted)
    {
      SignalSet("child processes was aborted");
    }

    if (0 == result.m_ReturnCode && passedOutputValidation < ValidationResult::UnexpectedConsoleOutputFail)
    {
      return requireFrontendRerun ? BuildProgress::kSucceededButDependendeesRequireFrontendRerun : BuildProgress::kSucceeded;
    }
    else
    {
      // Clean up output files after a failed build unless they are precious,
      // or unless the failure was from failing to write one of them
      if (0 == (NodeData::kFlagPreciousOutputs & node_data->m_Flags) &&
        !(0 == result.m_ReturnCode && passedOutputValidation == ValidationResult::UnwrittenOutputFileFail))
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
        const char* a = waiter->m_MmapData->m_Annotation.Get();
        Log(LogLevel::kInfo, "yeah %s\n", a);
        // Only wake nodes in our current pass
        if (waiter->m_MmapData->m_PassIndex != queue->m_CurrentPassIndex)
          continue;

        // Did someone else get to the node first?
        if (NodeStateIsQueued(waiter) || NodeStateIsActive(waiter))
          continue;

        // If the node isn't ready, skip it.
        if (!IsNodeReadyToBeAdvanced(queue, waiter))
          continue;

        //printf("%s is ready to go\n", GetSourceNode(queue, waiter)->m_Annotation);
        waiter->m_Progress = BuildProgress::kWaitingForDependencies;
        Enqueue(queue, waiter);
        
        ++enqueue_count;
      }
    }

    if (enqueue_count > 0)
      WakeWaiters(queue, enqueue_count);
  }

  static void WakeupAllBuildThreadsSoTheyCanExit(BuildQueue* queue)
  {
    //build threads are either waiting on m_WorkAvailable signal, or on m_MaxJobsChangedConditionalVariable. Let's send 'm both.
    CondBroadcast(&queue->m_WorkAvailable);
    CondBroadcast(&queue->m_MaxJobsChangedConditionalVariable);
  }

  static void SignalMainThreadToStartCleaningUp(BuildQueue* queue)
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

          if (BuildProgress::kWaitingForDependencies == node->m_Progress)
          {
            // Set ourselves as inactive until our dependencies are ready. note we do not requeue ourselves here.
            NodeStateFlagInactive(node);
            return;
          }
          break;

        case BuildProgress::kWaitingForDependencies:
          node->m_Progress = NextProgressForNodesWhoseDependenciesFinished(queue,node);
          break;

        case BuildProgress::kAllDependencesSucceeded:
          node->m_Progress = CheckInputSignature(queue, thread_state, node, queue_lock);
          break;

        case BuildProgress::kRunAction:
          node->m_Progress = RunAction(queue, thread_state, node, queue_lock);

          // If we couldn't make progress, we're a parked expensive node.
          // Another expensive job will put us back on the queue later when it
          // has finished.
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

          node->m_BuildResult = 1;
          node->m_Progress    = BuildProgress::kCompleted;

          SignalMainThreadToStartCleaningUp(queue);
          break;

        case BuildProgress::kSucceededButDependendeesRequireFrontendRerun:
          queue->m_RequireFrontendRerunNodeCount++;
          node->m_BuildResult = 0;
          //node->m_Progress = BuildProgress::kCompleted;
          //break;

        case BuildProgress::kPermanentlyBlocked:
        case BuildProgress::kCompleted:
          queue->m_PendingNodeCount--;
          
          UnblockWaiters(queue, node);

          if (queue->m_PendingNodeCount == 0)
            SignalMainThreadToStartCleaningUp(queue);

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

  static bool ShouldKeepBuilding(BuildQueue* queue)
  {
    // If we're quitting, definitely stop building.
    if (queue->m_MainThreadWantsToCleanUp)
      return false;


    //you'd think we don't have to check for this, as the main thread will realize the build has failed, and will shut us down,
    //but if we don't check this in the buildloop, we'll actually continue to build nodes whose dependencies have failed.
    if (queue->m_FailedNodeCount > 0)
      return false;

    return true;
  }
  
  static void BuildLoop(ThreadState* thread_state)
  {
    BuildQueue        *queue = thread_state->m_Queue;
    ConditionVariable *cv    = &queue->m_WorkAvailable;
    Mutex             *mutex = &queue->m_Lock;

    MutexLock(mutex);
    bool waitingForWork = false;

    auto HibernateForThrottlingIfRequired = [=]() {
      //check if dynamic max jobs amount has been reduced to a point where we need this thread to hibernate.
      //Don't take a mutex lock for this check, as this if check will almost never hit and it's in a perf critical loop.
      if (thread_state->m_ThreadIndex < queue->m_DynamicMaxJobs)
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

      if (NodeState * node = NextNode(queue))
      {
        if (waitingForWork)
        {
          ProfilerEnd(thread_state->m_ProfilerThreadId);
          waitingForWork = false;
        }
        AdvanceNode(queue, thread_state, node, mutex);
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
#if WIN32
      Sleep(10);
#endif
    }

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
    CondInit(&queue->m_MaxJobsChangedConditionalVariable);
    CondInit(&queue->m_BuildFinishedConditionalVariable);
    MutexInit(&queue->m_BuildFinishedMutex);
    MutexLock(&queue->m_BuildFinishedMutex);

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
    queue->m_RequireFrontendRerunNodeCount    = 0;
    queue->m_ProcessedNodeCount = 0;
    queue->m_MainThreadWantsToCleanUp = false;
    queue->m_BuildFinishedConditionalVariableSignaled = false;
    queue->m_ExpensiveRunning   = 0;
    queue->m_ExpensiveWaitCount = 0;
    queue->m_ExpensiveWaitList  = HeapAllocateArray<NodeState*>(heap, capacity);
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
      ThreadState* thread_state = &queue->m_ThreadState[i];

      //the profiler thread id here is "i+1",  since if we have 4 buildthreads, we'll have 5 total threads, as the main thread doesn't participate in building, but only sleeps
      //and pumps the OS messageloop.
      ThreadStateInit(thread_state, queue, MB(32), i, i+1);

      Log(kDebug, "starting build thread %d", i);
      queue->m_Threads[i] = ThreadStart(BuildThreadRoutine, thread_state);
    }
  }

  void BuildQueueDestroy(BuildQueue* queue)
  {
    Log(kDebug, "destroying build queue");
    const BuildQueueConfig* config = &queue->m_Config;

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
    MemAllocHeap* heap = queue->m_Config.m_Heap;
    HeapFree(heap, queue->m_ExpensiveWaitList);
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

  static void SetNewDynamicMaxJobs(BuildQueue* queue, int maxJobs, const char* formatString, ...)
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

  static void ProcessThrottling(BuildQueue* queue)
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
    else if (queue->m_FailedNodeCount)
      return BuildResult::kBuildError;
    else if (queue->m_RequireFrontendRerunNodeCount)
      return BuildResult::kRequireFrontendRerun;
    else
      return BuildResult::kOk;
  }
}

