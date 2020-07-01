#pragma once;

namespace Frozen
{
    struct Dat;
}
struct MemAllocHeap;
struct MemAllocLinear;

bool CompileDagDerived(const Frozen::Dag* dag, MemAllocHeap* heap, MemAllocLinear* scratch, const char* dagderived_filename);
