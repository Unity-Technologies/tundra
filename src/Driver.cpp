#include "Driver.hpp"
#include "BinaryWriter.hpp"
#include "Buffer.hpp"
#include "BuildQueue.hpp"
#include "Common.hpp"
#include "DagData.hpp"
#include "DagGenerator.hpp"
#include "DagDerivedCompiler.hpp"
#include "FileInfo.hpp"
#include "MemAllocLinear.hpp"
#include "MemoryMappedFile.hpp"
#include "RuntimeNode.hpp"
#include "ScanData.hpp"
#include "Scanner.hpp"
#include "SortedArrayUtil.hpp"
#include "AllBuiltNodes.hpp"
#include "Stats.hpp"
#include "HashTable.hpp"
#include "Hash.hpp"
#include "Profiler.hpp"
#include "NodeResultPrinting.hpp"
#include "FileSign.hpp"
#include "DynamicOutputDirectories.hpp"
#include "PathUtil.hpp"
#include "CacheClient.hpp"
#include "LeafInputSignature.hpp"
#include "LoadFrozenData.hpp"
#include "FindNodesByName.hpp"

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "stdarg.h"

TundraStats g_Stats;

static const char *s_BuildFile;
static const char *s_DagFileName;

static bool DriverCheckDagSignatures(Driver *self, char *out_of_date_reason, int out_of_date_reason_maxlength);
bool LoadOrBuildDag(Driver *self, const char *dag_fn);


void DriverInitializeTundraFilePaths(DriverOptions *driverOptions)
{
    s_BuildFile = "tundra.lua";
    s_DagFileName = driverOptions->m_DAGFileName;
}

// Set default options.
void DriverOptionsInit(DriverOptions *self)
{
    self->m_ShowHelp = false;
    self->m_ShowTargets = false;
    self->m_DebugMessages = false;
    self->m_Verbose = false;
    self->m_SpammyVerbose = false;
    self->m_DisplayStats = false;
    self->m_SilenceIfPossible = false;
    self->m_DontReusePreviousResults = false;
    self->m_DebugSigning = false;
    self->m_JustPrintLeafInputSignature = nullptr;
    self->m_IdentificationColor = 0;
    self->m_ThreadCount = GetCpuCount();
    self->m_WorkingDir = nullptr;
    self->m_DAGFileName = ".tundra2.dag";
    self->m_ProfileOutput = nullptr;
    self->m_IncludesOutput = nullptr;
    self->m_VisualMaxNodes = 1000;
#if defined(TUNDRA_WIN32)
    self->m_RunUnprotected = true;
#endif
}


void DriverShowTargets(Driver *self)
{
    const Frozen::Dag *dag = self->m_DagData;

    printf("\nNamed nodes and aliases:\n");
    printf("----------------------------------------------------------------\n");

    int32_t count = dag->m_NamedNodes.GetCount();
    const char **temp = (const char **)alloca(sizeof(const char *) * count);
    for (int i = 0; i < count; ++i)
    {
        temp[i] = dag->m_NamedNodes[i].m_Name.Get();
    }
    std::sort(temp, temp + count, [](const char *a, const char *b) { return strcmp(a, b) < 0; });

    for (int i = 0; i < count; ++i)
    {
        printf(" - %s\n", temp[i]);
    }
}

static void GetIncludesRecursive(const HashDigest &scannerGuid, const char *fn, uint32_t fnHash, const Frozen::ScanData *scan_data, int depth, HashTable<HashDigest, kFlagPathStrings> &seen, HashSet<kFlagPathStrings> &direct)
{
    if (depth == 0 && !HashSetLookup(&direct, fnHash, fn))
        HashSetInsert(&direct, fnHash, fn);

    if (HashTableLookup(&seen, fnHash, fn))
        return;
    HashTableInsert(&seen, fnHash, fn, scannerGuid);

    HashDigest scan_key;
    ComputeScanCacheKey(&scan_key, fn, scannerGuid, false);

    const int32_t count = scan_data->m_EntryCount;
    if (const HashDigest *ptr = BinarySearch(scan_data->m_Keys.Get(), count, scan_key))
    {
        int index = int(ptr - scan_data->m_Keys.Get());
        const Frozen::ScanCacheEntry *entry = scan_data->m_Data.Get() + index;
        int file_count = entry->m_IncludedFiles.GetCount();
        for (int i = 0; i < file_count; ++i)
        {
            GetIncludesRecursive(scannerGuid, entry->m_IncludedFiles[i].m_Filename.Get(), entry->m_IncludedFiles[i].m_FilenameHash, scan_data, depth + 1, seen, direct);
        }
    }
}

