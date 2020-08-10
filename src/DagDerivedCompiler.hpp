#pragma once;

namespace Frozen
{
    struct Dag;
}
struct MemAllocHeap;
struct MemAllocLinear;
struct StatCache;

bool CompileDagDerived(const Frozen::Dag* dag, MemAllocHeap* heap, MemAllocLinear* scratch, StatCache *stat_cache, const char* dagderived_filename);
