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
#include <stdarg.h>

#include <stdio.h>

static void CheckAndReportChangedInputFile(
    JsonWriter *msg,
    const char *filename,
    uint32_t filenameHash,
    uint64_t lastTimestamp,
    const char *dependencyType,
    DigestCache *digest_cache,
    StatCache *stat_cache,
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

static void ReportChangedInputFiles(JsonWriter *msg, const FrozenArray<Frozen::NodeInputFileData> &files, const char *dependencyType, DigestCache *digest_cache, StatCache *stat_cache, const uint32_t sha_extension_hashes[], uint32_t sha_extension_hash_count, bool force_use_timestamp)
{
    for (const Frozen::NodeInputFileData &input : files)
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

static void ReportValueWithOptionalTruncation(JsonWriter *msg, const char *keyName, const char *truncatedKeyName, const FrozenString &value)
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
    JsonWriter *msg,
    RuntimeNode *node,
    const Frozen::DagNode *node_data,
    const Frozen::BuiltNode *prev_state,
    StatCache *stat_cache,
    DigestCache *digest_cache,
    ScanCache *scan_cache,
    const uint32_t sha_extension_hashes[],
    int sha_extension_hash_count,
    ThreadState *thread_state)
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

    bool explicitInputFilesListChanged = node_data->m_InputFiles.GetCount() != prev_state->m_InputFiles.GetCount();
    for (int32_t i = 0; i < node_data->m_InputFiles.GetCount() && !explicitInputFilesListChanged; ++i)
    {
        const char *filename = node_data->m_InputFiles[i].m_Filename;
        const char *oldFilename = prev_state->m_InputFiles[i].m_Filename;
        explicitInputFilesListChanged |= (strcmp(filename, oldFilename) != 0);
    }
    bool force_use_timestamp = node->m_Flags & Frozen::DagNode::kFlagBanContentDigestForInputs;
    if (explicitInputFilesListChanged)
    {
        JsonWriteStartObject(msg);

        JsonWriteKeyName(msg, "key");
        JsonWriteValueString(msg, "InputFileList");

        JsonWriteKeyName(msg, "value");
        JsonWriteStartArray(msg);
        for (const FrozenFileAndHash &input : node_data->m_InputFiles)
            JsonWriteValueString(msg, input.m_Filename);
        JsonWriteEndArray(msg);

        JsonWriteKeyName(msg, "oldvalue");
        JsonWriteStartArray(msg);
        for (const Frozen::NodeInputFileData &input : prev_state->m_InputFiles)
            JsonWriteValueString(msg, input.m_Filename);
        JsonWriteEndArray(msg);

        JsonWriteKeyName(msg, "dependency");
        JsonWriteValueString(msg, "explicit");

        JsonWriteEndObject(msg);

        // We also want to catch if any of the input files (common to both old + new lists) have changed themselves,
        // because a common reason for the input list changing is the command changing, and the part of the
        // command that is different may be in response file(s).
        for (const Frozen::NodeInputFileData &oldInput : prev_state->m_InputFiles)
        {
            const FrozenFileAndHash *newInput;
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
                                           force_use_timestamp);
        }

        // Don't do any further checking for changes, there's little point scanning implicit dependencies
        return;
    }

    ReportChangedInputFiles(msg, prev_state->m_InputFiles, "explicit", digest_cache, stat_cache, sha_extension_hashes, sha_extension_hash_count, force_use_timestamp);

    if (node_data->m_Scanner)
    {
        HashTable<bool, kFlagPathStrings> implicitDependencies;
        HashTableInit(&implicitDependencies, &thread_state->m_LocalHeap);

        for (const FrozenFileAndHash &input : node_data->m_InputFiles)
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
                    const FileAndHash &path = scan_output.m_IncludedFiles[i];
                    if (HashTableLookup(&implicitDependencies, path.m_FilenameHash, path.m_Filename) == nullptr)
                        HashTableInsert(&implicitDependencies, path.m_FilenameHash, path.m_Filename, false);
                }
            }
        }

        bool implicitFilesListChanged = implicitDependencies.m_RecordCount != prev_state->m_ImplicitInputFiles.GetCount();
        if (!implicitFilesListChanged)
        {
            for (const Frozen::NodeInputFileData &implicitInput : prev_state->m_ImplicitInputFiles)
            {
                bool *visited = HashTableLookup(&implicitDependencies, Djb2HashPath(implicitInput.m_Filename), implicitInput.m_Filename);
                if (!visited)
                {
                    implicitFilesListChanged = true;
                    break;
                }

                *visited = true;
            }

            HashTableWalk(&implicitDependencies, [&](int32_t index, uint32_t hash, const char *filename, bool visited) {
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
            HashTableWalk(&implicitDependencies, [=](int32_t index, uint32_t hash, const char *filename, bool visited) {
                JsonWriteValueString(msg, filename);
            });
            JsonWriteEndArray(msg);

            JsonWriteKeyName(msg, "oldvalue");
            JsonWriteStartArray(msg);
            for (const Frozen::NodeInputFileData &input : prev_state->m_ImplicitInputFiles)
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

static bool OutputFilesDiffer(const Frozen::DagNode *node_data, const Frozen::BuiltNode *prev_state)
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

static bool OutputFilesMissing(StatCache *stat_cache, const Frozen::DagNode *node)
{
    for (const FrozenFileAndHash &f : node->m_OutputFiles)
    {
        FileInfo i = StatCacheStat(stat_cache, f.m_Filename, f.m_FilenameHash);

        if (!i.Exists())
            return true;
    }

    for (const FrozenFileAndHash &f : node->m_OutputDirectories)
    {
        FileInfo i = StatCacheStat(stat_cache, f.m_Filename, f.m_FilenameHash);

        if (!i.IsDirectory())
            return true;
    }

    return false;
}

bool CheckInputSignatureToSeeNodeNeedsExecuting(BuildQueue *queue, ThreadState *thread_state, RuntimeNode *node)
{
    const Frozen::DagNode *node_data = node->m_DagNode;

    ProfilerScope prof_scope("CheckInputSignature", thread_state->m_ProfilerThreadId, node_data->m_Annotation);

    const BuildQueueConfig &config = queue->m_Config;
    StatCache *stat_cache = config.m_StatCache;
    DigestCache *digest_cache = config.m_DigestCache;

    HashState sighash;
    FILE *debug_log = (FILE *)queue->m_Config.m_FileSigningLog;

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

    const Frozen::ScannerData *scanner = node_data->m_Scanner;

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

    bool force_use_timestamp = node_data->m_Flags & Frozen::DagNode::kFlagBanContentDigestForInputs;

    // Roll back scratch allocator after all file scans
    MemAllocLinearScope alloc_scope(&thread_state->m_ScratchAlloc);

    for (const FrozenFileAndHash &input : node_data->m_InputFiles)
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
            scan_input.m_ScratchAlloc = &thread_state->m_ScratchAlloc;
            scan_input.m_ScratchHeap = &thread_state->m_LocalHeap;
            scan_input.m_FileName = input.m_Filename;
            scan_input.m_ScanCache = queue->m_Config.m_ScanCache;

            ScanOutput scan_output;

            if (ScanImplicitDeps(stat_cache, &scan_input, &scan_output))
            {
                for (int i = 0, count = scan_output.m_IncludedFileCount; i < count; ++i)
                {
                    const FileAndHash &path = scan_output.m_IncludedFiles[i];
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
        HashSetWalk(&implicitDeps, [&](uint32_t, uint32_t hash, const char *filename) {
            HashAddPath(&sighash, filename);
            ComputeFileSignature(
                &sighash,
                stat_cache,
                digest_cache,
                filename,
                hash,
                config.m_ShaDigestExtensions,
                config.m_ShaDigestExtensionCount,
                force_use_timestamp);
        });

        HashSetDestroy(&implicitDeps);
    }

    for (const FrozenString &input : node_data->m_AllowedOutputSubstrings)
        HashAddString(&sighash, (const char *)input);

    HashAddInteger(&sighash, (node_data->m_Flags & Frozen::DagNode::kFlagAllowUnexpectedOutput) ? 1 : 0);
    HashAddInteger(&sighash, (node_data->m_Flags & Frozen::DagNode::kFlagAllowUnwrittenOutputFiles) ? 1 : 0);

    HashFinalize(&sighash, &node->m_InputSignature);

    if (debug_log)
    {
        char sig[kDigestStringSize];
        DigestToString(sig, node->m_InputSignature);
        fprintf(debug_log, "  => %s\n", sig);
        MutexUnlock(queue->m_Config.m_FileSigningLogMutex);
    }

    // Figure out if we need to rebuild this node.
    const Frozen::BuiltNode *prev_nodestatedata = node->m_BuiltNode;

    if (!prev_nodestatedata)
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

        return true;
    }

    if (prev_nodestatedata->m_InputSignature != node->m_InputSignature)
    {
        // The input signature has changed (either direct inputs or includes)
        // We need to rebuild this node.
        char oldDigest[kDigestStringSize];
        char newDigest[kDigestStringSize];
        DigestToString(oldDigest, prev_nodestatedata->m_InputSignature);
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

            ReportInputSignatureChanges(&msg, node, node_data, prev_nodestatedata, stat_cache, digest_cache, queue->m_Config.m_ScanCache, config.m_ShaDigestExtensions, config.m_ShaDigestExtensionCount, thread_state);

            JsonWriteEndArray(&msg);
            JsonWriteEndObject(&msg);
            LogStructured(&msg);
        }

        return true;
    }

    else if (!prev_nodestatedata->m_WasBuiltSuccessfully)
    {
        // The build progress failed the last time around - we need to retry it.
        Log(kSpam, "T=%d: building %s - previous build failed", thread_state->m_ThreadIndex, node_data->m_Annotation.Get());

        if (IsStructuredLogActive())
        {
            MemAllocLinearScope allocScope(&thread_state->m_ScratchAlloc);

            JsonWriter msg;
            JsonWriteInit(&msg, &thread_state->m_ScratchAlloc);
            JsonWriteStartObject(&msg);

            JsonWriteKeyName(&msg, "msg");
            JsonWriteValueString(&msg, "nodeRetryBuild");

            JsonWriteKeyName(&msg, "annotation");
            JsonWriteValueString(&msg, node_data->m_Annotation);

            JsonWriteKeyName(&msg, "index");
            JsonWriteValueInteger(&msg, node_data->m_OriginalIndex);

            JsonWriteEndObject(&msg);
            LogStructured(&msg);
        }

        return true;
    }

    if (OutputFilesDiffer(node_data, prev_nodestatedata))
    {
        // The output files are different - need to rebuild.
        Log(kSpam, "T=%d: building %s - output files have changed", thread_state->m_ThreadIndex, node_data->m_Annotation.Get());
        return true;
    }

    if (OutputFilesMissing(stat_cache, node_data))
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
            for (auto &f : node_data->m_OutputFiles)
            {
                FileInfo i = StatCacheStat(stat_cache, f.m_Filename, f.m_FilenameHash);
                if (!i.Exists())
                    JsonWriteValueString(&msg, f.m_Filename);
            }
            JsonWriteEndArray(&msg);

            JsonWriteKeyName(&msg, "directories");
            JsonWriteStartArray(&msg);
            for (auto &f : node_data->m_OutputDirectories)
            {
                FileInfo i = StatCacheStat(stat_cache, f.m_Filename, f.m_FilenameHash);

                if (!i.IsDirectory())
                    JsonWriteValueString(&msg, f.m_Filename);
            }
            JsonWriteEndArray(&msg);

            JsonWriteEndObject(&msg);
            LogStructured(&msg);
        }

        return true;
    }

    // Everything is up to date
    return false;
}

