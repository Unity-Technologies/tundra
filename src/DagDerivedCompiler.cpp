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
#include "LeafInputSignatureOffline.hpp"
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

    BinarySegment *arraydata_seg;
    BinarySegment *arraydata2_seg;

    BinarySegment *dependenciesArray_seg;
    BinarySegment *backlinksArray_seg;
    BinarySegment *nodeLeafInputsArray_seg;
    BinarySegment *dependentNodesThatThemselvesAreLeafInputCacheableArray_seg;
    BinarySegment *dependentNodesWithScannersArray_seg;
    BinarySegment *scannersWithListOfFilesArray_seg;
    BinarySegment *leafInputHashOfflineArray_seg;
    BinarySegment *str_seg;

    DagRuntimeData dagRuntimeData;
    const Frozen::Dag* dag;
    MemAllocHeap*  heap;
    MemAllocLinear* scratch;
    int node_count;
    StatCache *stat_cache;

    Buffer<int32_t> *combinedDependenciesBuffers;
    Buffer<int32_t> *backlinksBuffers;

    void AddUsedDependenciesOfDagNodeRecursive(const Frozen::DagNode* node, int i)
    {
        for(int dep : node->m_DependenciesConsumedDuringUsageOnly)
        {
            BufferAppendOne(&combinedDependenciesBuffers[i], heap, dep);
            AddUsedDependenciesOfDagNodeRecursive(dag->m_DagNodes + dep, i);
        }
    }

    static bool IsLeafInputCacheable(const Frozen::DagNode& dagNode)
    {
        return HasFlag(dagNode.m_Flags,Frozen::DagNode::kFlagCacheableByLeafInputs);
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
    };

    void PerNodeWorkerDataInit(PerNodeWorkerData* data)
    {
        BufferInit(&data->scannersWithListsOfFiles);
        BufferInit(&data->leafInputBuffer);
        HashSetInit(&data->m_AlreadyProcessedFiles, heap);
    }

    void PerNodeWorkerDataDestroy(PerNodeWorkerData* data)
    {
        BufferDestroy(&data->scannersWithListsOfFiles, heap);
        BufferDestroy(&data->leafInputBuffer, heap);
        HashSetDestroy(&data->m_AlreadyProcessedFiles);
    }

    PerNodeWorkerData m_PerNodeWorkerData;

    void ProcessDiscoverdInput(const FrozenFileAndHash& file, const Frozen::DagNode& childDagNode, int scannerIndex)
    {
        //maybe we already processed this one before?
        if (!HashSetInsertIfNotPresent(&m_PerNodeWorkerData.m_AlreadyProcessedFiles, file.m_FilenameHash, file.m_Filename.Get()))
        {
            //yeah we did, we're done here.
            return;
        }

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
                ProcessDiscoverdInput(f, *fileGeneratingNode, scannerIndex);

            return;
        }

        //ok, not a generated file, so this is a leaf input.

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

    void WriteIndexArray(BinarySegment* segment, Buffer<int32_t>& buffer)
    {
        BinarySegmentWriteInt32(segment, buffer.m_Size);
        BinarySegmentWritePointer(segment, BinarySegmentPosition(arraydata_seg));
        for(int index: buffer)
            BinarySegmentWriteInt32(arraydata_seg, index);
    }

    void FindAllDependenciesStoppingAtCacheableNodesFor(int dagNodeIndex, Buffer<int32_t>& resulting_dependencies, Buffer<int32_t>& resulting_dependencies_cacheable_themselves)
    {
        std::function<const int32_t*(int)> arrayAccess = [=](int index){return combinedDependenciesBuffers[index].begin();};
        std::function<size_t(int)> sizeAccess = [=](int index){return combinedDependenciesBuffers[index].m_Size;};

        std::function<bool(int,int)> filterAndCollectLeafInputCacheable = [&](int parentIndex, int childIndex)
        {
            if (IsLeafInputCacheable(dag->m_DagNodes[childIndex]))
            {
                if (std::find(resulting_dependencies_cacheable_themselves.begin(), resulting_dependencies_cacheable_themselves.end(), childIndex) == resulting_dependencies_cacheable_themselves.end())
                    BufferAppendOne(&resulting_dependencies_cacheable_themselves, heap, childIndex);
                return false;
            }

            return true;
        };

        FindDependentNodesFromRootIndex(heap, dag, arrayAccess, sizeAccess, filterAndCollectLeafInputCacheable, dagNodeIndex, resulting_dependencies);
    };

    void WriteIntoCacheableNodeDataArraysFor(int nodeIndex)
    {
        const Frozen::DagNode& node = dag->m_DagNodes[nodeIndex];
        if (!IsLeafInputCacheable(node))
        {
            BinarySegmentWriteInt32(nodeLeafInputsArray_seg, 0);
            BinarySegmentWriteNullPointer(nodeLeafInputsArray_seg);

            BinarySegmentWriteInt32(dependentNodesThatThemselvesAreLeafInputCacheableArray_seg, 0);
            BinarySegmentWriteNullPointer(dependentNodesThatThemselvesAreLeafInputCacheableArray_seg);

            BinarySegmentWriteInt32(scannersWithListOfFilesArray_seg, 0);
            BinarySegmentWriteNullPointer(scannersWithListOfFilesArray_seg);

            BinarySegmentWriteInt32(dependentNodesWithScannersArray_seg, 0);
            BinarySegmentWriteNullPointer(dependentNodesWithScannersArray_seg);

            HashDigest empty = {};
            BinarySegmentWriteHashDigest(dependentNodesWithScannersArray_seg, empty);
            return;
        }

        PerNodeWorkerDataInit(&m_PerNodeWorkerData);


        Buffer<int32_t> dependencies, dependenciesThatAreLeafInputCacheableThemselves;
        BufferInit(&dependenciesThatAreLeafInputCacheableThemselves);
        BufferInit(&dependencies);

        FindAllDependenciesStoppingAtCacheableNodesFor(nodeIndex, dependencies, dependenciesThatAreLeafInputCacheableThemselves);
        WriteIndexArray(dependentNodesThatThemselvesAreLeafInputCacheableArray_seg, dependenciesThatAreLeafInputCacheableThemselves);
        BufferDestroy(&dependenciesThatAreLeafInputCacheableThemselves, heap);

        for (const auto& ignoredInput: node.m_CachingInputIgnoreList)
            HashSetInsertIfNotPresent(&m_PerNodeWorkerData.m_AlreadyProcessedFiles, ignoredInput.m_FilenameHash, ignoredInput.m_Filename.Get());

        Buffer<int32_t> dependentNodesWithScanners;
        BufferInit(&dependentNodesWithScanners);

        for(int32_t dependencyDagIndex : dependencies)
        {
            const Frozen::DagNode& dependencyDagNode = dag->m_DagNodes[dependencyDagIndex];

            for (auto& inputFile: dependencyDagNode.m_InputFiles)
                ProcessDiscoverdInput(inputFile, dependencyDagNode, dependencyDagNode.m_ScannerIndex);
            for (auto& fileThatMightBeIncluded: dependencyDagNode.m_FilesThatMightBeIncluded)
                ProcessDiscoverdInput(fileThatMightBeIncluded, dependencyDagNode, dependencyDagNode.m_ScannerIndex);

            if (dependencyDagNode.m_ScannerIndex != -1)
                BufferAppendOne(&dependentNodesWithScanners, heap, dependencyDagNode.m_DagNodeIndex);
        }
        BufferDestroy(&dependencies, heap);

        WriteIndexArray(this->dependentNodesWithScannersArray_seg, dependentNodesWithScanners);
        BufferDestroy(&dependentNodesWithScanners, heap);



        auto& leafInputBuffer = m_PerNodeWorkerData.leafInputBuffer;
        SortBufferOfFileAndHash(leafInputBuffer);

        BinarySegmentWriteInt32(nodeLeafInputsArray_seg, leafInputBuffer.m_Size);
        BinarySegmentWritePointer(nodeLeafInputsArray_seg, BinarySegmentPosition(arraydata_seg));
        for(const FileAndHash& leafInput: leafInputBuffer)
        {
            WriteCommonStringPtr(arraydata_seg, str_seg, leafInput.m_Filename, &shared_strings, scratch);
            BinarySegmentWriteInt32(arraydata_seg, leafInput.m_FilenameHash);
        }



        BinarySegmentWriteInt32(scannersWithListOfFilesArray_seg, m_PerNodeWorkerData.scannersWithListsOfFiles.m_Size);
        BinarySegmentWritePointer(scannersWithListOfFilesArray_seg, BinarySegmentPosition(arraydata_seg));
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

        auto CalculateLeafInputHashOffline2 = [=](const Frozen::DagNode& node) -> HashDigest
        {
            char path[kMaxPathLength];
            snprintf(path, sizeof(path), "%s/offline-%d", dag->m_CacheSignatureDirectoryName.Get(), node.m_DagNodeIndex);
            PathBuffer output;
            PathInit(&output, path);
            MakeDirectoriesForFile(stat_cache, output);

            FILE *sig = fopen(path, "w");
            if (sig == NULL)
                CroakErrno("Failed opening offline signature ingredients for writing.");

            std::function<const int32_t*(int)> arrayAccess = [=](int index){return combinedDependenciesBuffers[index].begin();};
            std::function<size_t(int)> sizeAccess = [=](int index){return combinedDependenciesBuffers[index].m_Size;};
            return CalculateLeafInputHashOffline(dag, arrayAccess, sizeAccess, node.m_DagNodeIndex, heap, sig);
            fclose(sig);
        };

        BinarySegmentWriteHashDigest(leafInputHashOfflineArray_seg, CalculateLeafInputHashOffline2(node));

        PerNodeWorkerDataDestroy(&m_PerNodeWorkerData);
    }


    void WriteLeafInputHashOffline()
    {

    }

    bool WriteStreams(const char* dagderived_filename)
    {
        combinedDependenciesBuffers = HeapAllocateArrayZeroed<Buffer<int32_t>>(heap, node_count);
        for (int32_t i = 0; i < node_count; ++i)
        {
            for(int dep : dag->m_DagNodes[i].m_OriginalDependencies)
            {
                BufferAppendOne(&combinedDependenciesBuffers[i], heap, dep);
                AddUsedDependenciesOfDagNodeRecursive(dag->m_DagNodes + dep, i);
            }
        }

        backlinksBuffers = HeapAllocateArrayZeroed<Buffer<int32_t>>(heap, node_count);
        for (int32_t i = 0; i < node_count; ++i)
        {
            for(int dep : combinedDependenciesBuffers[i])
                BufferAppendOne(&backlinksBuffers[dep], heap, i);
        }




        auto WriteArrayOfIndices = [=](BinarySegment* segment, Buffer<int32_t>& indices)->void{
                BinarySegmentWriteInt32(segment, indices.m_Size);
                BinarySegmentWritePointer(segment, BinarySegmentPosition(arraydata_seg));
                for(int32_t dep : indices)
                    BinarySegmentWriteInt32(arraydata_seg, dep);
        };

        BinarySegmentWriteUint32(main_seg, Frozen::DagDerived::MagicNumber);
        BinarySegmentWriteUint32(main_seg, node_count);

        BinarySegmentWriteUint32(main_seg, node_count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(dependenciesArray_seg));

        BinarySegmentWriteUint32(main_seg, node_count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(backlinksArray_seg));

        BinarySegmentWriteUint32(main_seg, node_count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(nodeLeafInputsArray_seg));

        BinarySegmentWriteUint32(main_seg, node_count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(dependentNodesThatThemselvesAreLeafInputCacheableArray_seg));

        BinarySegmentWriteUint32(main_seg, node_count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(scannersWithListOfFilesArray_seg));

        BinarySegmentWriteUint32(main_seg, node_count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(dependentNodesWithScannersArray_seg));

        BinarySegmentWriteUint32(main_seg, node_count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(leafInputHashOfflineArray_seg));

        DagRuntimeDataInit(&dagRuntimeData, dag, heap);

        for (int32_t nodeIndex = 0; nodeIndex < node_count; ++nodeIndex)
        {
            WriteArrayOfIndices(dependenciesArray_seg, combinedDependenciesBuffers[nodeIndex]);
            WriteArrayOfIndices(backlinksArray_seg, backlinksBuffers[nodeIndex]);

            const Frozen::DagNode& dagNode = dag->m_DagNodes[nodeIndex];

            WriteIntoCacheableNodeDataArraysFor(nodeIndex);

        }

        DagRuntimeDataDestroy(&dagRuntimeData);

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

    data->dependenciesArray_seg = BinaryWriterAddSegment(data->writer);
    data->backlinksArray_seg = BinaryWriterAddSegment(data->writer);
    data->arraydata_seg = BinaryWriterAddSegment(data->writer);
    data->arraydata2_seg = BinaryWriterAddSegment(data->writer);
    data->nodeLeafInputsArray_seg = BinaryWriterAddSegment(data->writer);
    data->dependentNodesThatThemselvesAreLeafInputCacheableArray_seg = BinaryWriterAddSegment(data->writer);
    data->dependentNodesWithScannersArray_seg = BinaryWriterAddSegment(data->writer);
    data->scannersWithListOfFilesArray_seg = BinaryWriterAddSegment(data->writer);
    data->leafInputHashOfflineArray_seg = BinaryWriterAddSegment(data->writer);
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
        BufferDestroy(&data->backlinksBuffers[i], data->heap);
        BufferDestroy(&data->combinedDependenciesBuffers[i], data->heap);
    }
    HeapFree(data->heap, data->backlinksBuffers);
    HeapFree(data->heap, data->combinedDependenciesBuffers);
}

bool CompileDagDerived(const Frozen::Dag* dag, MemAllocHeap* heap, MemAllocLinear* scratch, StatCache *stat_cache, const char* dagderived_filename)
{
    CompileDagDerivedWorker worker;
    CompileDagDerivedWorkerInit(&worker,dag,heap,scratch,stat_cache);
    bool result = worker.WriteStreams(dagderived_filename);
    CompileDagDerivedWorkerDestroy(&worker);
    return result;
};
