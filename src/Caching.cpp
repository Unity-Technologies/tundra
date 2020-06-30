#include "DigestCache.hpp"
#include "RuntimeNode.hpp"
#include "Exec.hpp"
#include "BuildQueue.hpp"
#include "Hash.hpp"
#include "Caching.hpp"
#include "Driver.hpp"
#include "FileSign.hpp"
#include "MakeDirectories.hpp"
#include "Scanner.hpp"
#include "HashTable.hpp"
#include "Profiler.hpp"
#include "DagData.hpp"
#include "RunAction.hpp"
#include "NodeResultPrinting.hpp"

static void HashEntry(FILE* debug_hash_fd, HashState* state, const char* label, const char* str)
{
    fprintf(debug_hash_fd, "%s: %s\n", label, str);
    HashAddString(state, str);
}

static void HashEntry(FILE* debug_hash_fd, HashState* state, const char* label, const char* file, HashDigest& digest)
{
    char temp[kDigestStringSize];
    DigestToString(temp, digest);

    char buffer[1000];
    strncpy(buffer, file, sizeof(buffer));
    char*p = buffer;
    for ( ; *p; ++p) *p = tolower(*p);

    fprintf(debug_hash_fd, "%s: %s %s\n", label, buffer, temp);
    HashUpdate(state, &digest, sizeof(digest));
}

static void HashEntry(FILE* debug_hash_fd, HashState* state, const char* label, int payload)
{
    fprintf(debug_hash_fd, "%s: %d\n", label, payload);
    HashAddInteger(state, payload);
}

bool IsFileGenerated(const Frozen::Dag* dag, const char* filename)
{
     for(auto& implicitdir: dag->m_DirectoriesCausingImplicitDependencies)
    {
        bool match = strncmp(implicitdir.Get(), filename, strlen(implicitdir.Get()) ) == 0;
        if (match)
        {
            return true;
        }
    }
    return false;
}

static bool IgnoreGeneratedIncludes(void* _userData, const char* includingFile, const char* includedFile)
{
    return IsFileGenerated((const Frozen::Dag*)_userData, includedFile);
}

void AddIncludesForDagNode(StatCache* stat_cache, const Frozen::DagNode* dagNode, const FrozenString &filename, ScanInput *scan_input, IncludeCallback *callback, HashSet<kFlagPathStrings> *implicitDeps, MemAllocLinear *includeLinearAlloc)
{
    if (dagNode->m_Scanner != nullptr)
    {
        MemAllocLinearScope scratch_scope(scan_input->m_ScratchAlloc);

        scan_input->m_ScannerConfig = dagNode->m_Scanner;
        scan_input->m_FileName = filename;

        ScanOutput scan_output;

        if (ScanImplicitDeps(stat_cache, scan_input, &scan_output, callback))
        {
            for (int i = 0, count = scan_output.m_IncludedFileCount; i < count; ++i)
            {
                const FileAndHash &path = scan_output.m_IncludedFiles[i];
                if (!HashSetLookup(implicitDeps, path.m_FilenameHash, path.m_Filename))
                    HashSetInsert(implicitDeps, path.m_FilenameHash, StrDup(includeLinearAlloc, path.m_Filename));
            }
        }
    }
}

