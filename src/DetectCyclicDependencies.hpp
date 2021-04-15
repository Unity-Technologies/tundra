#pragma once

namespace Frozen
{
    struct Dag;
}
struct MemAllocHeap;

void DetectCyclicDependencies(const Frozen::Dag* dag, MemAllocHeap* heap);

