#pragma once
#include "DigestCache.hpp"
#include "BinaryData.hpp"

struct RuntimeNode;
struct ThreadState;

HashDigest ComputeLeafInputSignature(BuildQueueConfig* config, const Frozen::DagNode* dagNode);

namespace CacheMode
{
    enum CacheMode
    {
        kLookUp,
        kPost
    };
};

bool InvokeCacheMe(const HashDigest& digest, StatCache *stat_cache, const FrozenArray<FrozenFileAndHash>& outputFiles, ThreadState* thread_state, CacheMode::CacheMode mode);