bool DriverReportIncludes(Driver *self)
{
    MemAllocLinearScope allocScope(&self->m_Allocator);

    const Frozen::Dag *dag = self->m_DagData;
    if (dag == nullptr)
    {
        Log(kError, "No build DAG data");
        return false;
    }

    const Frozen::ScanData *scan_data = self->m_ScanData;
    if (scan_data == nullptr)
    {
        Log(kError, "No build file scan data (there was no previous build done?)");
        return false;
    }

    // For each file, we have to remember which include scanner hash digest was used.
    HashTable<HashDigest, kFlagPathStrings> seen;
    HashTableInit(&seen, &self->m_Heap);
    // Which files were directly compiled in DAG? all others are included indirectly.
    HashSet<kFlagPathStrings> direct;
    HashSetInit(&direct, &self->m_Heap);

    // Crawl the DAG and include scanner data to find all direct and indirect files.
    int node_count = dag->m_NodeCount;
    for (int i = 0; i < node_count; ++i)
    {
        const Frozen::DagNode &node = dag->m_DagNodes[i];


        if (node.m_ScannerIndex != -1 && node.m_InputFiles.GetCount() > 0)
        {
            const char *fn = node.m_InputFiles[0].m_Filename.Get();
            uint32_t fnHash = node.m_InputFiles[0].m_FilenameHash;
            const Frozen::ScannerData *s = dag->m_Scanners[node.m_ScannerIndex];
            GetIncludesRecursive(s->m_ScannerGuid, fn, fnHash, scan_data, 0, seen, direct);
        }
    }

    // Create JSON structure of includes report.
    JsonWriter msg;
    JsonWriteInit(&msg, &self->m_Allocator);
    JsonWriteStartObject(&msg);

    JsonWriteKeyName(&msg, "dagFile");
    JsonWriteValueString(&msg, self->m_Options.m_DAGFileName);

    JsonWriteKeyName(&msg, "files");
    JsonWriteStartArray(&msg);
    JsonWriteNewline(&msg);

    HashTableWalk(&seen, [&](uint32_t index, uint32_t hash, const char *filename, const HashDigest &scannerguid) {
        HashDigest scan_key;
        ComputeScanCacheKey(&scan_key, filename, scannerguid, false);
        const int32_t count = scan_data->m_EntryCount;
        if (const HashDigest *ptr = BinarySearch(scan_data->m_Keys.Get(), count, scan_key))
        {
            int index = int(ptr - scan_data->m_Keys.Get());
            const Frozen::ScanCacheEntry *entry = scan_data->m_Data.Get() + index;
            int file_count = entry->m_IncludedFiles.GetCount();
            JsonWriteStartObject(&msg);
            JsonWriteKeyName(&msg, "file");
            JsonWriteValueString(&msg, filename);
            if (HashSetLookup(&direct, hash, filename))
            {
                JsonWriteKeyName(&msg, "direct");
                JsonWriteValueInteger(&msg, 1);
            }
            JsonWriteKeyName(&msg, "includes");
            JsonWriteStartArray(&msg);
            JsonWriteNewline(&msg);
            for (int i = 0; i < file_count; ++i)
            {
                const char *fn = entry->m_IncludedFiles[i].m_Filename.Get();
                JsonWriteValueString(&msg, fn);
                JsonWriteNewline(&msg);
            }
            JsonWriteEndArray(&msg);
            JsonWriteEndObject(&msg);
        }
    });

    JsonWriteEndArray(&msg);
    JsonWriteEndObject(&msg);

    // Write into file.
    FILE *f = fopen(self->m_Options.m_IncludesOutput, "w");
    if (!f)
    {
        Log(kError, "Failed to create includes report file '%s'", self->m_Options.m_IncludesOutput);
        return false;
    }
    JsonWriteToFile(&msg, f);
    fclose(f);

    HashTableDestroy(&seen);
    HashSetDestroy(&direct);

    return true;
}

