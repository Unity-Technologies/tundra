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

static int AppendFileToCommandLine(int totalWritten, char* buffer, int bufferSize, const char* fileName)
{
    int remainingBudget = bufferSize - totalWritten;

    //headsup: requiredSpace return value does _not_ include length of the 0 terminator.
    int requiredSpace = snprintf(buffer+totalWritten, remainingBudget, " \"%s\" ", fileName);

    if (requiredSpace >= remainingBudget)
    {
        Log(kError, "Building CacheClient string exceeded buffer length");
        return 0;
    }

    return totalWritten + requiredSpace;
}


static uint32_t s_CacheClientFailureCount = 0;
const uint32_t kMaxClientFailureCount = 5;

static CacheResult::Enum Invoke_REAPI_Cache_Client(const HashDigest& digest, StatCache *stat_cache, const FrozenArray<FrozenFileAndHash>& outputFiles, ThreadState* thread_state, Operation operation, const Frozen::Dag* dag, const Frozen::DagNode* dagNode, Mutex* queue_lock, const char* ingredients_file)
{
    if (s_CacheClientFailureCount > kMaxClientFailureCount)
        return CacheResult::DidNotTry;

    ProfilerScope profiler_scope("InvokeCacheMe", thread_state->m_ProfilerThreadId, outputFiles[0].m_Filename);

    const char* reapi_raw = getenv(kENV_REAPI_CACHE_CLIENT);
    if (reapi_raw == nullptr)
        Croak("%s not setup", kENV_REAPI_CACHE_CLIENT);

    PathBuffer pathbuf;
    PathInit(&pathbuf, reapi_raw);
    char reapi[kMaxPathLength];
    PathFormat(reapi, &pathbuf);

    char buffer[5000];
    int totalWritten = 0;
    char digestString[kDigestStringSize];
    DigestToString(digestString, digest);

    const char* cmd = operation == kOperationRead ? "down" : "up";
    totalWritten += snprintf(buffer, sizeof(buffer), "%s -v %s %s00000000000000000000000000000002", reapi, cmd, digestString);

    auto processFailure = [queue_lock, dagNode](const char* msg)
    {
        MutexLock(queue_lock);
        printf("Failure while invoking caching client: %s\n%s\n", dagNode->m_Annotation.Get(), msg);
        s_CacheClientFailureCount++;
        if (s_CacheClientFailureCount > kMaxClientFailureCount)
        {
            printf("We encountered %d cache client failures. The rest of the build will not attempt any more cache client operations\n", s_CacheClientFailureCount);
        }

        MutexUnlock(queue_lock);
    };

    //when we start caching nodes with tons of outputs, we should move the filelist to a separate file. for now this will do,
    for (auto &it : outputFiles)
    {
        PathBuffer output;
        PathInit(&output, it.m_Filename);
        MakeDirectoriesForFile(stat_cache, output);

        if ((totalWritten = AppendFileToCommandLine(totalWritten, buffer, sizeof(buffer), it.m_Filename.Get())) == 0)
        {
            processFailure("Not enough space in commandline buffer for all output files");
            return CacheResult::Failure;;
        }
    }

    if (operation == kOperationWrite)
    {
        if ((totalWritten = AppendFileToCommandLine(totalWritten, buffer, sizeof(buffer), ingredients_file)) == 0)
        {
            processFailure("Not enough space in commandline buffer for ingredients_file");
            return CacheResult::Failure;
        }
    }

    SlowCallbackData slowCallbackData;
    slowCallbackData.node_data = dagNode;
    slowCallbackData.time_of_start = TimerGet();
    slowCallbackData.queue_lock = queue_lock;
    slowCallbackData.build_queue = thread_state->m_Queue;

    Log(kDebug,"%s\n",buffer);
    ExecResult result = ExecuteProcess(buffer, 0, nullptr, &thread_state->m_LocalHeap, thread_state->m_ThreadIndex, false, operation == kOperationRead ? SlowCallback_CacheRead : SlowCallback_CacheWrite , &slowCallbackData);

    if (operation == Operation::kOperationRead)
        for (auto &it : outputFiles)
            StatCacheMarkDirty(stat_cache, it.m_Filename, it.m_FilenameHash);

    CacheResult::Enum cacheResult = CacheResult::Success;

    if (operation == kOperationRead && result.m_ReturnCode == 404)
    {
        cacheResult = CacheResult::CacheMiss;
    } else if (result.m_ReturnCode != 0)
    {
        processFailure(result.m_OutputBuffer.buffer);
        cacheResult = CacheResult::Failure;
    }
    ExecResultFreeMemory(&result);

    return cacheResult;
}

CacheResult::Enum CacheClient::AttemptRead(const Frozen::Dag* dag, const Frozen::DagNode* dagNode, HashDigest signature, StatCache* stat_cache, Mutex* queue_lock, ThreadState* thread_state)
{
    return Invoke_REAPI_Cache_Client(signature, stat_cache, dagNode->m_OutputFiles, thread_state, Operation::kOperationRead, dag, dagNode, queue_lock, nullptr );
}

CacheResult::Enum CacheClient::AttemptWrite(const Frozen::Dag* dag, const Frozen::DagNode* dagNode, HashDigest signature, StatCache* stat_cache, Mutex* queue_lock, ThreadState* thread_state, const char* ingredients_file)
{
    return Invoke_REAPI_Cache_Client(signature, stat_cache, dagNode->m_OutputFiles, thread_state, Operation::kOperationWrite, dag, dagNode, queue_lock, ingredients_file );
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

    for (const char* c_ptr = behaviour; ; c_ptr++)
    {
        char c = *c_ptr;
        if (c == 0)
            break;
        if (c == 'R')
        {
            *attemptReads = true;
            continue;
        }
        if (c == 'W')
        {
            *attemptWrites = true;
            continue;
        }
        Croak("The cache behaviour string provided: %s contains a character that is not R or W", behaviour);
    }

    Log(kDebug, "Caching enabled with %s=%s %s=%s and mode: %s%s%s\n", kENV_CACHE_SERVER_ADDRESS, server, kENV_REAPI_CACHE_CLIENT, reapi_cache_client, *attemptReads ? "R":"_", *attemptWrites ? "W":"_");
}