HashDigest ComputeLeafInputSignature(BuildQueueConfig* config, ThreadState* thread_state, const Frozen::DagNode* dagNode)
{
    ProfilerScope profiler_scope("ComputeLeafInputSignature", thread_state->m_ProfilerThreadId, dagNode->m_Annotation);

    char filename[1000];

    snprintf(filename, sizeof(filename), "artifacts/cachesignatures/%s.txt", dagNode->m_Annotation.Get());
    PathBuffer output;
    PathInit(&output, filename);
    MakeDirectoriesForFile(config->m_StatCache, output);
    FILE* debug_hash_fd = fopen(filename, "w");

    if (debug_hash_fd == 0)
    {
        CroakAbort("Failed to create %s!!", filename);
    }

    MemAllocHeap* heap = config->m_Heap;
    Buffer<int32_t> allDependencies;
    BufferInitWithCapacity(&allDependencies, heap, 1024);

    Buffer<int32_t> rootNode;
    BufferInitWithCapacity(&rootNode, heap, 1024);

    int dagNodeIndex = dagNode - config->m_DagNodes;
    BufferAppendOne(&rootNode, heap, dagNodeIndex);

    FindAllDependentNodes(config, allDependencies, rootNode);

    HashState hashState;
    HashInit(&hashState);

    auto stat_cache = config->m_StatCache;
    auto digest_cache = config->m_DigestCache;

    HashSet<kFlagPathStrings> implicitDeps;
    HashSetInit(&implicitDeps, heap);

    MemAllocLinear includeLinearAlloc;
    LinearAllocInit(&includeLinearAlloc, &thread_state->m_LocalHeap, 1024*1024*16, "includes");

    ScanInput scan_input;
    scan_input.m_ScratchAlloc = &thread_state->m_ScratchAlloc;
    scan_input.m_ScratchHeap = &thread_state->m_LocalHeap;
    scan_input.m_ScanCache = config->m_ScanCache;

    IncludeCallback mycallback;
    mycallback.userData = (void*)&config->m_Dag;
    mycallback.callback = &IgnoreGeneratedIncludes;

    for (int dependencyIndex: allDependencies)
    {
        auto& dependencyDagNode = config->m_DagNodes[dependencyIndex];

        for (auto& f: dependencyDagNode.m_OutputFiles)
        {
            HashEntry(debug_hash_fd, &hashState, "outputFile", f.m_Filename.Get());
        }

        HashEntry(debug_hash_fd, &hashState, "action", dependencyDagNode.m_Action.Get());
        for (auto& e: dependencyDagNode.m_EnvVars)
        {
            HashEntry(debug_hash_fd, &hashState, "env-name", e.m_Name);
            if (strcmp("_MSPDBSRV_ENDPOINT_",e.m_Name.Get()) != 0)
                HashEntry(debug_hash_fd, &hashState, "env-value", e.m_Value);
        }

        for (auto& s: dependencyDagNode.m_AllowedOutputSubstrings)
            HashEntry(debug_hash_fd, &hashState, "allowedOutputStrings", s.Get());

        int relevantFlags = dependencyDagNode.m_Flags & ~Frozen::DagNode::kFlagCacheable;
        if (relevantFlags != (Frozen::DagNode::kFlagOverwriteOutputs | Frozen::DagNode::kFlagAllowUnexpectedOutput))
            HashEntry(debug_hash_fd, &hashState, "flags", relevantFlags);

        // Todo: can roll this into deriveddag m_LeafInputFiles
        for (auto& possibleInclude: dependencyDagNode.m_FilesThatMightBeIncluded)
        {
            if (IsFileGenerated(config->m_Dag, possibleInclude.m_Filename.Get()))
                continue;

            HashDigest digest = ComputeFileSignatureSha1(stat_cache, digest_cache, possibleInclude.m_Filename, possibleInclude.m_FilenameHash);
            HashEntry(debug_hash_fd, &hashState, "possibleInclude: ", possibleInclude.m_Filename, digest);
            
            AddIncludesForDagNode(stat_cache, &dependencyDagNode, possibleInclude.m_Filename, &scan_input, &mycallback, &implicitDeps, &includeLinearAlloc);
        }

        for (int leafInputIndex: dependencyDagNode.m_LeafInputFiles)
        {
            auto& inputFile = dependencyDagNode.m_InputFiles[leafInputIndex];

            HashDigest digest = ComputeFileSignatureSha1(stat_cache, digest_cache, inputFile.m_Filename, inputFile.m_FilenameHash);
            HashEntry(debug_hash_fd, &hashState, "leafInput: ", inputFile.m_Filename, digest);

            AddIncludesForDagNode(stat_cache, &dependencyDagNode, inputFile.m_Filename, &scan_input, &mycallback, &implicitDeps, &includeLinearAlloc);
        }

        fprintf(debug_hash_fd,"\n\n");
    }

    BufferDestroy(&allDependencies, heap);
    BufferDestroy(&rootNode, heap);

    HashSetWalk(&implicitDeps, [&](uint32_t index, uint32_t hash, const char *filename) {
        HashDigest digest = ComputeFileSignatureSha1(stat_cache, digest_cache, filename, Djb2HashPath(filename));


        HashEntry(debug_hash_fd, &hashState, "implicitDeps: ", filename, digest);
    });

    fclose(debug_hash_fd);
    LinearAllocDestroy(&includeLinearAlloc);

    HashDigest result;
    HashFinalize(&hashState, &result);
    return result;
}


