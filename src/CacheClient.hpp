#pragma once
#include "Hash.hpp"

namespace Frozen { struct DagNode; }
struct StatCache;
struct Mutex;
struct ThreadState;

struct CacheClient
{
    static bool CacheClient::AttemptRead(const Frozen::DagNode* dagNode, HashDigest signature, StatCache* stat_cache, Mutex* queue_lock, ThreadState* thread_state);
    static bool CacheClient::AttemptWrite(const Frozen::DagNode* dagNode, HashDigest signature, StatCache* stat_cache, Mutex* queue_lock, ThreadState* thread_state);
};

void GetCachingBehaviourSettingsFromEnvironment(bool* attemptReads, bool* attemptWrites);
