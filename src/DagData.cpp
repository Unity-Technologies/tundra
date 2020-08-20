#include "DagData.hpp"
#include "Buffer.hpp"
#include "HashTable.hpp"

static void FindDependentNodesFromRootIndices(MemAllocHeap* heap, const Frozen::Dag* dag, std::function<const int32_t*(int)>& arrayAccess, std::function<size_t(int)>& sizeAccess, std::function<bool(int,int)>& shouldProcess, int32_t* searchRootIndices, int32_t searchRootCount, Buffer<int32_t>& results)
{
    Buffer<int32_t> node_stack;
    BufferInitWithCapacity(&node_stack, heap, 1024);
    BufferAppend(&node_stack, heap, searchRootIndices, searchRootCount);

    int node_count = 0;
    const size_t node_word_count = (dag->m_NodeCount + 31) / 32;
    uint32_t *node_visited_bits = HeapAllocateArrayZeroed<uint32_t>(heap, node_word_count);

    while (node_stack.m_Size > 0)
    {
        int dag_index = BufferPopOne(&node_stack);
        const int dag_word = dag_index / 32;
        const int dag_bit = 1 << (dag_index & 31);

        if (0 == (node_visited_bits[dag_word] & dag_bit))
        {
            BufferAppendOne(&results, heap, dag_index);

            node_visited_bits[dag_word] |= dag_bit;

            // Update counts
            ++node_count;

            // Stash node dependencies on the work queue to keep iterating

            int dependencyCount = sizeAccess(dag_index);
            const int32_t* dependencyArray = arrayAccess(dag_index);

            for (int i=0; i!=dependencyCount; i++)
            {
                int childIndex = dependencyArray[i];
                if (shouldProcess(dag_index, childIndex))
                    BufferAppendOne(&node_stack, heap, childIndex);
            }
        }
    }

    HeapFree(heap, node_visited_bits);
    BufferDestroy(&node_stack, heap);
    node_visited_bits = nullptr;
}

void FindDependentNodesFromRootIndices(MemAllocHeap* heap, const Frozen::Dag* dag, const Frozen::DagDerived* dagDerived, int32_t* searchRootIndices, int32_t searchRootCount, Buffer<int32_t>& results)
{
    std::function<const int32_t*(int)> arrayAccess = [=](int index){return dagDerived->m_Dependencies[index].GetArray();};
    std::function<size_t(int)> sizeAccess = [=](int index){return dagDerived->m_Dependencies[index].GetCount();};
    std::function<bool(int,int)> alwaysTrue = [](int,int) { return true; };
    FindDependentNodesFromRootIndices(heap, dag, arrayAccess, sizeAccess, alwaysTrue, searchRootIndices, searchRootCount, results);
}

void FindDependentNodesFromRootIndex(MemAllocHeap* heap, const Frozen::Dag* dag, std::function<const int32_t*(int)>& arrayAccess, std::function<size_t(int)>& sizeAccess, std::function<bool(int,int)>& shouldProcess, int32_t rootIndex, Buffer<int32_t>& results)
{
    FindDependentNodesFromRootIndices(heap, dag, arrayAccess, sizeAccess, shouldProcess, &rootIndex, 1, results);
}

void FindDependentNodesFromRootIndex(MemAllocHeap* heap, const Frozen::Dag* dag, const Frozen::DagDerived* dagDerived, int32_t rootIndex, Buffer<int32_t>& results)
{
    FindDependentNodesFromRootIndices(heap, dag, dagDerived, &rootIndex, 1, results);
}

void FindAllOutputFiles(const Frozen::Dag* dag, HashSet<kFlagPathStrings>& outputFiles)
{
    int node_count = dag->m_NodeCount;
    for (int32_t i = 0; i < node_count; ++i)
        for (auto& outputFile : dag->m_DagNodes[i].m_OutputFiles)
            HashSetInsert(&outputFiles, outputFile.m_FilenameHash, outputFile.m_Filename.Get());
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
    // For those, we don't know which node they came from, so we ise the special index value of -1.
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
