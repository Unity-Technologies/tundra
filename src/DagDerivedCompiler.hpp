#pragma once;

namespace Frozen
{
    struct Dat;
}
struct MemAllocHeap;
struct MemAllocLinear;
struct StatCache;

bool CompileDagDerived(const Frozen::Dag* dag, MemAllocHeap* heap, MemAllocLinear* scratch, StatCache *stat_cache, const char* dagderived_filename);
