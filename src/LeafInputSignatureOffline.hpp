#pragma once

#include "Hash.hpp"
#include "Buffer.hpp"
#include <functional>

namespace Frozen
{
    struct Dag;
    struct DagDerived;
}
struct RuntimeNode;
struct ThreadState;
struct MemAllocHeap;
struct BuildQueue;
struct BuildQueueConfig;
struct Driver;

HashDigest CalculateLeafInputHashOffline(const BuildQueueConfig& queueConfig, int32_t nodeIndex, MemAllocHeap* heap, FILE* ingredient_stream);
