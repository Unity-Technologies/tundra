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

HashDigest ComputeLeafInputSignature(BuildQueueConfig* config, const Frozen::DagNode* dagNode)
{
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

    for (int dependencyIndex: allDependencies)
    {
        auto& dependencyDagNode = config->m_DagNodes[dependencyIndex];
        for (int leafInputIndex: dependencyDagNode.m_LeafInputFiles)
        {
            auto& inputFile = dependencyDagNode.m_InputFiles[leafInputIndex];

            {
                HashState tempState = hashState;
                HashDigest tempDigest;
                HashFinalize(&tempState, &tempDigest);
                char digestString[kDigestStringSize];
                DigestToString(digestString, tempDigest);

                printf("Before to hashState for: %s: %s\n", digestString, inputFile.m_Filename.Get());
            }

            ComputeFileSignatureSha1(&hashState, stat_cache, digest_cache, inputFile.m_Filename, inputFile.m_FilenameHash);

            {
                HashState tempState = hashState;
                HashDigest tempDigest;
                HashFinalize(&tempState, &tempDigest);
                char digestString[kDigestStringSize];
                DigestToString(digestString, tempDigest);

                printf("Added to hashState for: %s: %s\n", digestString, inputFile.m_Filename.Get());
            }
        }
    }

    BufferDestroy(&allDependencies, heap);
    BufferDestroy(&rootNode, heap);

    HashDigest result;
    HashFinalize(&hashState, &result);
    return result;
}

bool InvokeCacheMe(const HashDigest& digest, StatCache *stat_cache, const FrozenArray<FrozenFileAndHash>& outputFiles, ThreadState* thread_state, CacheMode::CacheMode mode)
{
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
