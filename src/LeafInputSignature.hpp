#pragma once
#include "Hash.hpp"
#include "Buffer.hpp"
#include <functional>

namespace Frozen
{
    struct Dag;
    struct DagDerived;
    struct DagNode;
}
struct RuntimeNode;
struct ThreadState;
struct MemAllocHeap;
struct MemAllocLinear;
struct StatCache;
struct DigestCache;
struct Driver;
struct ScanCache;
struct DagRuntimeData;
struct BuildQueue;

HashDigest ComputeLeafInputSignature(const int32_t* dagNodeIndexToRuntimeNodeIndex_Table, RuntimeNode* runtimeNodesArray, const Frozen::Dag* dag, const Frozen::DagDerived* dagDerived, const DagRuntimeData *dagRuntime, const Frozen::DagNode* dagNode, MemAllocHeap* heap, MemAllocLinear* scratch, int profilerThreadId, StatCache* stat_cache, DigestCache* digest_cache, ScanCache* scan_cache, FILE* ingredient_stream);
HashDigest CalculateLeafInputHashOffline(const Frozen::Dag* dag, std::function<const int32_t*(int)>& arrayAccess, std::function<size_t(int)>& sizeAccess, int32_t nodeIndex, MemAllocHeap* heap, FILE* ingredient_stream);

void PrintLeafInputSignature(Driver* driver, const char **argv, int argc);
