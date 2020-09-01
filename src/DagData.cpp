#include "DagData.hpp"
#include "Buffer.hpp"
#include "HashTable.hpp"

void FindDependentNodesFromRootIndex_IncludingSelf_NotRecursingIntoCacheableNodes(MemAllocHeap* heap, const Frozen::Dag* dag, const Frozen::DagNode& dagNode, Buffer<int32_t>& results, Buffer<int32_t>* dependenciesThatAreCacheableThemselves)
{
    Buffer<int32_t> node_stack;
    BufferInitWithCapacity(&node_stack, heap, 1024);

    int node_count = 0;
    const size_t node_word_count = (dag->m_NodeCount + 31) / 32;
    uint32_t *node_visited_bits = HeapAllocateArrayZeroed<uint32_t>(heap, node_word_count);

    auto AddToResultsAndNodeStackIfNotYetAdded = [&](int dag_index, bool isRootSearchNode = false)
    {
        auto& dagNode = dag->m_DagNodes[dag_index];

        const int dag_word = dag_index / 32;
        const int dag_bit = 1 << (dag_index & 31);
        if (0 == (node_visited_bits[dag_word] & dag_bit))
        {
            if ((dagNode.m_Flags & Frozen::DagNode::kFlagCacheableByLeafInputs) && !isRootSearchNode)
            {
                if (dependenciesThatAreCacheableThemselves)
                    BufferAppendOne(dependenciesThatAreCacheableThemselves, heap, dag_index);
                return;
            }

            node_visited_bits[dag_word] |= dag_bit;
            BufferAppendOne(&results, heap, dag_index);
            BufferAppendOne(&node_stack, heap, dag_index);
        }
    };

    AddToResultsAndNodeStackIfNotYetAdded(dagNode.m_DagNodeIndex, true);

    AddToResultsAndNodeStackIfNotYetAdded(dagNode.m_DagNodeIndex);
    if (dependenciesThatAreCacheableThemselves)
        BufferClear(dependenciesThatAreCacheableThemselves);

    while (node_stack.m_Size > 0)
    {
        int dag_index = BufferPopOne(&node_stack);

        for (auto buildDependency: dag->m_DagNodes[dag_index].m_ToBuildDependencies)
            AddToResultsAndNodeStackIfNotYetAdded(buildDependency);
        for (auto useDependency: dag->m_DagNodes[dag_index].m_ToUseDependencies)
            AddToResultsAndNodeStackIfNotYetAdded(useDependency);
    }

    HeapFree(heap, node_visited_bits);
    BufferDestroy(&node_stack, heap);
}

void DagRuntimeDataInit(DagRuntimeData* data, const Frozen::Dag* dag, MemAllocHeap *heap)
{
    HashTableInit(&data->m_OutputsToDagNodes, heap);
    HashTableInit(&data->m_OutputDirectoriesToDagNodes, heap);
    for (int i = 0; i<dag->m_NodeCount; i++)
    {
        const Frozen::DagNode* node = dag->m_DagNodes + i;
        for(auto &output: node->m_OutputFiles)
            HashTableInsert(&data->m_OutputsToDagNodes, output.m_FilenameHash, output.m_Filename.Get(), i);
        for(auto &output: node->m_OutputDirectories)
            HashTableInsert(&data->m_OutputDirectoriesToDagNodes, output.m_FilenameHash, output.m_Filename.Get(), i);
    }

    // We currently don't populate m_OutputDirectories for all nodes.
    // Some output directries still only show up in m_DirectoriesCausingImplicitDependencies.
    // For those, we don't know which node they came from, so we use the special index value of -1.
    for (auto& d: dag->m_DirectoriesCausingImplicitDependencies)
        HashTableInsert(&data->m_OutputDirectoriesToDagNodes, d.m_FilenameHash, d.m_Filename.Get(), -1);

    data->m_Dag = dag;
}

void DagRuntimeDataDestroy(DagRuntimeData* data)
{
    HashTableDestroy(&data->m_OutputsToDagNodes);
    HashTableDestroy(&data->m_OutputDirectoriesToDagNodes);
}

bool FindDagNodeForFile(const DagRuntimeData* data, uint32_t filenameHash, const char* filename, const Frozen::DagNode **result)
{
    if (int* nodeIndex = HashTableLookup(&data->m_OutputsToDagNodes, filenameHash, filename))
    {
        *result = data->m_Dag->m_DagNodes + *nodeIndex;
        return true;
    }

    PathBuffer filePath;
    PathInit(&filePath, filename);

    while (PathStripLast(&filePath))
    {
        char path[kMaxPathLength];
        PathFormat(path, &filePath);
        if (int* nodeIndex = HashTableLookup(&data->m_OutputDirectoriesToDagNodes, Djb2HashPath(path), path))
        {
            if (*nodeIndex == -1)
                *result = nullptr;
            else
                *result = data->m_Dag->m_DagNodes + *nodeIndex;
            return true;
        }
    }
    return false;
}

bool IsFileGenerated(const DagRuntimeData* data, uint32_t filenameHash, const char* filename)
{
    const Frozen::DagNode *dummy;
    return FindDagNodeForFile(data, filenameHash, filename, &dummy);
}
