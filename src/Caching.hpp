#pragma once
#include "DigestCache.hpp"
#include "BinaryData.hpp"

struct RuntimeNode;
struct ThreadState;
struct Mutex;

HashDigest ComputeLeafInputSignature(BuildQueueConfig* config, ThreadState* thread_state, const Frozen::DagNode* dagNode);

namespace CacheMode
{
    enum CacheMode
    {
        kLookUp,
        kPost
    };
};

bool InvokeCacheMe(const HashDigest& digest, StatCache *stat_cache, const FrozenArray<FrozenFileAndHash>& outputFiles, ThreadState* thread_state, CacheMode::CacheMode mode, const Frozen::DagNode* dagNode, Mutex* queue_lock);

