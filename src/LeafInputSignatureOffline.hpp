#pragma once

#include "Hash.hpp"
#include "Buffer.hpp"
#include <functional>

namespace Frozen
{
    struct Dag;
    struct DagDerived;
}
struct MemAllocHeap;

HashDigest CalculateLeafInputHashOffline(MemAllocHeap* heap, const Frozen::Dag* dag, int nodeIndex, FILE* ingredient_stream);
