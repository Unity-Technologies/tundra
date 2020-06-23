#include "DigestCache.hpp"
#include "RuntimeNode.hpp"
#include "Exec.hpp"
#include "BuildQueue.hpp"
#include "Hash.hpp"
#include "Caching.hpp"

HashDigest ComputeCacheKey(RuntimeNode* node)
{
    HashDigest result;
    memset(&result, 0, sizeof(HashDigest));
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