void DriverReportStartup(Driver *self, const char **targets, int target_count)
{
    MemAllocLinearScope allocScope(&self->m_Allocator);

    JsonWriter msg;
    JsonWriteInit(&msg, &self->m_Allocator);
    JsonWriteStartObject(&msg);

    JsonWriteKeyName(&msg, "msg");
    JsonWriteValueString(&msg, "init");

    JsonWriteKeyName(&msg, "dagFile");
    JsonWriteValueString(&msg, self->m_Options.m_DAGFileName);

    JsonWriteKeyName(&msg, "targets");
    JsonWriteStartArray(&msg);
    for (int i = 0; i < target_count; ++i)
        JsonWriteValueString(&msg, targets[i]);
    JsonWriteEndArray(&msg);

    JsonWriteEndObject(&msg);

    LogStructured(&msg);
}

bool DriverInitData(Driver *self)
{
    if (!LoadOrBuildDag(self, s_DagFileName))
        return false;

    ProfilerScope prof_scope("DriverInitData", 0);
    // do not produce/overwrite structured log output file,
    // if we're only reporting something and not doing an actual build
    if (self->m_Options.m_IncludesOutput == nullptr && !self->m_Options.m_ShowHelp && !self->m_Options.m_ShowTargets)
        SetStructuredLogFileName(self->m_DagData->m_StructuredLogFileName);

    DigestCacheInit(&self->m_DigestCache, MB(128), self->m_DagData->m_DigestCacheFileName);

    LoadFrozenData<Frozen::AllBuiltNodes>(self->m_DagData->m_StateFileName, &self->m_StateFile, &self->m_AllBuiltNodes);

    LoadFrozenData<Frozen::ScanData>(self->m_DagData->m_ScanCacheFileName, &self->m_ScanFile, &self->m_ScanData);

    ScanCacheSetCache(&self->m_ScanCache, self->m_ScanData);

    return true;
}


void DriverSelectNodes(const Frozen::Dag *dag, const char **targets, int target_count, Buffer<int32_t> *out_nodes, MemAllocHeap *heap)
{
    if (target_count > 0)
    {
        FindNodesByName(
            dag,
            out_nodes, heap,
            targets, target_count, dag->m_NamedNodes);
    }
    else
    {
        BufferAppend(out_nodes, heap, dag->m_DefaultNodes.GetArray(), dag->m_DefaultNodes.GetCount());
    }

    std::sort(out_nodes->begin(), out_nodes->end());
    int32_t *new_end = std::unique(out_nodes->begin(), out_nodes->end());
    out_nodes->m_Size = new_end - out_nodes->begin();
    Log(kDebug, "Node selection finished with %d nodes to build", (int)out_nodes->m_Size);
}

bool DriverPrepareNodes(Driver *self)
{
    ProfilerScope prof_scope("Tundra PrepareNodes", 0);

    const Frozen::Dag *dag = self->m_DagData;
    const Frozen::DagNode *dag_nodes = dag->m_DagNodes;
    const HashDigest *dag_node_guids = dag->m_NodeGuids;

    // Allocate space for nodes
    RuntimeNode *out_nodes = BufferAllocZero(&self->m_RuntimeNodes, &self->m_Heap, dag->m_NodeCount);

    int node_count = dag->m_NodeCount;

    // Initialize node state
    for (int i = 0; i < node_count; ++i)
    {
        const Frozen::DagNode *dag_node = dag_nodes + i;
        out_nodes[i].m_DagNode = dag_node;
        out_nodes[i].m_DagNodeIndex = i;
#if ENABLED(CHECKED_BUILD)
        out_nodes[i].m_DebugAnnotation = dag_node->m_Annotation.Get();
#endif
    }

    // Find frozen node state from previous build, if present.
    if (const Frozen::AllBuiltNodes *all_built_nodes = self->m_AllBuiltNodes)
    {
        const Frozen::BuiltNode *built_nodes = all_built_nodes->m_BuiltNodes;
        const HashDigest *state_guids = all_built_nodes->m_NodeGuids;
        const int state_guid_count = all_built_nodes->m_NodeCount;

        for (int i = 0; i < node_count; ++i)
        {
            const HashDigest *src_guid = dag_node_guids + i;
            if (const HashDigest *old_guid = BinarySearch(state_guids, state_guid_count, *src_guid))
            {
                int state_index = int(old_guid - state_guids);
                out_nodes[i].m_BuiltNode = built_nodes + state_index;
            }
        }
    }


    return true;
}

