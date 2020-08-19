#include "DagGenerator.hpp"
#include "Hash.hpp"
#include "PathUtil.hpp"
#include "Exec.hpp"
#include "FileInfo.hpp"
#include "MemAllocHeap.hpp"
#include "MemAllocLinear.hpp"
#include "JsonParse.hpp"
#include "BinaryWriter.hpp"
#include "DagData.hpp"
#include "HashTable.hpp"
#include "FileSign.hpp"
#include "BuildQueue.hpp"
#include "LeafInputSignature.hpp"
#include "CacheClient.hpp"
#include "MakeDirectories.hpp"
#include "StatCache.hpp"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <algorithm>

static void SortBufferOfFileAndHash(Buffer<FileAndHash>& buffer)
{
    std::sort(buffer.begin(), buffer.end(), [](const FileAndHash& a, const FileAndHash& b) { return strcmp(a.m_Filename, b.m_Filename) < 0; });
}

static bool HasFlag(int value, int flag)
{
    return (value & flag) != 0;
}

struct CompileDagDerivedWorker
{
    BinaryWriter _writer;
    BinaryWriter* writer;
    HashTable<CommonStringRecord, kFlagCaseSensitive> shared_strings;

    BinarySegment *main_seg;
    BinarySegment *data_seg;
    BinarySegment *arraydata_seg;
    BinarySegment *arraydata2_seg;
    BinarySegment *leafnodearray_seg;
    BinarySegment *cacheableDependenciesArray_seg;
    BinarySegment *dependenciesWithScannersArray_seg;
    BinarySegment *scannerindex_seg;
    BinarySegment *str_seg;

    DagRuntimeData dagRuntimeData;
    const Frozen::Dag* dag;
    MemAllocHeap*  heap;
    MemAllocLinear* scratch;
    int node_count;
    StatCache *stat_cache;

    Buffer<int32_t> *deps;
    Buffer<int32_t> *links;

    void WriteFrozenArrayOfBackLinks()
    {
        Buffer<int32_t> *links = HeapAllocateArrayZeroed<Buffer<int32_t>>(heap, node_count);
        for (int32_t i = 0; i < node_count; ++i)
        {
            for(int dep : dag->m_DagNodes[i].m_OriginalDependencies)
            BufferAppendOne(&links[dep], heap, i);
        }
        BinarySegmentWriteUint32(main_seg, node_count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(data_seg));
        for (int32_t i = 0; i < node_count; ++i)
        {
            BinarySegmentWriteInt32(data_seg, links[i].m_Size);
            BinarySegmentWritePointer(data_seg, BinarySegmentPosition(arraydata_seg));
            for(int32_t backLink : links[i])
                BinarySegmentWriteInt32(arraydata_seg, backLink);
        }
        for (size_t i = 0; i < node_count; ++i)
            BufferDestroy(&links[i], heap);
        HeapFree(heap, links);
    }

    void WriteFrozenArrayOfDependenciesAndBackLinks()
    {
        deps = HeapAllocateArrayZeroed<Buffer<int32_t>>(heap, node_count);
        for (int32_t i = 0; i < node_count; ++i)
        {
            for(int dep : dag->m_DagNodes[i].m_OriginalDependencies)
            {
                BufferAppendOne(&deps[i], heap, dep);
                for(int dep2 : dag->m_DagNodes[dep].m_DependenciesConsumedDuringUsageOnly)
                    BufferAppendOne(&deps[i], heap, dep2);
            }
        }

        links = HeapAllocateArrayZeroed<Buffer<int32_t>>(heap, node_count);
        for (int32_t i = 0; i < node_count; ++i)
        {
            for(int dep : deps[i])
                BufferAppendOne(&links[dep], heap, i);
        }

        auto writeArray = [=](Buffer<int32_t>* array)->void{
            BinarySegmentWriteUint32(main_seg, node_count);
            BinarySegmentWritePointer(main_seg, BinarySegmentPosition(data_seg));
            for (int32_t i = 0; i < node_count; ++i)
            {
                BinarySegmentWriteInt32(data_seg, array[i].m_Size);
                BinarySegmentWritePointer(data_seg, BinarySegmentPosition(arraydata_seg));
                for(int32_t dep : array[i])
                    BinarySegmentWriteInt32(arraydata_seg, dep);
            }
        };
        writeArray(deps);
        writeArray(links);
    }

