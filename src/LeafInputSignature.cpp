#include "LeafInputSignature.hpp"
#include "Hash.hpp"
#include "DagData.hpp"
#include "RuntimeNode.hpp"
#include "BuildQueue.hpp"
#include "Profiler.hpp"
#include "FileSign.hpp"
#include "Scanner.hpp"
#include "Driver.hpp"

HashDigest ComputeLeafInputSignature(const Frozen::Dag* dag, const Frozen::DagDerived* dagDerived, const Frozen::DagNode* dagNode, MemAllocHeap* heap, MemAllocLinear* scratch, int profilerThreadId, StatCache* stat_cache, DigestCache* digest_cache, ScanCache* scan_cache, FILE* ingredient_stream)
{
    ProfilerScope profiler_scope("ComputeLeafInputSignature", profilerThreadId, dagNode->m_Annotation);

    HashState hashState;
    HashInit(&hashState);

    const FrozenArray<FrozenFileAndHash>& leafInputs = dagDerived->m_NodeLeafInputs[dagNode->m_DagNodeIndex];

    HashSet<kFlagPathStrings> explicitLeafInputs, implicitLeafInputs;
    HashSetInit(&explicitLeafInputs, heap);
    HashSetInit(&implicitLeafInputs, heap);
    for (auto& leafInput: leafInputs)
        HashSetInsert(&explicitLeafInputs, leafInput.m_FilenameHash, leafInput.m_Filename.Get());

    ScanInput scanInput;
    scanInput.m_ScanCache = scan_cache;
    scanInput.m_ScratchAlloc = scratch;
    scanInput.m_ScratchHeap = heap;

    for (const Frozen::ScannerIndexWithListOfFiles& scannerIndexWithListOfFiles : dagDerived->m_Nodes_to_ScannersWithListsOfFiles[dagNode->m_DagNodeIndex])
    {
        scanInput.m_ScannerConfig = dag->m_Scanners[scannerIndexWithListOfFiles.m_ScannerIndex];
        for (const FrozenFileAndHash& file: scannerIndexWithListOfFiles.m_FilesToScan)
        {
            scanInput.m_FileName = file.m_Filename;
            ScanOutput scanOutput;
            if (ScanImplicitDeps(stat_cache, &scanInput, &scanOutput))
            {
                for(int i=0; i < scanOutput.m_IncludedFileCount; i++)
                {
                    const FileAndHash& includedFile = scanOutput.m_IncludedFiles[i];
                    if (HashSetLookup(&explicitLeafInputs, includedFile.m_FilenameHash, includedFile.m_Filename))
                        continue;
                    if (HashSetLookup(&implicitLeafInputs, includedFile.m_FilenameHash, includedFile.m_Filename))
                        continue;
                    HashSetInsert(&implicitLeafInputs, includedFile.m_FilenameHash, includedFile.m_Filename);
                }
            }
        }
    }

    auto addFileContentsToHash = [&](const char* filename, uint32_t filename_hash, const char* label)
    {
        HashDigest digest = ComputeFileSignatureSha1(stat_cache, digest_cache, filename, filename_hash);

        if (ingredient_stream)
        {
            char temp[kDigestStringSize];
            DigestToString(temp, digest);
            char buffer[1000];
            strncpy(buffer, filename, sizeof(buffer));
            char*p = buffer;
            for ( ; *p; ++p) *p = tolower(*p);
            fprintf(ingredient_stream, "%s: %s %s\n", label, temp, buffer);
        }
        HashAddPath(&hashState, filename);
        HashUpdate(&hashState, &digest, sizeof(digest));
    };

    HashSetWalk(&explicitLeafInputs, [&](uint32_t index, uint32_t hash, const char *filename) {
        addFileContentsToHash(filename, hash, "explicitLeafInput");
    });
    HashSetWalk(&implicitLeafInputs, [&](uint32_t index, uint32_t hash, const char *filename) {
        addFileContentsToHash(filename, hash, "implicitLeafInput");
    });

    HashDigest result;
    HashFinalize(&hashState, &result);
    return result;
}



void PrintLeafInputSignature(Driver* driver, const char **argv, int argc)
{
    Buffer<int32_t> requestedNodes;
    BufferInit(&requestedNodes);
    DriverSelectNodes(driver->m_DagData, argv, argc, &requestedNodes, &driver->m_Heap);
    if (requestedNodes.m_Size == 0)
        Croak("Cannot find requested target");
    if (requestedNodes.m_Size > 1)
        Croak("You can only print the leaf input signature for a single node, but %d are requested", requestedNodes.m_Size);

    int32_t requestedNode = requestedNodes[0];
    const Frozen::DagNode& dagNode = driver->m_DagData->m_DagNodes[requestedNode];
    const Frozen::DagNode& dummyDagNode = driver->m_DagData->m_DagNodes[123];

    if (0 == (dagNode.m_Flags & Frozen::DagNode::kFlagCacheableByLeafInputs))
    {
        Croak("Requested node %s is not cacheable by leaf inputs\n", dagNode.m_Annotation.Get());
    }
    MemAllocLinear scratch;
    LinearAllocInit(&scratch, &driver->m_Heap, MB(16), "PrintLeafInputSignature");
    ComputeLeafInputSignature(
        driver->m_DagData,
        driver->m_DagDerivedData,
        &dagNode,
        &driver->m_Heap,
        &scratch,
        0,
        &driver->m_StatCache,
        &driver->m_DigestCache,
        &driver->m_ScanCache,
        stdout);

    LinearAllocDestroy(&scratch);
}