bool DriverInit(Driver *self, const DriverOptions *options)
{
    memset(self, 0, sizeof(Driver));
    HeapInit(&self->m_Heap);
    LinearAllocInit(&self->m_Allocator, &self->m_Heap, MB(64), "Driver Linear Allocator");

    LinearAllocSetOwner(&self->m_Allocator, ThreadCurrent());

    InitNodeResultPrinting(options);

    MmapFileInit(&self->m_DagFile);
    MmapFileInit(&self->m_StateFile);
    MmapFileInit(&self->m_ScanFile);


    self->m_DagData = nullptr;
    self->m_AllBuiltNodes = nullptr;
    self->m_ScanData = nullptr;

    BufferInit(&self->m_RuntimeNodes);

    self->m_Options = *options;

    // This linear allocator is only accessed when the state cache is locked.
    LinearAllocInit(&self->m_ScanCacheAllocator, &self->m_Heap, MB(64), "scan cache");
    ScanCacheInit(&self->m_ScanCache, &self->m_Heap, &self->m_ScanCacheAllocator);

    // This linear allocator is only accessed when the state cache is locked.
    LinearAllocInit(&self->m_StatCacheAllocator, &self->m_Heap, MB(64), "stat cache");
    StatCacheInit(&self->m_StatCache, &self->m_StatCacheAllocator, &self->m_Heap);

    return true;
}

void DriverDestroy(Driver *self)
{
    DigestCacheDestroy(&self->m_DigestCache);

    StatCacheDestroy(&self->m_StatCache);

    ScanCacheDestroy(&self->m_ScanCache);

    for (auto &node: self->m_RuntimeNodes)
    {
        if (node.m_CurrentLeafInputSignature != nullptr)
            DestroyLeafInputSignatureData(&self->m_Heap, node.m_CurrentLeafInputSignature);
        if (HashSetIsInitialized(&node.m_ImplicitInputs))
            HashSetDestroy(&node.m_ImplicitInputs);
    }

    BufferDestroy(&self->m_RuntimeNodes, &self->m_Heap);

    MmapFileDestroy(&self->m_ScanFile);
    MmapFileDestroy(&self->m_StateFile);
    MmapFileDestroy(&self->m_DagFile);

    LinearAllocDestroy(&self->m_ScanCacheAllocator);
    LinearAllocDestroy(&self->m_StatCacheAllocator);
    LinearAllocDestroy(&self->m_Allocator, true);
    HeapDestroy(&self->m_Heap);
}

bool DriverPrepareDag(Driver *self, const char *dag_fn);
bool DriverAllocNodes(Driver *self);



