#pragma once

#include "Common.hpp"
#include "Mutex.hpp"
#include "ConditionVar.hpp"
#include "Thread.hpp"
#include "MemAllocLinear.hpp"
#include "MemAllocHeap.hpp"
#include "JsonWriter.hpp"
#include "DagData.hpp"
#include "BuildLoop.hpp"
#include "Buffer.hpp"

struct MemAllocHeap;
struct RuntimeNode;
struct ScanCache;
struct StatCache;
struct DigestCache;
struct DriverOptions;

enum
{
    kMaxBuildThreads = 128
};

struct BuildQueueConfig
{
    enum
    {
        // Print command lines to the TTY as actions are executed.
        kFlagEchoCommandLines = 1 << 0,
    };

    const DriverOptions* m_DriverOptions;
    uint32_t m_Flags;
    MemAllocHeap *m_Heap;
    MemAllocLinear *m_LinearAllocator;
    const Frozen::Dag* m_Dag;
    const Frozen::DagNode *m_DagNodes;
    const Frozen::DagDerived* m_DagDerived;
    DagRuntimeData m_DagRuntimeData;
    RuntimeNode *m_RuntimeNodes;
    int m_TotalRuntimeNodeCount;
    Buffer<int32_t> m_RequestedNodes;
    ScanCache *m_ScanCache;
    StatCache *m_StatCache;
    DigestCache *m_DigestCache;
    int m_ShaDigestExtensionCount;
    const uint32_t *m_ShaDigestExtensions;
    void *m_FileSigningLog;
    Mutex *m_FileSigningLogMutex;
    const Frozen::SharedResourceData *m_SharedResources;
    int m_SharedResourcesCount;
    bool m_AttemptCacheReads;
    bool m_AttemptCacheWrites;
};

struct BuildQueue;

struct ThreadState
{
    MemAllocHeap m_LocalHeap;
    MemAllocLinear m_ScratchAlloc;
    int m_ThreadIndex;
    int m_ProfilerThreadId;
    BuildQueue *m_Queue;
    Buffer<uint64_t> m_TimestampStorage;

    // For tracking which invalidated glob/file signature is causing a frontend rerun to be required
    // Only storing one of each type is sufficient for figuring out what message to give the user
    const Frozen::DagGlobSignature *m_GlobCausingFrontendRerun;
    const FrozenString *m_FileCausingFrontendRerun;
};

namespace BuildResult
{
// Note: these need to be kept in sync with the BuildResult::Names defined
// in the source file.
enum Enum
{
    kOk = 0,                   // All nodes built successfully
    kInterrupted = 1,          // User interrupted the build (e.g CTRL+C)
    kCroak = 2,                // An internal really bad error happened
    kBuildError = 3,           // We couldn't set up the build
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

    Buffer<int32_t> m_WorkStack;
    BuildQueueConfig m_Config;

    BuildResult::Enum m_FinalBuildResult;
    uint32_t m_FinishedNodeCount;
    uint32_t m_AmountOfNodesEverQueued;

    ThreadId m_Threads[kMaxBuildThreads];
    ThreadState m_ThreadState[kMaxBuildThreads];
    uint32_t *m_SharedResourcesCreated;
    Mutex m_SharedResourcesLock;
    bool m_MainThreadWantsToCleanUp;
};

void BuildQueueInit(BuildQueue *queue, const BuildQueueConfig *config, const char** targets, int target_count);

BuildResult::Enum BuildQueueBuild(BuildQueue *queue, MemAllocLinear* scratch);

void BuildQueueDestroy(BuildQueue *queue);

bool HasBuildStoppingFailures(const BuildQueue *queue);

static const int kRerunReasonBufferSize = kMaxPathLength + 128;
void BuildQueueGetFrontendRerunReason(BuildQueue* queue, char* out_frontend_rerun_reason);
