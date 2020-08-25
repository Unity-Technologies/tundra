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

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <sstream>
#include "stdarg.h"

TundraStats g_Stats;

static const char *s_BuildFile;
static const char *s_DagFileName;

static bool DriverPrepareDag(Driver *self, const char *dag_fn);
static bool DriverCheckDagSignatures(Driver *self, char *out_of_date_reason, int out_of_date_reason_maxlength);

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
    self->m_JustPrintLeafInputSignature = false;
    self->m_ThrottleOnHumanActivity = false;
    self->m_ThrottleInactivityPeriod = 30;
    self->m_ThrottledThreadsAmount = 0;
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

// Helper routine to load frozen data into RAM via memory mapping
template <typename FrozenType>
static bool LoadFrozenData(const char *fn, MemoryMappedFile *result, const FrozenType **ptr)
{
    MemoryMappedFile mapping;

    MmapFileInit(&mapping);

    MmapFileMap(&mapping, fn);

    if (MmapFileValid(&mapping))
    {
        char *mmap_buffer = static_cast<char *>(mapping.m_Address);
        const FrozenType *data = reinterpret_cast<const FrozenType *>(mmap_buffer);

        Log(kDebug, "%s: successfully mapped at %p (%d bytes)", fn, data, (int)mapping.m_Size);

        // Check size
        if (mapping.m_Size < sizeof(FrozenType))
        {
            Log(kWarning, "%s: Bad mmap size %d - need at least %d bytes",
                fn, (int)mapping.m_Size, (int)sizeof(FrozenType));
            goto error;
        }

        // Check magic number
        if (data->m_MagicNumber != FrozenType::MagicNumber)
        {
            Log(kDebug, "%s: Bad magic number %08x - current is %08x",
                fn, data->m_MagicNumber, FrozenType::MagicNumber);
            goto error;
        }

        // Check magic number
        if (data->m_MagicNumberEnd != FrozenType::MagicNumber)
        {
            Log(kError, "Did not find expected magic number marker at the end of %s. This most likely means data writing code for that file is writing too much or too little data", fn);
            goto error;
        }

        // Move ownership of memory mapping to member variable.
        *result = mapping;

        *ptr = data;

        return true;
    }

    Log(kDebug, "%s: mmap failed", fn);

error:
    MmapFileDestroy(&mapping);
    return false;
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
    if (!DriverPrepareDag(self, s_DagFileName))
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


static bool ExitRequestingFrontendRun(const char *reason_fmt, ...)
{
    char buffer[1024];

    va_list args;
    va_start(args, reason_fmt);
    int written =vsnprintf(buffer,1024,reason_fmt, args);
    va_end(args);
    buffer[written] = 0;


    PrintMessage(MessageStatusLevel::Success, "Require frontend run.  %s", buffer);

    exit(BuildResult::kRequireFrontendRerun);
    return false;
}


static bool DriverPrepareDag(Driver *self, const char *dag_fn)
{
    const int out_of_date_reason_length = 500;
    char out_of_date_reason[out_of_date_reason_length + 1];

    snprintf(out_of_date_reason, out_of_date_reason_length, "(unknown reason)");

    char json_filename[kMaxPathLength];
    snprintf(json_filename, sizeof json_filename, "%s.json", dag_fn);
    json_filename[sizeof(json_filename) - 1] = '\0';

    char dagderived_filename[kMaxPathLength];
    snprintf(dagderived_filename, sizeof dagderived_filename, "%s_derived", dag_fn);
    dagderived_filename[sizeof(dagderived_filename) - 1] = '\0';

    FileInfo dag_info = GetFileInfo(dag_fn);
    FileInfo dagderived_info = GetFileInfo(dagderived_filename);
    FileInfo json_info = GetFileInfo(json_filename);

    if (!dag_info.Exists() && !json_info.Exists())
        return ExitRequestingFrontendRun("%s does not exist yet", json_filename);

    bool frozeDag = false;
    if (json_info.Exists())
    {
        bool dagExists = dag_info.Exists();
        if (!dagExists || json_info.m_Timestamp > dag_info.m_Timestamp)
        {
            const char* reason = dagExists ? "Timestamp of .json > .dag" : ".dag file didn't exist";

            uint64_t time_exec_started = TimerGet();
            if (!FreezeDagJson(json_filename, dag_fn))
                return ExitRequestingFrontendRun("%s failed to freeze", json_filename);
            frozeDag = true;
            uint64_t now = TimerGet();
            double duration = TimerDiffSeconds(time_exec_started, now);
            PrintMessage(MessageStatusLevel::Success, duration, "Freezing %s into .dag (%s)", FindFileNameInside(json_filename), reason);
        }
    }

    if (!LoadFrozenData<Frozen::Dag>(dag_fn, &self->m_DagFile, &self->m_DagData))
    {
        remove(dag_fn);
        remove(dagderived_filename);
        return ExitRequestingFrontendRun("%s couldn't be loaded", dag_fn);
    }

    if (!dagderived_info.Exists() || frozeDag)
    {
        if (!CompileDagDerived(self->m_DagData, &self->m_Heap, &self->m_Allocator, &self->m_StatCache, dagderived_filename))
            return ExitRequestingFrontendRun("failed to create derived dag file %s", dagderived_filename);
    }

    if (!LoadFrozenData<Frozen::DagDerived>(dagderived_filename, &self->m_DagDerivedFile, &self->m_DagDerivedData))
    {
        remove(dag_fn);
        remove(dagderived_filename);
        return ExitRequestingFrontendRun("%s couldn't be loaded", dag_fn);
    }

    uint64_t time_exec_started = TimerGet();
    bool dagIsValid;
    {
        ProfilerScope prof_scope("DriverCheckDagSignatures", 0);
        dagIsValid = DriverCheckDagSignatures(self, out_of_date_reason, out_of_date_reason_length);
    }

    uint64_t now = TimerGet();
    double duration = TimerDiffSeconds(time_exec_started, now);
    if (duration > 1)
        PrintMessage(MessageStatusLevel::Warning, (int) duration, "Calculating file and glob signatures. (unusually slow)");

    if (dagIsValid)
        return true;

    if (self->m_Options.m_IncludesOutput != nullptr)
    {
        Log(kDebug, "Only showing includes; using existing DAG without out-of-date checks");
        return true;
    }

    MmapFileUnmap(&self->m_DagFile);
    self->m_DagData = nullptr;
    MmapFileUnmap(&self->m_DagDerivedFile);
    self->m_DagDerivedData = nullptr;

    if (remove(dag_fn))
        Croak("Failed to remove out of date dag at %s", dag_fn);
    if (remove(dagderived_filename))
        Croak("Failed to remove out of date dagderived file at %s", dagderived_filename);

    ExitRequestingFrontendRun("%s no longer valid. %s", FindFileNameInside(dag_fn), out_of_date_reason);

    return false;
}

static bool DriverCheckDagSignatures(Driver *self, char *out_of_date_reason, int out_of_date_reason_maxlength)
{
    const Frozen::Dag *dag_data = self->m_DagData;

#if ENABLED(CHECKED_BUILD)
    // Paranoia - make sure the data is sorted.
    for (int i = 1, count = dag_data->m_NodeCount; i < count; ++i)
    {
        if (dag_data->m_NodeGuids[i] < dag_data->m_NodeGuids[i - 1])
            Croak("DAG data is not sorted by guid");
    }
#endif

    Log(kDebug, "checking file signatures for DAG data");

    // Check timestamps of frontend files used to produce the DAG
    for (const Frozen::DagFileSignature &sig : dag_data->m_FileSignatures)
    {
        const char *path = sig.m_Path;

        uint64_t timestamp = sig.m_Timestamp;
        FileInfo info = GetFileInfo(path);

        if (info.m_Timestamp != timestamp)
        {
            snprintf(out_of_date_reason, out_of_date_reason_maxlength, "FileSignature timestamp changed: %s", sig.m_Path.Get());
            return false;
        }
    }

    // Check directory listing fingerprints
    // Note that the digest computation in here must match the one in LuaListDirectory
    // The digests computed there are stored in the signature block by frontend code.
    for (const Frozen::DagGlobSignature &sig : dag_data->m_GlobSignatures)
    {
        HashDigest digest = CalculateGlobSignatureFor(sig.m_Path, sig.m_Filter, sig.m_Recurse, &self->m_Heap, &self->m_Allocator);

        // Compare digest with the one stored in the signature block
        if (0 != memcmp(&digest, &sig.m_Digest, sizeof digest))
        {
            char stored[kDigestStringSize], actual[kDigestStringSize];
            DigestToString(stored, sig.m_Digest);
            DigestToString(actual, digest);
            snprintf(out_of_date_reason, out_of_date_reason_maxlength, "directory contents changed: %s", sig.m_Path.Get());
            Log(kInfo, "DAG out of date: file glob change for %s (%s => %s)", sig.m_Path.Get(), stored, actual);
            return false;
        }
    }

    return true;
}

static int LevenshteinDistanceNoCase(const char *s, const char *t)
{
    int n = (int)strlen(s);
    int m = (int)strlen(t);

    if (n == 0)
        return m;
    if (m == 0)
        return n;

    int xSize = n + 1;
    int ySize = m + 1;
    int *d = (int *)alloca(xSize * ySize * sizeof(int));

    for (int x = 0; x <= n; x++)
        d[ySize * x] = x;
    for (int y = 0; y <= m; y++)
        d[y] = y;

    for (int y = 1; y <= m; y++)
    {
        for (int x = 1; x <= n; x++)
        {
            if (tolower(s[x - 1]) == tolower(t[y - 1]))        // Case insensitive
                d[ySize * x + y] = d[ySize * (x - 1) + y - 1]; // no operation
            else
                d[ySize * x + y] = std::min(std::min(
                                                d[ySize * (x - 1) + y] + 1, // a deletion
                                                d[ySize * x + y - 1] + 1),  // an insertion
                                            d[ySize * (x - 1) + y - 1] + 1  // a substitution
                );
        }
    }
    return d[ySize * n + m];
}

//searching in inputs prevents useful single object builds, as the requested object gets found as an input of the linker
#define SUPPORT_SEARCHING_IN_INPUTS 0

// Match their source files and output files against the names specified.
static void FindNodesByName(
    const Frozen::Dag *dag,
    Buffer<int32_t> *out_nodes,
    MemAllocHeap *heap,
    const char **names,
    size_t name_count,
    const FrozenArray<Frozen::NamedNodeData> &named_nodes)
{
    size_t node_bits_size = (dag->m_NodeCount + 31) / 32 * sizeof(uint32_t);
    uint32_t *node_bits = (uint32_t *)alloca(node_bits_size);

    memset(node_bits, 0, node_bits_size);

    for (size_t name_i = 0; name_i < name_count; ++name_i)
    {
        const char *name = names[name_i];

        bool found = false;

        // Try all named nodes first
        bool foundMatchingPrefix = false;
        bool prefixIsAmbigious = false;
        const Frozen::NamedNodeData *nodeDataForMatchingPrefix = nullptr;
        struct StringWithScore
        {
            StringWithScore(int _score, const char *_string) : score(_score), string(_string) {}
            int score;
            const char *string;
        };
        std::vector<StringWithScore> fuzzyMatches;
        fuzzyMatches.reserve(named_nodes.GetCount());
        for (const Frozen::NamedNodeData &named_node : named_nodes)
        {
            const int distance = LevenshteinDistanceNoCase(named_node.m_Name, name);
            const int fuzzyMatchLimit = std::max(0, std::min((int)strlen(name) - 2, 4));
            bool isFuzzyMatch = distance <= fuzzyMatchLimit;

            // Exact match?
            if (distance == 0)
            {
                if (strcmp(named_node.m_Name, name) != 0)
                    Log(kInfo, "found case insensitive match for %s, mapping to %s", name, named_node.m_Name.Get());

                BufferAppendOne(out_nodes, heap, named_node.m_NodeIndex);
                Log(kDebug, "mapped %s to node %d", name, named_node.m_NodeIndex);
                found = true;
                break;
            }
            // Fuzzy match?
            else if (isFuzzyMatch)
                fuzzyMatches.emplace_back(distance, named_node.m_Name.Get());
            // Prefix match?
            if (strncasecmp(named_node.m_Name, name, strlen(name)) == 0)
            {
                prefixIsAmbigious = foundMatchingPrefix;
                if (!foundMatchingPrefix)
                {
                    foundMatchingPrefix = true;
                    nodeDataForMatchingPrefix = &named_node;
                }
                if (!isFuzzyMatch)
                    fuzzyMatches.emplace_back(strlen(named_node.m_Name) - strlen(name), named_node.m_Name.Get());
            }
        }

        // If the given name is an unambigious prefix of one of our named nodes, we go with it, but warn the user.
        if (!found && foundMatchingPrefix && !prefixIsAmbigious)
        {
            Log(kWarning, "autocompleting %s to %s", name, nodeDataForMatchingPrefix->m_Name.Get());
            BufferAppendOne(out_nodes, heap, nodeDataForMatchingPrefix->m_NodeIndex);
            found = true;
        }

        if (found)
            continue;

        //since outputs in the dag are "cleaned paths", with forward slashes converted to backward ones,
        //make sure we convert our searchstring in the same way
        PathBuffer pathbuf;
        PathInit(&pathbuf, name);
        char cleaned_path[kMaxPathLength];
        PathFormat(cleaned_path, &pathbuf);

        const uint32_t filename_hash = Djb2HashPath(cleaned_path);
        for (int node_index = 0; node_index != dag->m_NodeCount; node_index++)
        {
            const Frozen::DagNode &node = dag->m_DagNodes[node_index];
            for (const FrozenFileAndHash &output : node.m_OutputFiles)
            {
                if (filename_hash == output.m_FilenameHash && 0 == PathCompare(output.m_Filename, cleaned_path))
                {
                    BufferAppendOne(out_nodes, heap, node_index);
                    Log(kDebug, "mapped %s to node %d (based on output file)", name, node_index);
                    found = true;
                    break;
                }
            }
        }

        if (!found)
        {
            std::stringstream errorOutput;
            errorOutput << "unable to map " << name << " to any named node or input/output file";
            if (!fuzzyMatches.empty())
            {
                std::sort(fuzzyMatches.begin(), fuzzyMatches.end(), [](const StringWithScore &a, const StringWithScore &b) { return a.score < b.score; });
                errorOutput << "\nmaybe you meant:\n";
                for (int i = 0; i < fuzzyMatches.size() - 1; ++i)
                    errorOutput << "- " << fuzzyMatches[i].string << "\n";
                errorOutput << "- " << fuzzyMatches[fuzzyMatches.size() - 1].string;
            }
            Croak(errorOutput.str().c_str());
        }
    }
}

static void DriverSelectNodes(const Frozen::Dag *dag, const char **targets, int target_count, Buffer<int32_t> *out_nodes, MemAllocHeap *heap)
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

bool DriverPrepareNodes(Driver *self, const char **targets, int target_count)
{
    ProfilerScope prof_scope("Tundra PrepareNodes", 0);

    const Frozen::Dag *dag = self->m_DagData;
    const Frozen::DagNode *dag_nodes = dag->m_DagNodes;
    const HashDigest *dag_node_guids = dag->m_NodeGuids;
    MemAllocHeap *heap = &self->m_Heap;

    Buffer<int32_t> node_stack;
    BufferInitWithCapacity(&node_stack, heap, 1024);

    DriverSelectNodes(dag, targets, target_count, &node_stack, heap);
    int explicitelyRequestedCount = node_stack.m_Size;

    for (int i = 0; i < node_stack.m_Size; ++i)
    {
        const Frozen::DagNode *dag_node = dag_nodes + node_stack[i];

        for (auto usageDep : dag_node->m_DependenciesConsumedDuringUsageOnly)
        {
            if (!BufferContains(&node_stack, usageDep))
                BufferAppendOne(&node_stack, heap, usageDep);
        }
    }

    self->m_AmountOfRuntimeNodesSpecificallyRequested = node_stack.m_Size;

    Buffer<int32_t> node_indices;
    BufferInitWithCapacity(&node_indices, heap, 1024);
    FindDependentNodesFromRootIndices(heap, dag, self->m_DagDerivedData, nullptr, &node_stack[0], node_stack.m_Size, node_indices);

    int node_count = node_indices.m_Size;
    // Allocate space for nodes
    RuntimeNode *out_nodes = BufferAllocZero(&self->m_RuntimeNodes, &self->m_Heap, node_count);

    // Initialize node state
    for (int i = 0; i < node_count; ++i)
    {
        const Frozen::DagNode *dag_node = dag_nodes + node_indices[i];
        out_nodes[i].m_DagNode = dag_node;
        out_nodes[i].m_DagNodeIndex = node_indices[i];
#if ENABLED(CHECKED_BUILD)
        out_nodes[i].m_DebugAnnotation = dag_node->m_Annotation.Get();
#endif
        for (int j = 0; j < node_stack.m_Size; ++j)
        {
            if (node_indices[i] == node_stack[j])
            {
                RuntimeNodeSetExplicitlyRequested(&out_nodes[i]);
                if (j>=explicitelyRequestedCount)
                    RuntimeNodeSetExplicitlyRequestedThroughUseDependency(&out_nodes[i]);
            }
        }
    }

    // Find frozen node state from previous build, if present.
    if (const Frozen::AllBuiltNodes *all_built_nodes = self->m_AllBuiltNodes)
    {
        const Frozen::BuiltNode *built_nodes = all_built_nodes->m_BuiltNodes;
        const HashDigest *state_guids = all_built_nodes->m_NodeGuids;
        const int state_guid_count = all_built_nodes->m_NodeCount;

        for (int i = 0; i < node_count; ++i)
        {
            const HashDigest *src_guid = dag_node_guids + node_indices[i];

            if (const HashDigest *old_guid = BinarySearch(state_guids, state_guid_count, *src_guid))
            {
                int state_index = int(old_guid - state_guids);
                out_nodes[i].m_BuiltNode = built_nodes + state_index;
            }
        }
    }

    // initialize a remapping table from global (dag) index to local (state)
    // index. This is so we can map any DAG node reference onto any local state.
    int32_t *node_remap = BufferAllocFill(&self->m_DagNodeIndexToRuntimeNodeIndex_Table, heap, dag->m_NodeCount, -1);

    CHECK(node_remap == self->m_DagNodeIndexToRuntimeNodeIndex_Table.m_Storage);

    for (int local_index = 0; local_index < node_count; ++local_index)
    {
        const Frozen::DagNode *global_node = out_nodes[local_index].m_DagNode;
        const int global_index = int(global_node - dag_nodes);
        CHECK(node_remap[global_index] == -1);
        node_remap[global_index] = local_index;
    }

    Log(kDebug, "Node remap: %d src nodes, %d active nodes, using %d bytes of node state buffer space",
        dag->m_NodeCount, node_count, sizeof(RuntimeNode) * node_count);

    BufferDestroy(&node_stack, &self->m_Heap);
    BufferDestroy(&node_indices, &self->m_Heap);

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

    BufferInit(&self->m_DagNodeIndexToRuntimeNodeIndex_Table);
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
    BufferDestroy(&self->m_DagNodeIndexToRuntimeNodeIndex_Table, &self->m_Heap);

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



BuildResult::Enum DriverBuild(Driver *self, int* out_finished_node_count)
{
    const Frozen::Dag *dag = self->m_DagData;

    // Initialize build queue
    Mutex debug_signing_mutex;

    BuildQueueConfig queue_config;
    queue_config.m_DriverOptions = &self->m_Options;
    queue_config.m_Flags = 0;
    queue_config.m_Heap = &self->m_Heap;
    queue_config.m_Dag = self->m_DagData;
    queue_config.m_DagNodes = self->m_DagData->m_DagNodes;
    queue_config.m_DagDerived = self->m_DagDerivedData;
    queue_config.m_RuntimeNodes = self->m_RuntimeNodes.m_Storage;
    queue_config.m_TotalRuntimeNodeCount = (int)self->m_RuntimeNodes.m_Size;
    queue_config.m_DagNodeIndexToRuntimeNodeIndex_Table = self->m_DagNodeIndexToRuntimeNodeIndex_Table.m_Storage;
    queue_config.m_ScanCache = &self->m_ScanCache;
    queue_config.m_StatCache = &self->m_StatCache;
    queue_config.m_DigestCache = &self->m_DigestCache;
    queue_config.m_ShaDigestExtensionCount = dag->m_ShaExtensionHashes.GetCount();
    queue_config.m_ShaDigestExtensions = dag->m_ShaExtensionHashes.GetArray();
    queue_config.m_SharedResources = dag->m_SharedResources.GetArray();
    queue_config.m_SharedResourcesCount = dag->m_SharedResources.GetCount();
    queue_config.m_AmountOfRuntimeNodesSpecificallyRequested = self->m_AmountOfRuntimeNodesSpecificallyRequested;

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

#if ENABLED(CHECKED_BUILD)
    {
        ProfilerScope prof_scope("Tundra DebugCheckRemap", 0);
        // Paranoia - double check node remapping table
        for (size_t i = 0, count = self->m_RuntimeNodes.m_Size; i < count; ++i)
        {
            const RuntimeNode *state = self->m_RuntimeNodes.m_Storage + i;
            const Frozen::DagNode *src = state->m_DagNode;
            const int dag_node_index = int(src - self->m_DagData->m_DagNodes);
            int remapped_index = self->m_DagNodeIndexToRuntimeNodeIndex_Table[dag_node_index];
            CHECK(size_t(remapped_index) == i);
        }
    }
#endif

    BuildQueue build_queue;
    BuildQueueInit(&build_queue, &queue_config);

    BuildResult::Enum build_result = BuildResult::kOk;

    if (self->m_Options.m_JustPrintLeafInputSignature)
    {
        PrintLeafInputSignature(&build_queue);
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

struct StateSavingSegments
{
    BinarySegment *main;
    BinarySegment *guid;
    BinarySegment *built_nodes;
    BinarySegment *array;
    BinarySegment *string;
};

template <class TNodeType>
static void save_node_sharedcode(bool nodeWasBuiltSuccesfully, const HashDigest *input_signature, const HashDigest* leafinput_signature, const TNodeType *src_node, const HashDigest *guid, const StateSavingSegments &segments, const SinglyLinkedPathList* additionalDiscoveredOutputFiles)
{
    //we're writing to two arrays in one go.  the FrozenArray<HashDigest> m_NodeGuids and the FrozenArray<BuiltNode> m_BuiltNodes
    //the hashdigest is quick
    BinarySegmentWriteHashDigest(segments.guid, *guid);

    //the rest not so much
    BinarySegmentWriteInt32(segments.built_nodes, nodeWasBuiltSuccesfully ? 1 : 0);
    BinarySegmentWriteHashDigest(segments.built_nodes, *input_signature);
    BinarySegmentWriteHashDigest(segments.built_nodes, *leafinput_signature);

    auto WriteFrozenFileAndHashIntoBuiltNodesStream = [segments](const FrozenFileAndHash& f) -> void {
        BinarySegmentWritePointer(segments.array, BinarySegmentPosition(segments.string));
        BinarySegmentWriteStringData(segments.string, f.m_Filename.Get());
        BinarySegmentWriteInt32(segments.array, f.m_FilenameHash);
    };

    int32_t file_count = src_node->m_OutputFiles.GetCount();
    BinarySegmentWriteInt32(segments.built_nodes, file_count + (additionalDiscoveredOutputFiles == nullptr ? 0 : additionalDiscoveredOutputFiles->count));
    BinarySegmentWritePointer(segments.built_nodes, BinarySegmentPosition(segments.array));
    for (int32_t i = 0; i < file_count; ++i)
        WriteFrozenFileAndHashIntoBuiltNodesStream(src_node->m_OutputFiles[i]);

    if (additionalDiscoveredOutputFiles != nullptr)
    {
        auto iterator = additionalDiscoveredOutputFiles->head;
        while(iterator)
        {
            BinarySegmentWritePointer(segments.array, BinarySegmentPosition(segments.string));
            BinarySegmentWriteStringData(segments.string, iterator->path);
            BinarySegmentWriteInt32(segments.array, Djb2Hash(iterator->path));
            iterator = iterator->next;
        }
    }

    file_count = src_node->m_AuxOutputFiles.GetCount();
    BinarySegmentWriteInt32(segments.built_nodes, file_count);
    BinarySegmentWritePointer(segments.built_nodes, BinarySegmentPosition(segments.array));
    for (int32_t i = 0; i < file_count; ++i)
        WriteFrozenFileAndHashIntoBuiltNodesStream(src_node->m_AuxOutputFiles[i]);

    BinarySegmentWritePointer(segments.built_nodes, BinarySegmentPosition(segments.string));
    BinarySegmentWriteStringData(segments.string, src_node->m_Action);
}

static bool node_was_used_by_this_dag_previously(const Frozen::BuiltNode *previously_built_node, uint32_t current_dag_identifier)
{
    auto &previous_dags = previously_built_node->m_DagsWeHaveSeenThisNodeInPreviously;
    return std::find(previous_dags.begin(), previous_dags.end(), current_dag_identifier) != previous_dags.end();
}


bool DriverSaveAllBuiltNodes(Driver *self)
{
    TimingScope timing_scope(nullptr, &g_Stats.m_StateSaveTimeCycles);
    ProfilerScope prof_scope("Tundra Write AllBuiltNodes", 0);

    MemAllocLinearScope alloc_scope(&self->m_Allocator);

    BinaryWriter writer;
    BinaryWriterInit(&writer, &self->m_Heap);

    StateSavingSegments segments;
    BinarySegment *main_seg = BinaryWriterAddSegment(&writer);
    BinarySegment *guid_seg = BinaryWriterAddSegment(&writer);
    BinarySegment *built_nodes_seg = BinaryWriterAddSegment(&writer);
    BinarySegment *array_seg = BinaryWriterAddSegment(&writer);
    BinarySegment *string_seg = BinaryWriterAddSegment(&writer);

    HashTable<CommonStringRecord, kFlagCaseSensitive> shared_strings;
    HashTableInit(&shared_strings, &self->m_Heap);

    segments.main = main_seg;
    segments.guid = guid_seg;
    segments.built_nodes = built_nodes_seg;
    segments.array = array_seg;
    segments.string = string_seg;

    BinaryLocator guid_ptr = BinarySegmentPosition(guid_seg);
    BinaryLocator built_nodes_ptr = BinarySegmentPosition(built_nodes_seg);

    uint32_t dag_node_count = self->m_DagData->m_NodeCount;
    const HashDigest *dag_node_guids = self->m_DagData->m_NodeGuids;
    const Frozen::DagNode *dag_nodes = self->m_DagData->m_DagNodes;
    RuntimeNode *runtime_nodes = self->m_RuntimeNodes.m_Storage;
    const size_t runtime_nodes_count = self->m_RuntimeNodes.m_Size;

    std::sort(runtime_nodes, runtime_nodes + runtime_nodes_count, [=](const RuntimeNode &l, const RuntimeNode &r) {
        // We know guids are sorted, so all we need to do is compare pointers into that table.
        return l.m_DagNode < r.m_DagNode;
    });

    const HashDigest *old_guids = nullptr;
    const Frozen::BuiltNode *old_state = nullptr;
    uint32_t previously_built_nodes_count = 0;

    if (const Frozen::AllBuiltNodes *all_built_nodes = self->m_AllBuiltNodes)
    {
        old_guids = all_built_nodes->m_NodeGuids;
        old_state = all_built_nodes->m_BuiltNodes;
        previously_built_nodes_count = all_built_nodes->m_NodeCount;
    }

    int emitted_built_nodes_count = 0;
    uint32_t this_dag_hashed_identifier = self->m_DagData->m_HashedIdentifier;

    auto EmitBuiltNodeFromRuntimeNode = [=, &emitted_built_nodes_count, &shared_strings](const RuntimeNode* runtime_node, const HashDigest *guid) -> void {
        emitted_built_nodes_count++;
        MemAllocLinear *scratch = &self->m_Allocator;

        const Frozen::DagNode* dag_node = runtime_node->m_DagNode;

        bool nodeWasBuiltSuccessfully = runtime_node->m_BuildResult != NodeBuildResult::kRanFailed;

        HashDigest leafInputSignatureDigest = {};
        if (runtime_node->m_CurrentLeafInputSignature)
            leafInputSignatureDigest = runtime_node->m_CurrentLeafInputSignature->digest;

        save_node_sharedcode(nodeWasBuiltSuccessfully, &runtime_node->m_CurrentInputSignature, &leafInputSignatureDigest, runtime_node->m_DagNode, guid, segments, runtime_node->m_DynamicallyDiscoveredOutputFiles);

        int32_t file_count = dag_node->m_InputFiles.GetCount();
        BinarySegmentWriteInt32(built_nodes_seg, file_count);
        BinarySegmentWritePointer(built_nodes_seg, BinarySegmentPosition(array_seg));
        for (int32_t i = 0; i < file_count; ++i)
        {
            uint64_t timestamp = 0;
            uint32_t filenameHash = dag_node->m_InputFiles[i].m_FilenameHash;
            const FrozenString& filename = dag_node->m_InputFiles[i].m_Filename;
            FileInfo fileInfo = StatCacheStat(&self->m_StatCache, filename, filenameHash);
            if (fileInfo.Exists())
                timestamp = fileInfo.m_Timestamp;

            BinarySegmentWriteUint64(array_seg, timestamp);
            BinarySegmentWriteUint32(array_seg, filenameHash);
            WriteCommonStringPtr(array_seg, string_seg, filename, &shared_strings, scratch);
        }

        if (dag_node->m_ScannerIndex != -1)
        {
            BinarySegmentWriteInt32(built_nodes_seg, runtime_node->m_ImplicitInputs.m_RecordCount);
            BinarySegmentWritePointer(built_nodes_seg, BinarySegmentPosition(array_seg));

            HashSetWalk(&runtime_node->m_ImplicitInputs, [=, &shared_strings](uint32_t index, uint32_t hash, const char *filename) {
                uint64_t timestamp = 0;
                FileInfo fileInfo = StatCacheStat(&self->m_StatCache, filename, hash);
                if (fileInfo.Exists())
                    timestamp = fileInfo.m_Timestamp;

                BinarySegmentWriteUint64(array_seg, timestamp);
                BinarySegmentWriteUint32(array_seg, hash);
                WriteCommonStringPtr(array_seg, string_seg, filename, &shared_strings, scratch);
            });
        }
        else
        {
            BinarySegmentWriteInt32(built_nodes_seg, 0);
            BinarySegmentWriteNullPointer(built_nodes_seg);
        }

        const Frozen::BuiltNode* built_node = runtime_node->m_BuiltNode;
        //we cast the empty_frozen_array below here to a FrozenArray<uint32_t> that is empty, so the code below gets a lot simpler.
        const FrozenArray<uint32_t> &previous_dags = (built_node == nullptr) ? FrozenArray<uint32_t>::empty() : built_node->m_DagsWeHaveSeenThisNodeInPreviously;

        bool haveToAddOurselves = std::find(previous_dags.begin(), previous_dags.end(), this_dag_hashed_identifier) == previous_dags.end();

        BinarySegmentWriteUint32(built_nodes_seg, previous_dags.GetCount() + (haveToAddOurselves ? 1 : 0));
        BinarySegmentWritePointer(built_nodes_seg, BinarySegmentPosition(array_seg));
        for (auto &identifier : previous_dags)
            BinarySegmentWriteUint32(array_seg, identifier);

        if (haveToAddOurselves)
            BinarySegmentWriteUint32(array_seg, this_dag_hashed_identifier);
    };

    auto EmitBuiltNodeFromPreviouslyBuiltNode = [=, &emitted_built_nodes_count, &shared_strings](const Frozen::BuiltNode *built_node, const HashDigest *guid) -> void {

        save_node_sharedcode(built_node->m_WasBuiltSuccessfully, &built_node->m_InputSignature, &built_node->m_LeafInputSignature, built_node, guid, segments, nullptr);
        emitted_built_nodes_count++;

        int32_t file_count = built_node->m_InputFiles.GetCount();
        BinarySegmentWriteInt32(built_nodes_seg, file_count);
        BinarySegmentWritePointer(built_nodes_seg, BinarySegmentPosition(array_seg));
        for (int32_t i = 0; i < file_count; ++i)
        {
            BinarySegmentWriteUint64(array_seg, built_node->m_InputFiles[i].m_Timestamp);
            BinarySegmentWriteUint32(array_seg, built_node->m_InputFiles[i].m_FilenameHash);
            WriteCommonStringPtr(array_seg, string_seg, built_node->m_InputFiles[i].m_Filename, &shared_strings, &self->m_Allocator);
        }

        file_count = built_node->m_ImplicitInputFiles.GetCount();
        BinarySegmentWriteInt32(built_nodes_seg, file_count);
        BinarySegmentWritePointer(built_nodes_seg, BinarySegmentPosition(array_seg));
        for (int32_t i = 0; i < file_count; ++i)
        {
            BinarySegmentWriteUint64(array_seg, built_node->m_ImplicitInputFiles[i].m_Timestamp);
            BinarySegmentWriteUint32(array_seg, built_node->m_ImplicitInputFiles[i].m_FilenameHash);
            WriteCommonStringPtr(array_seg, string_seg, built_node->m_ImplicitInputFiles[i].m_Filename, &shared_strings, &self->m_Allocator);
        }

        int32_t dag_count = built_node->m_DagsWeHaveSeenThisNodeInPreviously.GetCount();
        BinarySegmentWriteInt32(built_nodes_seg, dag_count);
        BinarySegmentWritePointer(built_nodes_seg, BinarySegmentPosition(array_seg));
        BinarySegmentWrite(array_seg, built_node->m_DagsWeHaveSeenThisNodeInPreviously.GetArray(), dag_count * sizeof(uint32_t));
    };

    auto RuntimeNodeGuidForRuntimeNodeIndex = [=](size_t index) -> const HashDigest * {
        int dag_index = int(runtime_nodes[index].m_DagNode - dag_nodes);
        return dag_node_guids + dag_index;
    };

    auto BuiltNodeGuidForBuiltNodeIndex = [=](size_t index) -> const HashDigest * {
        return old_guids + index;
    };

    auto IsRuntimeNodeValidForWritingToBuiltNodes = [=](const RuntimeNode* runtime_node) -> bool {
        switch(runtime_node->m_BuildResult)
        {
            case NodeBuildResult::kDidNotRun:
                return false;
            case NodeBuildResult::kUpToDate:
            case NodeBuildResult::kRanSuccesfully:
            case NodeBuildResult::kRanSuccessButDependeesRequireFrontendRerun:
            case NodeBuildResult::kRanFailed:
                return true;
        }
        Croak("Unexpected NodeBuildResult");
        return true;
    };

    auto IsPreviouslyBuiltNodeValidForWritingToBuiltNodes = [=](const Frozen::BuiltNode* built_node, const HashDigest* guid) -> bool {
        // Make sure this node is still relevant before saving.
        bool node_is_in_dag = BinarySearch(dag_node_guids, dag_node_count, *guid) != nullptr;

        return node_is_in_dag || !node_was_used_by_this_dag_previously(built_node, this_dag_hashed_identifier);
    };

    {
        size_t runtime_nodes_iterator = 0, previously_built_nodes_iterator = 0;

        //we have an array of RuntimeNodes,  and an array of BuiltNodes. We're going to write most of them
        //to a new frozen AllBuiltinNodes file that will be the next builds' BuiltNodes. We cannot just write out both though:
        //
        //most RuntimeNodes will have a matching pair in BuiltNodes, if the node had been built before. In this case, we want to write out the new RuntimeNode.
        //the RuntimeNode might not have actually executed. This case we want to preserve the old version in BuiltNodes if it existed.
        //
        //We need  to maintain the invariant that BuiltNodes is sorted by the built node's guid. In order to do that, we're going to walk both input arrays at the same time.
        //Both input arrays are also guaranteed to be already sorted, so we just need to make sure we interleave both arrays according to guid sort order.
        while (runtime_nodes_iterator < runtime_nodes_count && previously_built_nodes_iterator < previously_built_nodes_count)
        {
            //future optimization:  avoid doing the Is***ValidForWriting() checks this early, and instead do them after the GUID comparison.
            RuntimeNode* first_runtimenode_in_line = &runtime_nodes[runtime_nodes_iterator];
            if (!IsRuntimeNodeValidForWritingToBuiltNodes(first_runtimenode_in_line))
            {
                //this runtime node didn't run, let's skip over it.
                runtime_nodes_iterator++;
                continue;
            }

            const HashDigest * runtime_node_guid = RuntimeNodeGuidForRuntimeNodeIndex(runtime_nodes_iterator);
            const HashDigest * previously_built_guid = BuiltNodeGuidForBuiltNodeIndex(previously_built_nodes_iterator);
            const Frozen::BuiltNode* previously_built_node = &old_state[previously_built_nodes_iterator];

            if (!IsPreviouslyBuiltNodeValidForWritingToBuiltNodes(previously_built_node, previously_built_guid))
            {
                previously_built_nodes_iterator++;
                continue;
            }

            //ok, now we have a runtimenode that did actually run, and a previously built node that is still valid for this dag. let's figure out
            //which one we should write out first.

            int compare = CompareHashDigests(*runtime_node_guid, *previously_built_guid);

            if (compare > 0)
            {
                //the first in line from the builtnodes actually has a lower GUID, we need to write that one out first.
                EmitBuiltNodeFromPreviouslyBuiltNode(previously_built_node, previously_built_guid);

                previously_built_nodes_iterator++;
                continue;
            }

            //ok, so now the RuntimeNode first in line has the lowest guid, let's write it out.
            EmitBuiltNodeFromRuntimeNode(first_runtimenode_in_line, runtime_node_guid);
            runtime_nodes_iterator++;

            if (compare == 0)
            {
                //the BuiltNode array had a matching node for this RuntimeNode. We're going to drop it, since the RuntimeNode has newer information.
                previously_built_nodes_iterator++;
            }
        }

        //ok, one of the arrays have been drained, so we can just blindly process the other one
        for( ; runtime_nodes_iterator < runtime_nodes_count; runtime_nodes_iterator++)
        {
            RuntimeNode* first_runtimenode_in_line = &runtime_nodes[runtime_nodes_iterator];
            if (!IsRuntimeNodeValidForWritingToBuiltNodes(first_runtimenode_in_line))
            {
                //this runtime node didn't run, let's skip over it.
                continue;
            }

            const HashDigest * runtime_node_guid = RuntimeNodeGuidForRuntimeNodeIndex(runtime_nodes_iterator);
            EmitBuiltNodeFromRuntimeNode(first_runtimenode_in_line, runtime_node_guid);
        }

        for ( ; previously_built_nodes_iterator < previously_built_nodes_count; previously_built_nodes_iterator++)
        {
            const Frozen::BuiltNode* previously_built_node = &old_state[previously_built_nodes_iterator];
            const HashDigest * previously_built_guid = BuiltNodeGuidForBuiltNodeIndex(previously_built_nodes_iterator);

            if (IsPreviouslyBuiltNodeValidForWritingToBuiltNodes(previously_built_node, previously_built_guid))
                EmitBuiltNodeFromPreviouslyBuiltNode(previously_built_node,previously_built_guid);
        }
    }

    // Complete main data structure.
    BinarySegmentWriteUint32(main_seg, Frozen::AllBuiltNodes::MagicNumber);
    BinarySegmentWriteInt32(main_seg, emitted_built_nodes_count);
    BinarySegmentWritePointer(main_seg, guid_ptr);
    BinarySegmentWritePointer(main_seg, built_nodes_ptr);
    BinarySegmentWriteUint32(main_seg, Frozen::AllBuiltNodes::MagicNumber);

    // Unmap old state data.
    MmapFileUnmap(&self->m_StateFile);
    self->m_AllBuiltNodes = nullptr;

    bool success = BinaryWriterFlush(&writer, self->m_DagData->m_StateFileNameTmp);

    if (success)
    {
        // Commit atomically with a file rename.
        success = RenameFile(self->m_DagData->m_StateFileNameTmp, self->m_DagData->m_StateFileName);
    }
    else
    {
        remove(self->m_DagData->m_StateFileNameTmp);
    }

    HashTableDestroy(&shared_strings);

    BinaryWriterDestroy(&writer);

    return success;
}

// Returns true if the path was actually cleaned up.
// Does NOT delete symlinks.
static bool CleanupPath(const char *path)
{
    FileInfo info = GetFileInfo(path);
    if (!info.Exists())
        return false;
    if (info.IsSymlink())
        return false;
#if defined(TUNDRA_UNIX)
    return 0 == remove(path);
#else
    else if (info.IsDirectory())
        return TRUE == RemoveDirectoryA(path);
    else
        return TRUE == DeleteFileA(path);
#endif
}

void DriverRemoveStaleOutputs(Driver *self)
{
    TimingScope timing_scope(nullptr, &g_Stats.m_StaleCheckTimeCycles);
    ProfilerScope prof_scope("Tundra RemoveStaleOutputs", 0);

    const Frozen::Dag *dag = self->m_DagData;
    const Frozen::AllBuiltNodes *all_built_nodes = self->m_AllBuiltNodes;
    MemAllocLinear *scratch = &self->m_Allocator;

    MemAllocLinearScope scratch_scope(scratch);

    if (!all_built_nodes)
    {
        Log(kDebug, "unable to clean up stale output files - no previous build state");
        return;
    }

    HashSet<kFlagPathStrings> file_table;
    HashSetInit(&file_table, &self->m_Heap);
    HashSet<kFlagPathStrings> directory_table;
    HashSetInit(&directory_table, &self->m_Heap);

    // Insert all current regular and aux output files into the hash table.
    auto add_file = [&file_table](const FrozenFileAndHash &p) -> void {
        const uint32_t hash = p.m_FilenameHash;

        if (!HashSetLookup(&file_table, hash, p.m_Filename))
        {
            HashSetInsert(&file_table, hash, p.m_Filename);
        }
    };

    auto add_directory = [&directory_table](const FrozenFileAndHash &p) -> void {
        const uint32_t hash = p.m_FilenameHash;

        if (!HashSetLookup(&directory_table, hash, p.m_Filename))
        {
            HashSetInsert(&directory_table, hash, p.m_Filename);
        }
    };

    for (int i = 0, node_count = dag->m_NodeCount; i < node_count; ++i)
    {
        const Frozen::DagNode *node = dag->m_DagNodes + i;

        for (const FrozenFileAndHash &p : node->m_OutputFiles)
            add_file(p);

        for (const FrozenFileAndHash &p : node->m_AuxOutputFiles)
            add_file(p);

        for (const FrozenFileAndHash &p : node->m_OutputDirectories)
            add_directory(p);
    }

    HashSet<kFlagPathStrings> nuke_table;
    HashSetInit(&nuke_table, &self->m_Heap);

    auto add_parent_directories_to_nuke_table = [&nuke_table, &scratch](const char* path)
    {
        PathBuffer buffer;
        PathInit(&buffer, path);

        while (PathStripLast(&buffer))
        {
            if (buffer.m_SegCount == 0)
                break;

            char dir[kMaxPathLength];
            PathFormat(dir, &buffer);
            uint32_t dir_hash = Djb2HashPath(dir);

            if (!HashSetLookup(&nuke_table, dir_hash, dir))
            {
                HashSetInsert(&nuke_table, dir_hash, StrDup(scratch, dir));
            }
        }
    };

    auto startsWith = [](const char *pre, const char *str) -> bool
    {
        return strncmp(pre, str, strlen(pre)) == 0;
    };

    // Check all output files in the state if they're still around.
    // Otherwise schedule them (and all their parent dirs) for nuking.
    // We will rely on the fact that we can't rmdir() non-empty directories.
    auto check_file = [&file_table, &nuke_table, add_parent_directories_to_nuke_table, &directory_table, startsWith](const FrozenFileAndHash& fileAndHash) {
        uint32_t path_hash = fileAndHash.m_FilenameHash;
        const char* path = fileAndHash.m_Filename.Get();

        if (HashSetLookup(&file_table, path_hash, path))
            return;

        bool wasChild = false;
        HashSetWalk(&directory_table, [&](uint32_t index, uint32_t hash, const char* dir) {
            if (startsWith(dir,path))
                wasChild = true;
            });
        if (wasChild)
            return;

        if (!HashSetLookup(&nuke_table, path_hash, path))
        {
            HashSetInsert(&nuke_table, path_hash, path);
        }

        add_parent_directories_to_nuke_table(path);
    };


    HashSet<kFlagPathStrings> outputdir_nuke_table;
    HashSetInit(&outputdir_nuke_table, &self->m_Heap);

    for (int i = 0, state_count = all_built_nodes->m_NodeCount; i < state_count; ++i)
    {
        const Frozen::BuiltNode *built_node = all_built_nodes->m_BuiltNodes + i;

        if (!node_was_used_by_this_dag_previously(built_node, dag->m_HashedIdentifier))
            continue;

        for (const FrozenFileAndHash& fileAndHash : built_node->m_OutputFiles)
        {
            check_file(fileAndHash);
        }

        for (const FrozenFileAndHash& fileAndHash : built_node->m_AuxOutputFiles)
        {
            check_file(fileAndHash);
        }
    }

    //actually do the directory deletion
    const char* any_nuked_dir = nullptr;
    HashSetWalk(&outputdir_nuke_table, [&](uint32_t index, uint32_t hash, const char* path) {
        DeleteDirectory(path);
        any_nuked_dir = path;
    });

    // Create list of files and dirs, sort descending by path length. This sorts
    // files and subdirectories before their parent directories.
    const char **paths = LinearAllocateArray<const char *>(scratch, nuke_table.m_RecordCount);
    HashSetWalk(&nuke_table, [paths](uint32_t index, uint32_t hash, const char *str) {
        paths[index] = str;
    });

    std::sort(paths, paths + nuke_table.m_RecordCount, [](const char *l, const char *r) {
        return strlen(r) < strlen(l);
    });

    uint32_t file_nuke_count = nuke_table.m_RecordCount;
    uint64_t time_exec_started = TimerGet();
    for (uint32_t i = 0; i < file_nuke_count; ++i)
    {
        if (CleanupPath(paths[i]))
        {
            Log(kDebug, "cleaned up %s", paths[i]);
        }
    }

    uint32_t nuke_count = file_nuke_count + outputdir_nuke_table.m_RecordCount;

    if (nuke_count > 0)
    {
        char buffer[2000];
        snprintf(buffer, sizeof(buffer), "Delete %d artifact files that are no longer in use. (like %s)", nuke_count, any_nuked_dir == nullptr ? paths[0] : any_nuked_dir);
        PrintMessage(MessageStatusLevel::Success, (int)TimerDiffSeconds(time_exec_started, TimerGet()),buffer);
    }

    HashSetDestroy(&nuke_table);
    HashSetDestroy(&directory_table);
    HashSetDestroy(&outputdir_nuke_table);
    HashSetDestroy(&file_table);
}