BuildResult::Enum DriverBuild(Driver *self, int* out_finished_node_count, const char** argv, int argc)
{
    const Frozen::Dag *dag = self->m_DagData;

    // Initialize build queue
    Mutex debug_signing_mutex;

    BuildQueueConfig queue_config;
    queue_config.m_DriverOptions = &self->m_Options;
    queue_config.m_Flags = 0;
    queue_config.m_Heap = &self->m_Heap;
    queue_config.m_LinearAllocator = &self->m_Allocator;
    queue_config.m_Dag = self->m_DagData;
    queue_config.m_DagNodes = self->m_DagData->m_DagNodes;
    queue_config.m_DagDerived = self->m_DagDerivedData;
    queue_config.m_ScanCache = &self->m_ScanCache;
    queue_config.m_StatCache = &self->m_StatCache;
    queue_config.m_DigestCache = &self->m_DigestCache;
    queue_config.m_ShaDigestExtensionCount = dag->m_ShaExtensionHashes.GetCount();
    queue_config.m_ShaDigestExtensions = dag->m_ShaExtensionHashes.GetArray();
    queue_config.m_SharedResources = dag->m_SharedResources.GetArray();
    queue_config.m_SharedResourcesCount = dag->m_SharedResources.GetCount();
    BufferInit(&queue_config.m_RequestedNodes);

    GetCachingBehaviourSettingsFromEnvironment(&queue_config.m_AttemptCacheReads, &queue_config.m_AttemptCacheWrites);

    DagRuntimeDataInit(&queue_config.m_DagRuntimeData, self->m_DagData, &self->m_Heap);

    if (self->m_Options.m_Verbose)
    {
        queue_config.m_Flags |= BuildQueueConfig::kFlagEchoCommandLines;
    }

    if (self->m_Options.m_DebugSigning)
    {
        MutexInit(&debug_signing_mutex);
        queue_config.m_FileSigningLogMutex = &debug_signing_mutex;
        queue_config.m_FileSigningLog = fopen("signing-debug.txt", "w");
    }
    else
    {
        queue_config.m_FileSigningLogMutex = nullptr;
        queue_config.m_FileSigningLog = nullptr;
    }

    BuildResult::Enum build_result = BuildResult::kOk;

    // Prepare list of nodes to build/clean/rebuild
    if (!DriverPrepareNodes(self))
    {
        Log(kError, "couldn't set up list of targets to build");
        build_result = BuildResult::kBuildError;
        goto leave;
    }

    BuildQueue build_queue;
    BuildQueueInit(&build_queue, &queue_config,(const char**)argv, argc);
    build_queue.m_Config.m_RuntimeNodes = self->m_RuntimeNodes.m_Storage;
    build_queue.m_Config.m_TotalRuntimeNodeCount = (int)self->m_RuntimeNodes.m_Size;

    if (self->m_Options.m_JustPrintLeafInputSignature)
    {
        PrintLeafInputSignature(&build_queue, self->m_Options.m_JustPrintLeafInputSignature);
        goto leave;
    }

    build_result = BuildQueueBuild(&build_queue, &self->m_Allocator);

    if (self->m_Options.m_DebugSigning)
    {
        fclose((FILE *)queue_config.m_FileSigningLog);
        MutexDestroy(&debug_signing_mutex);
    }

    *out_finished_node_count = build_queue.m_FinishedNodeCount;

leave:
    // Shut down build queue
    BuildQueueDestroy(&build_queue);

    DagRuntimeDataDestroy(&queue_config.m_DagRuntimeData);

    return build_result;
}

// Save scan cache
bool DriverSaveScanCache(Driver *self)
{
    ScanCache *scan_cache = &self->m_ScanCache;

    if (!ScanCacheDirty(scan_cache))
        return true;

    // This will be invalidated.
    self->m_ScanData = nullptr;

    bool success = ScanCacheSave(scan_cache, self->m_DagData->m_ScanCacheFileNameTmp, &self->m_Heap);

    // Unmap the file so we can overwrite it (on Windows.)
    MmapFileDestroy(&self->m_ScanFile);

    if (success)
    {
        success = RenameFile(self->m_DagData->m_ScanCacheFileNameTmp, self->m_DagData->m_ScanCacheFileName);
    }
    else
    {
        remove(self->m_DagData->m_ScanCacheFileNameTmp);
    }

    return success;
}

// Save digest cache
bool DriverSaveDigestCache(Driver *self)
{
    // This will be invalidated.
    return DigestCacheSave(&self->m_DigestCache, &self->m_Heap, self->m_DagData->m_DigestCacheFileName, self->m_DagData->m_DigestCacheFileNameTmp);
}


bool node_was_used_by_this_dag_previously(const Frozen::BuiltNode *previously_built_node, uint32_t current_dag_identifier)
{
    auto &previous_dags = previously_built_node->m_DagsWeHaveSeenThisNodeInPreviously;
    return std::find(previous_dags.begin(), previous_dags.end(), current_dag_identifier) != previous_dags.end();
}