    struct ScannerIndexWithListOfFiles
    {
        int32_t m_ScannerIndex;
        Buffer<FileAndHash> m_FilesToScan;
    };

    struct PerNodeWorkerData
    {
        Buffer<FileAndHash> leafInputBuffer;
        HashSet<kFlagPathStrings> m_AlreadyProcessedFiles;
        Buffer<ScannerIndexWithListOfFiles> scannersWithListsOfFiles;
        Buffer<int32_t> all_dependent_nodes;
        Buffer<int32_t> recursive_dependencies_with_scanners;
    };

    void PerNodeWorkerDataInit(PerNodeWorkerData* data)
    {
        BufferInit(&data->scannersWithListsOfFiles);
        BufferInit(&data->leafInputBuffer);
        BufferInit(&data->all_dependent_nodes);
        BufferInit(&data->recursive_dependencies_with_scanners);
        HashSetInit(&data->m_AlreadyProcessedFiles, heap);
    }

    void PerNodeWorkerDataDestroy(PerNodeWorkerData* data)
    {
        BufferDestroy(&data->scannersWithListsOfFiles, heap);
        BufferDestroy(&data->leafInputBuffer, heap);
        BufferDestroy(&data->all_dependent_nodes, heap);
        BufferDestroy(&data->recursive_dependencies_with_scanners, heap);
        HashSetDestroy(&data->m_AlreadyProcessedFiles);
    }

    PerNodeWorkerData m_PerNodeWorkerData;

    void PerNode_ProcessDiscoveredExplicitLeafInput(const FrozenFileAndHash& file, const Frozen::DagNode& childDagNode, int scannerIndex)
    {
        const Frozen::DagNode* fileGeneratingNode;

        if (FindDagNodeForFile(&dagRuntimeData, file.m_FilenameHash, file.m_Filename.Get(), &fileGeneratingNode))
        {
            //ok, so this means it's a generated file.  we do not recurse into those, but we will add their FilesThatMightBeIncluded

            if (fileGeneratingNode == nullptr)
            {
                //this can happen if we do know a file is generated, but we do not know which node made it. Only happens with DirectoriesCreatingImplicitDependencies,
                //which we should remove.
                return;
            }

            for(auto& f: fileGeneratingNode->m_FilesThatMightBeIncluded)
                PerNode_ProcessDiscoveredExplicitLeafInput(f, *fileGeneratingNode, scannerIndex);

            return;
        }

        //ok, not a generated file, so this is a leaf input.

        //maybe we already processed this one before?
        if (!HashSetInsertIfNotPresent(&m_PerNodeWorkerData.m_AlreadyProcessedFiles, file.m_FilenameHash, file.m_Filename.Get()))
        {
            //yeah we did, we're done here.
            return;
        }

        //so we will add the file to our list of found leaf input files.
        FileAndHash leafInputFile;
        leafInputFile.m_Filename = file.m_Filename.Get();
        leafInputFile.m_FilenameHash = file.m_FilenameHash;
        BufferAppendOne(&m_PerNodeWorkerData.leafInputBuffer, heap, leafInputFile);

        //and if it doesn't have a scanner we are done here.
        if (scannerIndex == -1)
            return;

        //if it does have a scanner, we need to make this sure this file will get scanned with its scanner as part of runtime leaf input signature creation.
        auto findOrMakeScannerWithListOfFilesFor = [&](int scannerIndex) -> ScannerIndexWithListOfFiles*
        {
            for (auto& s: m_PerNodeWorkerData.scannersWithListsOfFiles)
                if (s.m_ScannerIndex == scannerIndex)
                    return &s;
            ScannerIndexWithListOfFiles* newEntry = BufferAlloc(&m_PerNodeWorkerData.scannersWithListsOfFiles, heap, 1);
            newEntry->m_ScannerIndex = scannerIndex;
            BufferInit(&newEntry->m_FilesToScan);
            return newEntry;
        };

        //for each cacheable node, we keep a list of ScannerIndexWithListOfFiles. Let's see if there is already an entry for our scanner. if there isn't we make one.
        ScannerIndexWithListOfFiles* scannerIndexWithListsOfFiles = findOrMakeScannerWithListOfFilesFor(scannerIndex);

        //and add this file to the list for that scanner.
        BufferAppendOne(&scannerIndexWithListsOfFiles->m_FilesToScan, heap, leafInputFile);
    }

