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
    BinarySegment *leafInputsArray_seg;
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
            BufferAppendOneIfNotPresent(&combinedDependenciesBuffers[i], heap, dep);
            AddUsedDependenciesOfDagNodeRecursive(dag->m_DagNodes + dep, i);
        }
    }

    static bool IsLeafInputCacheable(const Frozen::DagNode& dagNode)
    {
        return HasFlag(dagNode.m_Flags,Frozen::DagNode::kFlagCacheableByLeafInputs);
    }

    void WriteIndexArray(BinarySegment* segment, Buffer<int32_t>& buffer)
    {
        BinarySegmentWriteInt32(segment, buffer.m_Size);
        BinarySegmentWritePointer(segment, BinarySegmentPosition(arraydata_seg));
        for(int index: buffer)
            BinarySegmentWriteInt32(arraydata_seg, index);
    }

    void WriteFileAndHashArray(BinarySegment* segment, Buffer<FileAndHash>& fileAndHashes)
    {
        BinarySegmentWriteInt32(segment, fileAndHashes.m_Size);
        BinarySegmentWritePointer(segment, BinarySegmentPosition(arraydata_seg));
        for(const FileAndHash& fileAndHash: fileAndHashes)
        {
            WriteCommonStringPtr(arraydata_seg, str_seg, fileAndHash.m_Filename, &shared_strings, scratch);
            BinarySegmentWriteInt32(arraydata_seg, fileAndHash.m_FilenameHash);
        }
    }

    void WriteSortedPathsHashSetAsFrozenFileAndHash(BinarySegment* segment, HashSet<kFlagPathStrings>& paths)
    {
        Buffer<FileAndHash> buffer;
        BufferInitWithCapacity(&buffer, heap, paths.m_RecordCount);
        HashSetWalk(&paths, [&](uint32_t index, uint32_t hash, const char* path) {
            FileAndHash fileAndHash;
            fileAndHash.m_Filename = path;
            fileAndHash.m_FilenameHash = hash;

            BufferAppendOne(&buffer, heap, fileAndHash);
        });

        SortBufferOfFileAndHash(buffer);
        WriteFileAndHashArray(segment, buffer);
        BufferDestroy(&buffer, heap);
    };


    void FindAllDependenciesAndSelfStoppingAtCacheableNodesFor(int dagNodeIndex, Buffer<int32_t>& resulting_dependencies, Buffer<int32_t>& resulting_dependencies_cacheable_themselves)
    {
        std::function<const int32_t*(int)> funcToGetDependenciesForNode = [=](int index){return combinedDependenciesBuffers[index].begin();};
        std::function<size_t(int)> funcToGetDependenciesCountForNode = [=](int index){return combinedDependenciesBuffers[index].m_Size;};

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

        FindDependentNodesFromRootIndex(heap, dag, funcToGetDependenciesForNode, funcToGetDependenciesCountForNode, filterAndCollectLeafInputCacheable, dagNodeIndex, resulting_dependencies);
        BufferAppendOne(&resulting_dependencies, heap, dagNodeIndex);
    };


    void CollectNonGeneratedFilesBeingOperatedOnByScanner(const Frozen::DagNode& dagNode, HashSet<kFlagPathStrings>& result, const FrozenArray<FrozenFileAndHash>& files)
    {
        for(auto& file: files)
        {
            const Frozen::DagNode* generatingNode;
            if (!FindDagNodeForFile(&this->dagRuntimeData, file.m_FilenameHash, file.m_Filename, &generatingNode))
            {
                HashSetInsertIfNotPresent(&result, file.m_FilenameHash, file.m_Filename.Get());
                continue;
            }

            if (generatingNode != nullptr)
                CollectNonGeneratedFilesBeingOperatedOnByScanner(*generatingNode, result, generatingNode->m_FilesThatMightBeIncluded);
        }
    }

    void CollectNonGeneratedFilesBeingOperatedOnByScanner(const Frozen::DagNode& dagNode, HashSet<kFlagPathStrings>& result)
    {
        CollectNonGeneratedFilesBeingOperatedOnByScanner(dagNode, result, dagNode.m_InputFiles);
    };

    void WriteIntoCacheableNodeDataArraysFor(int nodeIndex)
    {
        const Frozen::DagNode& node = dag->m_DagNodes[nodeIndex];
        if (!IsLeafInputCacheable(node))
        {
            BinarySegmentWriteInt32(leafInputsArray_seg, 0);
            BinarySegmentWriteNullPointer(leafInputsArray_seg);

            BinarySegmentWriteInt32(dependentNodesThatThemselvesAreLeafInputCacheableArray_seg, 0);
            BinarySegmentWriteNullPointer(dependentNodesThatThemselvesAreLeafInputCacheableArray_seg);

            BinarySegmentWriteInt32(scannersWithListOfFilesArray_seg, 0);
            BinarySegmentWriteNullPointer(scannersWithListOfFilesArray_seg);

            BinarySegmentWriteInt32(dependentNodesWithScannersArray_seg, 0);
            BinarySegmentWriteNullPointer(dependentNodesWithScannersArray_seg);

            HashDigest empty = {};
            BinarySegmentWriteHashDigest(leafInputHashOfflineArray_seg, empty);
            return;
        }

        Buffer<int32_t> dependenciesAndSelf, dependenciesThatAreLeafInputCacheableThemselves;
        BufferInit(&dependenciesThatAreLeafInputCacheableThemselves);
        BufferInit(&dependenciesAndSelf);

        FindAllDependenciesAndSelfStoppingAtCacheableNodesFor(nodeIndex, dependenciesAndSelf, dependenciesThatAreLeafInputCacheableThemselves);
        WriteIndexArray(dependentNodesThatThemselvesAreLeafInputCacheableArray_seg, dependenciesThatAreLeafInputCacheableThemselves);
        BufferDestroy(&dependenciesThatAreLeafInputCacheableThemselves, heap);


        /* find all leaf input files*/
        {
            HashSet<kFlagPathStrings> leafInputFiles;
            HashSetInit(&leafInputFiles, heap);

            HashSet<kFlagPathStrings> ignoreSet;
            HashSetInit(&ignoreSet, heap);
            for(auto& ignore: node.m_CachingInputIgnoreList)
                HashSetInsertIfNotPresent(&ignoreSet, ignore.m_FilenameHash, ignore.m_Filename);

            Buffer<HashSet<kFlagPathStrings>> filesAffectedByScanners;
            BufferInit(&filesAffectedByScanners);
            BufferAlloc(&filesAffectedByScanners, heap, dag->m_Scanners.GetCount());
            for(auto& fileList: filesAffectedByScanners)
                HashSetInit(&fileList, heap);

            Buffer<int32_t> dependentNodesWithScanners;
            BufferInit(&dependentNodesWithScanners);

            auto AddToLeafInputsIfNotOnIgnoreList = [&leafInputFiles, ignoreSet](uint32_t filenameHash, const char* fileName) mutable
            {
                if (!HashSetLookup(&ignoreSet, filenameHash, fileName))
                    HashSetInsertIfNotPresent(&leafInputFiles, filenameHash, fileName);
            };

            auto AddToLeafInputsIfNonGeneratedAndNotOnIgnoreList = [=](const FrozenFileAndHash& file) mutable
            {
                const Frozen::DagNode* generatingNode;
                if (FindDagNodeForFile(&this->dagRuntimeData, file.m_FilenameHash, file.m_Filename, &generatingNode))
                    return;
                AddToLeafInputsIfNotOnIgnoreList(file.m_FilenameHash, file.m_Filename.Get());
            };

            for(int32_t dependencyDagIndex : dependenciesAndSelf)
            {
                const Frozen::DagNode& dependencyDagNode = dag->m_DagNodes[dependencyDagIndex];

                for (auto& file: dependencyDagNode.m_InputFiles)
                   AddToLeafInputsIfNonGeneratedAndNotOnIgnoreList(file);
                for (auto& file: dependencyDagNode.m_FilesThatMightBeIncluded)
                   AddToLeafInputsIfNonGeneratedAndNotOnIgnoreList(file);

                if (dependencyDagNode.m_ScannerIndex != -1)
                {
                    BufferAppendOne(&dependentNodesWithScanners, heap, dependencyDagNode.m_DagNodeIndex);
                    CollectNonGeneratedFilesBeingOperatedOnByScanner(dependencyDagNode, filesAffectedByScanners[dependencyDagNode.m_ScannerIndex]);
                }
            }

            WriteSortedPathsHashSetAsFrozenFileAndHash(this->leafInputsArray_seg, leafInputFiles);
            HashSetDestroy(&leafInputFiles);
            HashSetDestroy(&ignoreSet);

            BinarySegmentWriteInt32(scannersWithListOfFilesArray_seg, dag->m_Scanners.GetCount());
            BinarySegmentWritePointer(scannersWithListOfFilesArray_seg, BinarySegmentPosition(arraydata2_seg));
            for (int scannerIndex=0; scannerIndex != dag->m_Scanners.GetCount(); scannerIndex++)
                WriteSortedPathsHashSetAsFrozenFileAndHash(arraydata2_seg, filesAffectedByScanners[scannerIndex]);

            for(auto& fileList: filesAffectedByScanners)
                HashSetDestroy(&fileList);

            WriteIndexArray(dependentNodesWithScannersArray_seg, dependentNodesWithScanners);
            BufferDestroy(&dependentNodesWithScanners, heap);
        }
        BufferDestroy(&dependenciesAndSelf, heap);


        {  //write out offline leaf input hash parts
            char path[kMaxPathLength];
            snprintf(path, sizeof(path), "%s/offline-%d", dag->m_CacheSignatureDirectoryName.Get(), node.m_DagNodeIndex);
            PathBuffer output;
            PathInit(&output, path);
            MakeDirectoriesForFile(stat_cache, output);

            FILE *sig = fopen(path, "w");
            if (sig == NULL)
                CroakErrno("Failed opening offline signature ingredients for writing.");

            std::function<const int32_t*(int)> funcToGetDependenciesForNode = [=](int index){return combinedDependenciesBuffers[index].begin();};
            std::function<size_t(int)> funcToGetDependenciesCountForNode = [=](int index){return combinedDependenciesBuffers[index].m_Size;};
            BinarySegmentWriteHashDigest(leafInputHashOfflineArray_seg, CalculateLeafInputHashOffline(dag, funcToGetDependenciesForNode, funcToGetDependenciesCountForNode, node.m_DagNodeIndex, heap, sig));
            fclose(sig);
        }
    }



    bool WriteStreams(const char* dagderived_filename)
    {
        MemAllocLinearScope scratchScope(scratch);

        combinedDependenciesBuffers = HeapAllocateArrayZeroed<Buffer<int32_t>>(heap, node_count);
        for (int32_t i = 0; i < node_count; ++i)
        {
            for(int dep : dag->m_DagNodes[i].m_OriginalDependencies)
            {
                BufferAppendOneIfNotPresent(&combinedDependenciesBuffers[i], heap, dep);
                AddUsedDependenciesOfDagNodeRecursive(dag->m_DagNodes + dep, i);
            }
        }

        backlinksBuffers = HeapAllocateArrayZeroed<Buffer<int32_t>>(heap, node_count);
        for (int32_t i = 0; i < node_count; ++i)
        {
            for(int dep : combinedDependenciesBuffers[i])
                BufferAppendOneIfNotPresent(&backlinksBuffers[dep], heap, i);
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
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(leafInputsArray_seg));

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
    data->leafInputsArray_seg = BinaryWriterAddSegment(data->writer);
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
