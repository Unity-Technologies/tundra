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

HashDigest CalculateLeafInputSignatureRuntime(BuildQueue* queue, ThreadState* thread_state, RuntimeNode* node);
bool VerifyAllVersionedFilesIncludedByGeneratedHeaderFilesWereAlreadyPartOfTheLeafInputs(BuildQueue* queue, ThreadState* thread_state, RuntimeNode* node, const Frozen::DagDerived* dagDerived);
void PrintLeafInputSignature(Driver* driver, const char **argv, int argc);