    void WriteLeafInputsAndScannerIndicesToFilesToScan_ForSingleNode(int nodeIndex)
    {
        const Frozen::DagNode& node = dag->m_DagNodes[nodeIndex];
        if (!HasFlag(node.m_Flags,Frozen::DagNode::kFlagCacheableByLeafInputs))
        {
            BinarySegmentWriteInt32(leafnodearray_seg, 0);
            BinarySegmentWriteNullPointer(leafnodearray_seg);

            BinarySegmentWriteInt32(scannerindex_seg, 0);
            BinarySegmentWriteNullPointer(scannerindex_seg);

            BinarySegmentWriteInt32(cacheableDependenciesArray_seg, 0);
            BinarySegmentWriteNullPointer(cacheableDependenciesArray_seg);

            BinarySegmentWriteInt32(dependenciesWithScannersArray_seg, 0);
            BinarySegmentWriteNullPointer(dependenciesWithScannersArray_seg);
            return;
        }

        PerNodeWorkerDataInit(&m_PerNodeWorkerData);

        for (const auto& ignoredInput: node.m_CachingInputIgnoreList)
            HashSetInsertIfNotPresent(&m_PerNodeWorkerData.m_AlreadyProcessedFiles, ignoredInput.m_FilenameHash, ignoredInput.m_Filename.Get());

        std::function<const int32_t*(int)> arrayAccess = [=](int index){return deps[index].begin();};
        std::function<size_t(int)> sizeAccess = [=](int index){return deps[index].m_Size;};

        Buffer<int32_t> dependenciesThatAreLeafInputCacheableThemselves;
        BufferInit(&dependenciesThatAreLeafInputCacheableThemselves);

        std::function<bool(int,int)> filterLeafInputCacheable = [&](int parentIndex, int childIndex)
        {
            bool isCacheable = HasFlag(dag->m_DagNodes[childIndex].m_Flags,Frozen::DagNode::kFlagCacheableByLeafInputs);
            if (isCacheable)
            {
                if (std::find(dependenciesThatAreLeafInputCacheableThemselves.begin(), dependenciesThatAreLeafInputCacheableThemselves.end(), childIndex) == dependenciesThatAreLeafInputCacheableThemselves.end())
                    BufferAppendOne(&dependenciesThatAreLeafInputCacheableThemselves, heap, childIndex);
                return false;
            }

            return true;
        };

        FindDependentNodesFromRootIndex(heap, dag, arrayAccess, sizeAccess, filterLeafInputCacheable, nodeIndex, m_PerNodeWorkerData.all_dependent_nodes);

        for(int32_t childNodeIndex : m_PerNodeWorkerData.all_dependent_nodes)
        {
            const Frozen::DagNode& childDagNode = dag->m_DagNodes[childNodeIndex];

            for (auto& inputFile: childDagNode.m_InputFiles)
                PerNode_ProcessDiscoveredExplicitLeafInput(inputFile, childDagNode, childDagNode.m_ScannerIndex);
            for (auto& fileThatMightBeIncluded: childDagNode.m_FilesThatMightBeIncluded)
                PerNode_ProcessDiscoveredExplicitLeafInput(fileThatMightBeIncluded, childDagNode, childDagNode.m_ScannerIndex);

            if (childDagNode.m_ScannerIndex != -1)
                BufferAppendOne(&m_PerNodeWorkerData.recursive_dependencies_with_scanners, heap, childDagNode.m_DagNodeIndex);
        }

        auto& leafInputBuffer = m_PerNodeWorkerData.leafInputBuffer;
        SortBufferOfFileAndHash(leafInputBuffer);

        BinarySegmentWriteInt32(leafnodearray_seg, leafInputBuffer.m_Size);
        BinarySegmentWritePointer(leafnodearray_seg, BinarySegmentPosition(arraydata_seg));
        for(const FileAndHash& leafInput: leafInputBuffer)
        {
            WriteCommonStringPtr(arraydata_seg, str_seg, leafInput.m_Filename, &shared_strings, scratch);
            BinarySegmentWriteInt32(arraydata_seg, leafInput.m_FilenameHash);
        }


        BinarySegmentWriteInt32(cacheableDependenciesArray_seg, dependenciesThatAreLeafInputCacheableThemselves.m_Size);
        BinarySegmentWritePointer(cacheableDependenciesArray_seg, BinarySegmentPosition(arraydata_seg));
        for(int i: dependenciesThatAreLeafInputCacheableThemselves)
            BinarySegmentWriteInt32(arraydata_seg, i);
        BufferDestroy(&dependenciesThatAreLeafInputCacheableThemselves, heap);

        BinarySegmentWriteInt32(scannerindex_seg, m_PerNodeWorkerData.scannersWithListsOfFiles.m_Size);
        BinarySegmentWritePointer(scannerindex_seg, BinarySegmentPosition(arraydata_seg));
        for(ScannerIndexWithListOfFiles& scannerIndexWithListOfFiles: m_PerNodeWorkerData.scannersWithListsOfFiles)
        {
            BinarySegmentWriteInt32(arraydata_seg, scannerIndexWithListOfFiles.m_ScannerIndex);
            BinarySegmentWriteInt32(arraydata_seg, scannerIndexWithListOfFiles.m_FilesToScan.m_Size);
            BinarySegmentWritePointer(arraydata_seg, BinarySegmentPosition(arraydata2_seg));

            SortBufferOfFileAndHash(scannerIndexWithListOfFiles.m_FilesToScan);
            for(const FileAndHash& fileForScanner: scannerIndexWithListOfFiles.m_FilesToScan)
            {
                WriteCommonStringPtr(arraydata2_seg, str_seg, fileForScanner.m_Filename, &shared_strings, scratch);
                BinarySegmentWriteInt32(arraydata2_seg, fileForScanner.m_FilenameHash);
            }
            BufferDestroy(&scannerIndexWithListOfFiles.m_FilesToScan, heap);
        }

        BinarySegmentWriteInt32(dependenciesWithScannersArray_seg, m_PerNodeWorkerData.recursive_dependencies_with_scanners.m_Size);
        BinarySegmentWritePointer(dependenciesWithScannersArray_seg, BinarySegmentPosition(arraydata_seg));
        for(auto d: m_PerNodeWorkerData.recursive_dependencies_with_scanners)
            BinarySegmentWriteUint32(arraydata_seg, d);

        PerNodeWorkerDataDestroy(&m_PerNodeWorkerData);
    }


