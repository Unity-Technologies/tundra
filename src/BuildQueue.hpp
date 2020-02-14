#pragma once

#include "Common.hpp"
#include "Mutex.hpp"
#include "ConditionVar.hpp"
#include "Thread.hpp"
#include "MemAllocLinear.hpp"
#include "MemAllocHeap.hpp"
#include "JsonWriter.hpp"
#include "DagData.hpp"


struct MemAllocHeap;
struct RuntimeNode;
struct ScanCache;
struct StatCache;
struct DigestCache;

enum
{
    kMaxBuildThreads = 64
};

struct BuildQueueConfig
{
    enum
    {
        // Print command lines to the TTY as actions are executed.
        kFlagEchoCommandLines = 1 << 0,
        // Print annotations to the TTY as actions are executed
        kFlagEchoAnnotations = 1 << 1,
    };

    uint32_t m_Flags;
    MemAllocHeap *m_Heap;
    int m_ThreadCount;
    int m_ThrottleInactivityPeriod;
    const Frozen::DagNode *m_DagNodes;
    RuntimeNode *m_RuntimeNodes;
    int m_TotalRuntimeNodeCount;
    const int32_t *m_DagNodeIndexToRuntimeNodeIndex_Table;
    ScanCache *m_ScanCache;
    StatCache *m_StatCache;
    DigestCache *m_DigestCache;
    int m_ShaDigestExtensionCount;
    const uint32_t *m_ShaDigestExtensions;
    void *m_FileSigningLog;
    Mutex *m_FileSigningLogMutex;
    const Frozen::SharedResourceData *m_SharedResources;
    int m_SharedResourcesCount;
    bool m_ThrottleOnHumanActivity;
    int m_ThrottledThreadsAmount;
    bool m_DontReusePreviousResults;
};

struct BuildQueue;

struct ThreadState
{
    MemAllocHeap m_LocalHeap;
    MemAllocLinear m_ScratchAlloc;
    int m_ThreadIndex;
    int m_ProfilerThreadId;
    BuildQueue *m_Queue;
};

namespace BuildResult
{
enum Enum
{
    kOk = 0,                   // All nodes built successfully
    kInterrupted = 1,          // User interrupted the build (e.g CTRL+C)
    kBuildError = 2,           // At least one node failed to build
    kSetupError = 3,           // We couldn't set up the build
    kRequireFrontendRerun = 4, //Frontend needs to run again
    kCount
};

extern const char *Names[Enum::kCount];
} // namespace BuildResult

struct BuildQueue
{
    Mutex m_Lock;
    ConditionVariable m_WorkAvailable;
    ConditionVariable m_MaxJobsChangedConditionalVariable;
    ConditionVariable m_BuildFinishedConditionalVariable;
    Mutex m_BuildFinishedMutex;
    bool m_BuildFinishedConditionalVariableSignaled;

    int32_t *m_Queue;
    uint32_t m_QueueCapacity;
    uint32_t m_QueueReadIndex;
    uint32_t m_QueueWriteIndex;
    BuildQueueConfig m_Config;

    BuildResult::Enum m_FinalBuildResult;
    uint32_t m_FinishedNodeCount;

    ThreadId m_Threads[kMaxBuildThreads];
    ThreadState m_ThreadState[kMaxBuildThreads];
    uint32_t *m_SharedResourcesCreated;
    Mutex m_SharedResourcesLock;
    bool m_MainThreadWantsToCleanUp;
    uint32_t m_DynamicMaxJobs;
};

void BuildQueueInit(BuildQueue *queue, const BuildQueueConfig *config);

BuildResult::Enum BuildQueueBuild(BuildQueue *queue);

void BuildQueueDestroy(BuildQueue *queue);

bool HasBuildStoppingFailures(const BuildQueue *queue);