static int SlowCallback(void *user_data, const char* label)
{
    SlowCallbackData *data = (SlowCallbackData *)user_data;
    MutexLock(data->queue_lock);
    char buffer[1000];
    snprintf(buffer,sizeof(buffer),"%s %s", label, data->node_data->m_Annotation.Get());
    int sendNextCallbackIn = PrintNodeInProgress(data->node_data, data->time_of_start, data->build_queue, buffer);
    MutexUnlock(data->queue_lock);
    return sendNextCallbackIn;
}

static int SlowCallback_CacheGet(void *user_data)
{
    return SlowCallback(user_data, "[CacheGet]");
}

static int SlowCallback_CachePost(void *user_data)
{
    return SlowCallback(user_data, "[CachePush]");
}

bool InvokeCacheMe(const HashDigest& digest, StatCache *stat_cache, const FrozenArray<FrozenFileAndHash>& outputFiles, ThreadState* thread_state, CacheMode::CacheMode mode, const Frozen::DagNode* dagNode, Mutex* queue_lock)
{
    ProfilerScope profiler_scope(mode == CacheMode::kLookUp ? "InvokeCacheMe-down" : "InvokeCacheMe-up", thread_state->m_ProfilerThreadId, outputFiles[0].m_Filename);

    char buffer[5000];

    char digestString[kDigestStringSize];
    DigestToString(digestString, digest);

    //todo put files in rsp.

    const char* cmd = mode == CacheMode::kLookUp ? "down" : "up";

    char *bufferPos = buffer;

#if defined(TUNDRA_WIN32)
    bufferPos += snprintf(bufferPos, sizeof(buffer), "cacheme.exe");
#else
    bufferPos += snprintf(bufferPos, sizeof(buffer), "cacheme");
#endif
    bufferPos += snprintf(bufferPos, sizeof(buffer), " %s %s00000000000000000000000000000001", cmd, digestString);

    for (auto &it : outputFiles)
    {
        PathBuffer output;
        PathInit(&output, it.m_Filename);
        MakeDirectoriesForFile(stat_cache, output);
        bufferPos += snprintf(bufferPos, sizeof(buffer), " \"%s\" ", it.m_Filename.Get());
    }

    EnvVariable env_var;
    env_var.m_Name = "CACHE_SERVER_ADDRESS";
    env_var.m_Value = "127.0.0.1:9092";

    EnvVariable* envs = &env_var;

    SlowCallbackData slowCallbackData;
    slowCallbackData.node_data = dagNode;
    slowCallbackData.time_of_start = TimerGet();
    slowCallbackData.queue_lock = queue_lock;
    slowCallbackData.build_queue = thread_state->m_Queue;

    ExecResult result = ExecuteProcess(buffer, 1, envs, nullptr, thread_state->m_ThreadIndex, true, mode == CacheMode::kLookUp ? SlowCallback_CacheGet : SlowCallback_CachePost , &slowCallbackData);
    return result.m_ReturnCode == 0;
}
