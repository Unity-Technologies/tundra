#pragma once
#include "DigestCache.hpp"
#include "BinaryData.hpp"

struct RuntimeNode;
struct ThreadState;

HashDigest ComputeLeafInputSignature(BuildQueueConfig* config, ThreadState* thread_state, const Frozen::DagNode* dagNode);
bool IsFileGenerated(const Frozen::Dag* dag, const char* filename);

namespace CacheMode
{
    enum CacheMode
    {
        kLookUp,
        kPost
    };
};

bool InvokeCacheMe(const HashDigest& digest, StatCache *stat_cache, const FrozenArray<FrozenFileAndHash>& outputFiles, ThreadState* thread_state, CacheMode::CacheMode mode);
