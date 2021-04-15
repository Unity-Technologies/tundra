#pragma once

namespace Frozen
{
    struct Dag;
}
struct MemAllocHeap;

//returns true if a cycle was found
bool DetectCyclicDependencies(const Frozen::Dag* dag, MemAllocHeap* heap);