    void WriteLeafInputsAndScannerIndicesToFilesToScan()
    {
        DagRuntimeDataInit(&dagRuntimeData, dag, heap);

        //This function writes two arrays in parallel. we first write the headers for both, and make sure to
        //make sure to use two different payload segments:

        //Write LeafInputs array header
        BinarySegmentWriteUint32(main_seg, node_count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(leafnodearray_seg));

        BinarySegmentWriteUint32(main_seg, node_count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(cacheableDependenciesArray_seg));

        //Write m_ScannerIndices_To_Files_To_Scan header
        BinarySegmentWriteUint32(main_seg, node_count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(scannerindex_seg));

        BinarySegmentWriteUint32(main_seg, node_count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(dependenciesWithScannersArray_seg));

        for (int32_t nodeIndex = 0; nodeIndex < node_count; ++nodeIndex)
        {
            WriteLeafInputsAndScannerIndicesToFilesToScan_ForSingleNode(nodeIndex);
        }

        DagRuntimeDataDestroy(&dagRuntimeData);
    }

    void WriteLeafInputHashOffline()
    {
        BinarySegmentWriteUint32(main_seg, node_count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(data_seg));
        for (int32_t i = 0; i < node_count; ++i)
        {
            HashDigest hashResult = {};

            const Frozen::DagNode& node = dag->m_DagNodes[i];
            if (0 != (node.m_Flags & Frozen::DagNode::kFlagCacheableByLeafInputs))
            {
                char path[kMaxPathLength];
                snprintf(path, sizeof(path), "%s/offline-%d", dag->m_CacheSignatureDirectoryName.Get(), i);
                PathBuffer output;
                PathInit(&output, path);
                MakeDirectoriesForFile(stat_cache, output);

                FILE *sig = fopen(path, "w");
                if (sig == NULL)
                    CroakErrno("Failed opening offline signature ingredients for writing.");

                std::function<const int32_t*(int)> arrayAccess = [=](int index){return deps[index].begin();};
                std::function<size_t(int)> sizeAccess = [=](int index){return deps[index].m_Size;};
                hashResult = CalculateLeafInputHashOffline(dag, arrayAccess, sizeAccess, i, heap, sig);
                fclose(sig);
            }
            BinarySegmentWrite(data_seg, (const char *)&hashResult, sizeof(HashDigest));
        }
    }

