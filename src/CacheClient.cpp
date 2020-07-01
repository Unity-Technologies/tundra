#include "CacheClient.hpp"
#include "Hash.hpp"
#include "DagData.hpp"
#include "RunAction.hpp"
#include "NodeResultPrinting.hpp"
#include "StatCache.hpp"
#include "Profiler.hpp"
#include "Exec.hpp"
#include "BuildQueue.hpp"
#include "MakeDirectories.hpp"

const char* kENV_REAPI_CACHE_CLIENT = "REAPI_CACHE_CLIENT";
const char* kENV_CACHE_SERVER_ADDRESS = "CACHE_SERVER_ADDRESS";
const char* kENV_BEE_CACHE_BEHAVIOUR = "BEE_CACHE_BEHAVIOUR";

static int SlowCallback(void *user_data, const char* label)
{
    SlowCallbackData *data = (SlowCallbackData *)user_data;
    MutexLock(data->queue_lock);
    char buffer[1000];
    snprintf(buffer,sizeof(buffer),"%s %s", data->node_data->m_Annotation.Get(), label);
    int sendNextCallbackIn = PrintNodeInProgress(data->node_data, data->time_of_start, data->build_queue, buffer);
    MutexUnlock(data->queue_lock);
    return sendNextCallbackIn;
}

static int SlowCallback_CacheRead(void *user_data)
{
    return SlowCallback(user_data, "[CacheRead]");
}

static int SlowCallback_CacheWrite(void *user_data)
{
    return SlowCallback(user_data, "[CacheWrite]");
}

enum Operation
{
    kOperationRead,
    kOperationWrite
};

static bool Invoke_REAPI_Cache_Client(const HashDigest& digest, StatCache *stat_cache, const FrozenArray<FrozenFileAndHash>& outputFiles, ThreadState* thread_state, Operation operation, const Frozen::DagNode* dagNode, Mutex* queue_lock)
{
    ProfilerScope profiler_scope("InvokeCacheMe", thread_state->m_ProfilerThreadId, outputFiles[0].m_Filename);

    const char* reapi_raw = getenv(kENV_REAPI_CACHE_CLIENT);
    if (reapi_raw == nullptr)
        return false;

    PathBuffer pathbuf;
    PathInit(&pathbuf, reapi_raw);
    char reapi[kMaxPathLength];
    PathFormat(reapi, &pathbuf);

    char buffer[5000];
    int totalWritten = 0;
    char digestString[kDigestStringSize];
    DigestToString(digestString, digest);

    const char* cmd = operation == kOperationRead ? "down" : "up";
    totalWritten += snprintf(buffer, sizeof(buffer), "%s %s %s00000000000000000000000000000002", reapi, cmd, digestString);

    for (auto &it : outputFiles)
    {
        PathBuffer output;
        PathInit(&output, it.m_Filename);
        MakeDirectoriesForFile(stat_cache, output);
        totalWritten += snprintf(buffer+totalWritten, sizeof(buffer)-totalWritten, " \"%s\" ", it.m_Filename.Get());
    }

    SlowCallbackData slowCallbackData;
    slowCallbackData.node_data = dagNode;
    slowCallbackData.time_of_start = TimerGet();
    slowCallbackData.queue_lock = queue_lock;
    slowCallbackData.build_queue = thread_state->m_Queue;

    Log(kDebug,"%s\n",buffer);
    ExecResult result = ExecuteProcess(buffer, 0, nullptr, nullptr, thread_state->m_ThreadIndex, false, operation == kOperationRead ? SlowCallback_CacheRead : SlowCallback_CacheWrite , &slowCallbackData);

    if (operation == Operation::kOperationRead)
        for (auto &it : outputFiles)
            StatCacheMarkDirty(stat_cache, it.m_Filename, it.m_FilenameHash);

    return result.m_ReturnCode == 0;
}

bool CacheClient::AttemptRead(const Frozen::DagNode* dagNode, HashDigest signature, StatCache* stat_cache, Mutex* queue_lock, ThreadState* thread_state)
{
    return Invoke_REAPI_Cache_Client(signature, stat_cache, dagNode->m_OutputFiles, thread_state, Operation::kOperationRead, dagNode, queue_lock );
}
bool CacheClient::AttemptWrite(const Frozen::DagNode* dagNode, HashDigest signature, StatCache* stat_cache, Mutex* queue_lock, ThreadState* thread_state)
{
    return Invoke_REAPI_Cache_Client(signature, stat_cache, dagNode->m_OutputFiles, thread_state, Operation::kOperationWrite, dagNode, queue_lock );
}

void GetCachingBehaviourSettingsFromEnvironment(bool* attemptReads, bool* attemptWrites)
{
    *attemptReads = false;
    *attemptWrites = false;

    const char* server = getenv(kENV_CACHE_SERVER_ADDRESS);
    if (server == nullptr)
        return;

    const char* reapi_cache_client = getenv(kENV_REAPI_CACHE_CLIENT);
    if (reapi_cache_client == nullptr)
        Croak("%s is set, but %s is not.",kENV_CACHE_SERVER_ADDRESS, kENV_REAPI_CACHE_CLIENT);

    const char* behaviour = getenv(kENV_BEE_CACHE_BEHAVIOUR);
    if (behaviour == nullptr)
        Croak("%s is set, but %s is not.", kENV_CACHE_SERVER_ADDRESS,kENV_BEE_CACHE_BEHAVIOUR);

    if (0 == strcmp("RW", behaviour))
    {
        *attemptReads = true;
        *attemptWrites = true;
    }
    if (0 == strcmp("R", behaviour))
        *attemptReads = true;
    if (0 == strcmp("W", behaviour))
        *attemptWrites = true;

    Log(kDebug, "Caching enabled with %s=%s %s=%s and mode: %s%s\n", kENV_CACHE_SERVER_ADDRESS, server, kENV_REAPI_CACHE_CLIENT, reapi_cache_client, *attemptReads ? "R":"_", *attemptWrites ? "W":"_");
}
