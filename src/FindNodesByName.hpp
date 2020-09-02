#pragma once
#include "Buffer.hpp"
#include "BinaryData.hpp"

namespace Frozen
{
    struct Dag;
    struct NamedNodeData;
}

struct MemAllocHeap;

void FindNodesByName(
    const Frozen::Dag *dag,
    Buffer<int32_t> *out_nodes,
    MemAllocHeap *heap,
    const char **names,
    size_t name_count,
    const FrozenArray<Frozen::NamedNodeData> &named_nodes);