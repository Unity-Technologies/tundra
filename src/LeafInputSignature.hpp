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
struct Driver;

HashDigest CalculateLeafInputSignature(BuildQueue* queue, ThreadState* thread_state, RuntimeNode* node);
HashDigest CalculateLeafInputHashOffline(const Frozen::Dag* dag, std::function<const int32_t*(int)>& arrayAccess, std::function<size_t(int)>& sizeAccess, int32_t nodeIndex, MemAllocHeap* heap, FILE* ingredient_stream);
bool VerifyAllVersionedFilesIncludedByGeneratedHeaderFilesWereAlreadyPartOfTheLeafInputs(BuildQueue* queue, ThreadState* thread_state, RuntimeNode* node, const Frozen::DagDerived* dagDerived);
void PrintLeafInputSignature(Driver* driver, const char **argv, int argc);
