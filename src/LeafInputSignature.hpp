#pragma once
#include "Hash.hpp"
#include "Buffer.hpp"
#include <functional>
#include "HashTable.hpp"

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

void CalculateLeafInputSignatureRuntime(BuildQueue* queue, ThreadState* thread_state, RuntimeNode* node, FILE* ingredient_stream);
bool VerifyAllVersionedFilesIncludedByGeneratedHeaderFilesWereAlreadyPartOfTheLeafInputs(BuildQueue* queue, ThreadState* thread_state, RuntimeNode* node, const Frozen::DagDerived* dagDerived);
void PrintLeafInputSignature(BuildQueue* buildQueue);
