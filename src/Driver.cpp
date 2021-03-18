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
#include "MakeDirectories.hpp"
#include "NodeResultPrinting.hpp"
#include "FileSign.hpp"
#include "PathUtil.hpp"
#include "CacheClient.hpp"
#include "LeafInputSignature.hpp"
#include "LoadFrozenData.hpp"
#include "FindNodesByName.hpp"
#include "FileSystem.hpp"

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "stdarg.h"

TundraStats g_Stats;

static const char *s_BuildFile;
static const char *s_DagFileName;

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
    self->m_ContinueOnFailure = false;
    self->m_StandardInputCanary = false;
    self->m_JustPrintLeafInputSignature = nullptr;
    self->m_IdentificationColor = 0;
    self->m_ThreadCount = GetCpuCount();
    self->m_WorkingDir = nullptr;
    self->m_DAGFileName = ".tundra2.dag";
    self->m_ProfileOutput = nullptr;
    self->m_IncludesOutput = nullptr;
    self->m_VisualMaxNodes = 1000;
    self->m_DagFileNameJson = nullptr;
#if defined(TUNDRA_WIN32)
    self->m_RunUnprotected = true;
#endif
    self->m_Inspect = false;
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

    // do not produce/overwrite structured log output or state file,
    // if we're only reporting something and not doing an actual build
    if (self->m_Options.m_IncludesOutput == nullptr && !self->m_Options.m_ShowHelp && !self->m_Options.m_ShowTargets)
    {
        SetStructuredLogFileName(self->m_DagData->m_StructuredLogFileName);

        const char* stateFile = self->m_DagData->m_StateFileName.Get();
        if (GetFileInfo(stateFile).Exists() && !RenameFile(stateFile, self->m_DagData->m_StateFileNameMapped.Get()))
            Croak("Unable to rename state file '%s' => '%s'", stateFile, self->m_DagData->m_StateFileNameMapped.Get());
        LoadFrozenData<Frozen::AllBuiltNodes>(self->m_DagData->m_StateFileNameMapped, &self->m_StateFile, &self->m_AllBuiltNodes);
    }
    else
    {
        LoadFrozenData<Frozen::AllBuiltNodes>(self->m_DagData->m_StateFileName, &self->m_StateFile, &self->m_AllBuiltNodes);
    }

    DigestCacheInit(&self->m_DigestCache, MB(128), self->m_DagData->m_DigestCacheFileName);

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

    FileSystemInit(s_DagFileName);

    return true;
}

void DriverDestroy(Driver *self)
{
    FileSystemDestroy();

    DigestCacheDestroy(&self->m_DigestCache);

    StatCacheDestroy(&self->m_StatCache);

    ScanCacheDestroy(&self->m_ScanCache);

    for (auto &node: self->m_RuntimeNodes)
    {
        if (node.m_CurrentLeafInputSignature != nullptr)
            DestroyLeafInputSignatureData(&self->m_Heap, node.m_CurrentLeafInputSignature);
        if (HashSetIsInitialized(&node.m_ImplicitInputs))
            HashSetDestroy(&node.m_ImplicitInputs);
        if (node.m_DynamicallyDiscoveredOutputFiles != nullptr)
        {
            node.m_DynamicallyDiscoveredOutputFiles->Destroy();
            HeapFree(&self->m_Heap, node.m_DynamicallyDiscoveredOutputFiles);
        }
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

BuildResult::Enum DriverBuild(Driver *self, int* out_finished_node_count, char* out_frontend_rerun_reason, const char** argv, int argc)
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
        queue_config.m_FileSigningLog = OpenFile("signing-debug.txt", "w");
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

    if (build_result == BuildResult::kRequireFrontendRerun)
    {
        BuildQueueGetFrontendRerunReason(&build_queue, out_frontend_rerun_reason);
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

    // Ensure that the target directory exists.
    PathBuffer path;
    PathInit(&path, self->m_DagData->m_ScanCacheFileName.Get());
    if (!MakeDirectoriesForFile(&self->m_StatCache, path))
    {
        Log(kWarning, "Failed to create directories for \"%s\"", self->m_DagData->m_ScanCacheFileName.Get());
    }

    if (success)
    {
        success = RenameFile(self->m_DagData->m_ScanCacheFileNameTmp, self->m_DagData->m_ScanCacheFileName);
        if (!success)
        {
            Log(kWarning, "Failed to rename \"%s\" to \"%s\"",
                self->m_DagData->m_ScanCacheFileNameTmp.Get(),
                self->m_DagData->m_ScanCacheFileName.Get());
        }
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
    // Ensure that the directories exist.
    PathBuffer path;
    PathInit(&path, self->m_DagData->m_DigestCacheFileName.Get());
    if (!MakeDirectoriesForFile(&self->m_StatCache, path))
    {
        Log(kWarning, "Failed to create directories for \"%s\"", self->m_DagData->m_DigestCacheFileName.Get());
    }
    PathInit(&path, self->m_DagData->m_DigestCacheFileNameTmp.Get());
    if (!MakeDirectoriesForFile(&self->m_StatCache, path))
    {
        Log(kWarning, "Failed to create directories for \"%s\"", self->m_DagData->m_DigestCacheFileNameTmp.Get());
    }

    // This will be invalidated.
    return DigestCacheSave(&self->m_DigestCache, &self->m_Heap, self->m_DagData->m_DigestCacheFileName, self->m_DagData->m_DigestCacheFileNameTmp);
}




