#pragma once
#include "DigestCache.hpp"
#include "BinaryData.hpp"

struct RuntimeNode;
struct ThreadState;

HashDigest ComputeLeafInputSignature(RuntimeNode* node);

namespace CacheMode
{
    enum CacheMode
    {
        kLookUp,
        kPost
    };
};

bool InvokeCacheMe(const HashDigest& digest, const FrozenArray<FrozenFileAndHash>& outputFiles, ThreadState* thread_state, CacheMode::CacheMode mode);
