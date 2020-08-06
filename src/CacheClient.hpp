#pragma once
#include "Hash.hpp"

namespace Frozen { struct DagNode; }
struct StatCache;
struct Mutex;
struct ThreadState;

struct CacheClient
{
    static bool AttemptRead(const Frozen::DagNode* dagNode, HashDigest signature, StatCache* stat_cache, Mutex* queue_lock, ThreadState* thread_state);
    static bool AttemptWrite(const Frozen::DagNode* dagNode, HashDigest signature, StatCache* stat_cache, Mutex* queue_lock, ThreadState* thread_state);
};

void GetCachingBehaviourSettingsFromEnvironment(bool* attemptReads, bool* attemptWrites);

#define kCacheSignaturesDirectory "artifacts/signatures"