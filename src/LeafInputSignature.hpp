#pragma once
#include "Hash.hpp"
#include "HashTable.hpp"
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

struct LeafInputSignatureData
{
    HashDigest digest;
    HashSet<kFlagPathStrings> m_ExplicitLeafInputs;
    HashSet<kFlagPathStrings> m_ImplicitLeafInputs;
};

void DestroyLeafInputSignatureData(MemAllocHeap *heap, LeafInputSignatureData *data);
void CalculateLeafInputSignature(BuildQueue* queue, ThreadState* thread_state, RuntimeNode* node);
bool VerifyAllVersionedFilesIncludedByGeneratedHeaderFilesWereAlreadyPartOfTheLeafInputs(BuildQueue* queue, ThreadState* thread_state, RuntimeNode* node, const Frozen::DagDerived* dagDerived);
void PrintLeafInputSignature(Driver* driver, const char **argv, int argc);
