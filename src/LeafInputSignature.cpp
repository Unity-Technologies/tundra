#include "LeafInputSignature.hpp"
#include "Hash.hpp"
#include "DagData.hpp"
#include "RuntimeNode.hpp"
#include "BuildQueue.hpp"
#include "Profiler.hpp"
#include "FileSign.hpp"
#include "Scanner.hpp"
#include "Driver.hpp"

HashDigest ComputeLeafInputSignature(const Frozen::Dag* dag, const Frozen::DagDerived* dagDerived, const Frozen::DagNode* dagNode, MemAllocHeap* heap, MemAllocLinear* scratch, int profilerThreadId, StatCache* stat_cache, DigestCache* digest_cache, ScanCache* scan_cache)
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
        HashAndToLowerFileName(nullptr, &hashState, label, filename, digest);
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
