#include "DigestCache.hpp"
#include "RuntimeNode.hpp"
#include "Exec.hpp"
#include "BuildQueue.hpp"
#include "Hash.hpp"
#include "Caching.hpp"
#include "Buffer.hpp"
#include "Driver.hpp"
#include "FileSign.hpp"
#include "MakeDirectories.hpp"
#include "Scanner.hpp"
#include "HashTable.hpp"
#include "Profiler.hpp"

HashDigest ComputeLeafInputSignature(BuildQueueConfig* config, ThreadState* thread_state, const Frozen::DagNode* dagNode)
{
    ProfilerScope profiler_scope("ComputeLeafInputSignature", thread_state->m_ProfilerThreadId, dagNode->m_Annotation);

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

    for (int dependencyIndex: allDependencies)
    {
        auto& dependencyDagNode = config->m_DagNodes[dependencyIndex];
        for (int leafInputIndex: dependencyDagNode.m_LeafInputFiles)
        {
            auto& inputFile = dependencyDagNode.m_InputFiles[leafInputIndex];

            ComputeFileSignatureSha1(&hashState, stat_cache, digest_cache, inputFile.m_Filename, inputFile.m_FilenameHash);

            if (dependencyDagNode.m_Scanner != nullptr)
            {
                ScanInput scan_input;
                scan_input.m_ScannerConfig = dependencyDagNode.m_Scanner;
                scan_input.m_ScratchAlloc = &thread_state->m_ScratchAlloc;
                scan_input.m_ScratchHeap = &thread_state->m_LocalHeap;
                scan_input.m_FileName = inputFile.m_Filename;
                scan_input.m_ScanCache = config->m_ScanCache;

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

        HashAddString(&hashState, dependencyDagNode.m_Action.Get());
        for (auto& e: dependencyDagNode.m_EnvVars)
        {
            HashAddString(&hashState, e.m_Name);
            HashAddString(&hashState, e.m_Value);
        }

        for (auto& s: dependencyDagNode.m_AllowedOutputSubstrings)
            HashAddString(&hashState, s.Get());

        for (auto& f: dependencyDagNode.m_OutputFiles)
            HashAddString(&hashState, f.m_Filename.Get());

        HashAddInteger(&hashState, dependencyDagNode.m_Flags);
    }

    BufferDestroy(&allDependencies, heap);
    BufferDestroy(&rootNode, heap);

    HashSetWalk(&implicitDeps, [&](uint32_t index, uint32_t hash, const char *filename) {
        ComputeFileSignatureSha1(&hashState, stat_cache, digest_cache, filename, Djb2HashPath(filename));
    });


    HashDigest result;
    HashFinalize(&hashState, &result);
    return result;
}

bool InvokeCacheMe(const HashDigest& digest, StatCache *stat_cache, const FrozenArray<FrozenFileAndHash>& outputFiles, ThreadState* thread_state, CacheMode::CacheMode mode)
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
        bufferPos += snprintf(bufferPos, sizeof(buffer), " %s", it.m_Filename.Get());
    }

    printf("debug: %s\n", buffer);

    EnvVariable env_var;
    env_var.m_Name = "CACHE_SERVER_ADDRESS";
    env_var.m_Value = "127.0.0.1:9092";

    EnvVariable* envs = &env_var;

    ExecResult result = ExecuteProcess(buffer, 1, envs, nullptr, thread_state->m_ThreadIndex, true, nullptr, nullptr);
    return result.m_ReturnCode == 0;
}
