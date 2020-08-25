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

HashDigest CalculateLeafInputHashOffline_FromDependencyBuffers(MemAllocHeap* heap, const Frozen::Dag* dag, Buffer<int32_t>* dependencyBuffers, int nodeIndex);
HashDigest CalculateLeafInputHashOffline_FromDagDerived(const Frozen::Dag* dag, const Frozen::DagDerived* dagDerived, int32_t nodeIndex, MemAllocHeap* heap, FILE* ingredient_stream);
