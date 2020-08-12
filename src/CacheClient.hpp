#pragma once
#include "Hash.hpp"

namespace Frozen { struct DagNode; struct Dag; }
struct StatCache;
struct Mutex;
struct ThreadState;

namespace CacheResult
{
    enum Enum
    {
        Failure,
        CacheMiss,
        Success,
    };
}


struct CacheClient
{
    static CacheResult::Enum AttemptRead(const Frozen::Dag* dag, const Frozen::DagNode* dagNode, HashDigest signature, StatCache* stat_cache, Mutex* queue_lock, ThreadState* thread_state);
    static CacheResult::Enum AttemptWrite(const Frozen::Dag* dag, const Frozen::DagNode* dagNode, HashDigest signature, StatCache* stat_cache, Mutex* queue_lock, ThreadState* thread_state);
};

void GetCachingBehaviourSettingsFromEnvironment(bool* attemptReads, bool* attemptWrites);
