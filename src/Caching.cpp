#include "DigestCache.hpp"
#include "RuntimeNode.hpp"
#include "Exec.hpp"
#include "BuildQueue.hpp"
#include "Hash.hpp"
#include "Caching.hpp"
#include "Buffer.hpp"
#include "Driver.hpp"
#include "FileSign.hpp"

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

bool InvokeCacheMe(const HashDigest& digest, const FrozenArray<FrozenFileAndHash>& outputFiles, ThreadState* thread_state, CacheMode::CacheMode mode)
{
    char buffer[5000];

    char digestString[kDigestStringSize];
    DigestToString(digestString, digest);

    //todo put files in rsp.

    char* cmd = mode == CacheMode::kLookUp ? "down" : "up";

    snprintf(buffer, sizeof(buffer), "c:\\cacheme\\cacheme.exe %s %s00000000000000000000000000000001 artifacts/myprogram/debug_Win64_VS2019_nonlump/myprogram.exe artifacts/myprogram/debug_Win64_VS2019_nonlump/myprogram.pdb", cmd, digestString);

    printf("debug: %s\n", buffer);

    EnvVariable env_var;
    env_var.m_Name = "CACHE_SERVER_ADDRESS";
    env_var.m_Value = "127.0.0.1:9092";

    EnvVariable* envs = &env_var;

    ExecResult result = ExecuteProcess(buffer, 1, envs, thread_state->m_Queue->m_Config.m_Heap, thread_state->m_ThreadIndex, true, nullptr, nullptr);
    return result.m_ReturnCode == 0;
}