    bool WriteStreams(const char* dagderived_filename)
    {
        BinarySegmentWriteUint32(main_seg, Frozen::DagDerived::MagicNumber);
        BinarySegmentWriteUint32(main_seg, node_count);
        WriteFrozenArrayOfDependenciesAndBackLinks();
        WriteLeafInputsAndScannerIndicesToFilesToScan();
        WriteLeafInputHashOffline();
        BinarySegmentWriteUint32(main_seg, Frozen::DagDerived::MagicNumber);
        return BinaryWriterFlush(writer, dagderived_filename);
    }
};

static void CompileDagDerivedWorkerInit(CompileDagDerivedWorker* data, const Frozen::Dag* dag, MemAllocHeap* heap, MemAllocLinear* scratch, StatCache *stat_cache)
{
    data->heap = heap;
    data->scratch = scratch;
    data->dag = dag;
    data->writer = &data->_writer;
    BinaryWriterInit(data->writer, heap);
    HashTableInit(&data->shared_strings, heap);
    data->main_seg = BinaryWriterAddSegment(data->writer);
    data->data_seg = BinaryWriterAddSegment(data->writer);
    data->arraydata_seg = BinaryWriterAddSegment(data->writer);
    data->arraydata2_seg = BinaryWriterAddSegment(data->writer);
    data->leafnodearray_seg = BinaryWriterAddSegment(data->writer);
    data->cacheableDependenciesArray_seg = BinaryWriterAddSegment(data->writer);
    data->dependenciesWithScannersArray_seg = BinaryWriterAddSegment(data->writer);
    data->scannerindex_seg = BinaryWriterAddSegment(data->writer);
    data->str_seg = BinaryWriterAddSegment(data->writer);
    data->node_count = dag->m_NodeCount;
    data->stat_cache = stat_cache;
}

static void CompileDagDerivedWorkerDestroy(CompileDagDerivedWorker* data)
{
    HashTableDestroy(&data->shared_strings);
    BinaryWriterDestroy(data->writer);

    for (size_t i = 0; i < data->node_count; ++i)
    {
        BufferDestroy(&data->links[i], data->heap);
        BufferDestroy(&data->deps[i], data->heap);
    }
    HeapFree(data->heap, data->links);
    HeapFree(data->heap, data->deps);
}

bool CompileDagDerived(const Frozen::Dag* dag, MemAllocHeap* heap, MemAllocLinear* scratch, StatCache *stat_cache, const char* dagderived_filename)
{
    CompileDagDerivedWorker worker;
    CompileDagDerivedWorkerInit(&worker,dag,heap,scratch,stat_cache);
    bool result = worker.WriteStreams(dagderived_filename);
    CompileDagDerivedWorkerDestroy(&worker);
    return result;
};
