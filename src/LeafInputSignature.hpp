#pragma once
#include "Hash.hpp"

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

HashDigest ComputeLeafInputSignature(const Frozen::Dag* dag, const Frozen::DagDerived* dagDerived, const Frozen::DagNode* dagNode, MemAllocHeap* heap, MemAllocLinear* scratch, int profilerThreadId, StatCache* stat_cache, DigestCache* digest_cache, ScanCache* scan_cache, FILE* ingredient_stream);
void PrintLeafInputSignature(Driver* driver, const char **argv, int argc);
