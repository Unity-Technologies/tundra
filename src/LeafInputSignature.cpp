#include "LeafInputSignature.hpp"
#include "LeafInputSignatureOffline.hpp"
#include "DagData.hpp"
#include "RuntimeNode.hpp"
#include "BuildQueue.hpp"
#include "Profiler.hpp"
#include "FileSign.hpp"
#include "Scanner.hpp"
#include "Driver.hpp"
#include "Exec.hpp"
#include "NodeResultPrinting.hpp"
#include "AllBuiltNodes.hpp"
#include "Atomic.hpp"
#include <time.h>
#if !TUNDRA_WIN32
#include <unistd.h>
#endif

static bool FilterOutGeneratedIncludedFiles(void* _userData, const char* includingFile, const char* includedFile)
{
    //return true if we want to process the file, false if we want it to be skipped
    return !IsFileGenerated((DagRuntimeData*)_userData, Djb2HashPath(includedFile), includedFile);
}

static void CalculateLeafInputSignatureRuntime_Impl(
    BuildQueue* buildQueue,
    const Frozen::DagNode* dagNode,
    RuntimeNode* runtimeNode,
    MemAllocHeap* heap,
    MemAllocLinear* scratch,
    int profilerThreadId,
    FILE* ingredient_stream)
{
    HashState hashState;
    HashInit(&hashState);

    const char* salt = getenv("BEE_CACHE_SALT");
    if (salt != nullptr)
    {
        HashAddString(&hashState, salt);
        if (ingredient_stream)
            fprintf(ingredient_stream, "BEE_CACHE_SALT: %s\n", salt);
    } else {
        if (ingredient_stream)
            fprintf(ingredient_stream, "BEE_CACHE_SALT: none\n");
    }

    auto& dagDerived = buildQueue->m_Config.m_DagDerived;
    auto& dagNodeIndexToRuntimeNodeIndex_Table = buildQueue->m_Config.m_DagNodeIndexToRuntimeNodeIndex_Table;
    auto& runtimeNodesArray = buildQueue->m_Config.m_RuntimeNodes;
    auto& dag = buildQueue->m_Config.m_Dag;

    for (auto& dependentNodeThatIsCacheableItself: dagDerived->DependentNodesThatThemselvesAreLeafInputCacheableFor(dagNode->m_DagNodeIndex))
    {
        int childRuntimeNodeIndex = dagNodeIndexToRuntimeNodeIndex_Table[dependentNodeThatIsCacheableItself];
        auto& childRuntimeNode = runtimeNodesArray[childRuntimeNodeIndex];
        const auto& childDagNode = dag->m_DagNodes[dependentNodeThatIsCacheableItself];

        if (childRuntimeNode.m_CurrentLeafInputSignature == nullptr)
        {
            CalculateLeafInputSignatureRuntime_Impl(buildQueue, &childDagNode, &childRuntimeNode, heap, scratch, profilerThreadId, nullptr);
            CHECK(childRuntimeNode.m_CurrentLeafInputSignature != nullptr);
        }

        if (ingredient_stream)
        {
            char childLeafInputSignatureStr[kDigestStringSize];
            DigestToString(childLeafInputSignatureStr, childRuntimeNode.m_CurrentLeafInputSignature->digest);
            fprintf(ingredient_stream, "cacheabledependentnode: %s %s\n", childLeafInputSignatureStr, childDagNode.m_Annotation.Get());
        }
        HashAddHashDigest(&hashState, childRuntimeNode.m_CurrentLeafInputSignature->digest);
    }

    //starting the profiler scope late here, because we do not support nested profiler scope, and at the top of this function we recurse.
    ProfilerScope profiler_scope("ComputeLeafInputSignature", profilerThreadId, dagNode->m_Annotation);

    HashDigest offlinePart = dagDerived->LeafInputHashOfflineFor(dagNode->m_DagNodeIndex);
    HashUpdate(&hashState, &offlinePart, sizeof(HashDigest));

    const FrozenArray<FrozenFileAndHash>& leafInputs = dagDerived->LeafInputsFor(dagNode->m_DagNodeIndex);

    auto result = (LeafInputSignatureData*)HeapAllocate(heap, sizeof(LeafInputSignatureData));
    auto& explicitLeafInputs = result->m_ExplicitLeafInputs;
    auto& implicitLeafInputs = result->m_ImplicitLeafInputs;

    HashSetInit(&explicitLeafInputs, heap);
    HashSetInit(&implicitLeafInputs, heap);
    for (auto& leafInput: leafInputs)
        HashSetInsert(&explicitLeafInputs, leafInput.m_FilenameHash, leafInput.m_Filename.Get());

    ScanInput scanInput;
    scanInput.m_SafeToScanBeforeDependenciesAreProduced = true;
    scanInput.m_ScanCache = buildQueue->m_Config.m_ScanCache;
    scanInput.m_ScratchAlloc = scratch;
    scanInput.m_ScratchHeap = heap;

    MemAllocLinear scanResults;
    LinearAllocInit(&scanResults, heap, MB(4), "ComputeLeafInputSignature");

    IncludeFilterCallback ignoreCallback;
    ignoreCallback.userData = (void*)&buildQueue->m_Config.m_DagRuntimeData;
    ignoreCallback.callback = FilterOutGeneratedIncludedFiles;
    auto& stat_cache = buildQueue->m_Config.m_StatCache;

    auto& filesAffectedByScanners = dagDerived->ScannersWithListOfFilesFor(dagNode->m_DagNodeIndex);
    for (int scannerIndex=0; scannerIndex != filesAffectedByScanners.GetCount(); scannerIndex++)
    {
        scanInput.m_ScannerConfig = dag->m_Scanners[scannerIndex];

        for (const FrozenFileAndHash& file: filesAffectedByScanners[scannerIndex])
        {
            MemAllocLinearScope allocScope(scratch);
            scanInput.m_FileName = file.m_Filename;

            ScanOutput scanOutput;
            if (ScanImplicitDeps(stat_cache, &scanInput, &scanOutput, &ignoreCallback))
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

    auto digest_cache = buildQueue->m_Config.m_DigestCache;

    auto addFileContentsToHash = [&](const char* filename, uint32_t filename_hash, const char* label)
    {
        HashDigest digest = ComputeFileSignatureSha1(stat_cache, digest_cache, filename, filename_hash);

        if (ingredient_stream)
        {
            char digestString[kDigestStringSize];
            DigestToString(digestString, digest);
            char buffer[kMaxPathLength];
            strncpy(buffer, filename, sizeof(buffer));
            char*p = buffer;
            for ( ; *p; ++p) *p = tolower(*p);
            fprintf(ingredient_stream, "%s: %s %s\n", label, digestString, buffer);
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

    HashFinalize(&hashState, &result->digest);

    if (runtimeNode != nullptr)
        if (AtomicCompareExchange((void**)&runtimeNode->m_CurrentLeafInputSignature, result, nullptr) != nullptr)
            DestroyLeafInputSignatureData(heap, result);

    LinearAllocDestroy(&scanResults);
}


void DestroyLeafInputSignatureData(MemAllocHeap *heap, LeafInputSignatureData *data)
{
    HashSetDestroy(&data->m_ExplicitLeafInputs);
    HashSetDestroy(&data->m_ImplicitLeafInputs);
    HeapFree(heap, data);
}

void CalculateLeafInputSignatureRuntime(BuildQueue* queue, ThreadState* thread_state, RuntimeNode* node, FILE* ingredient_stream)
{
    if (ingredient_stream)
    {
        time_t rawtime;
        struct tm *info;
        time( &rawtime );
        info = localtime( &rawtime );

        fprintf(ingredient_stream, "Current local time and date: %s", asctime(info));
        fprintf(ingredient_stream, "Annotation %s\n\n", node->m_DagNode->m_Annotation.Get());
        fprintf(ingredient_stream, "Cold hash ingredients (anything below this line is hashed and must not change to get a cache hit):\n");

#if TUNDRA_WIN32
        const char* username = getenv("USERNAME");
        if (username != nullptr)
            fprintf(ingredient_stream, "Signature generated by %s\n", username);
#else
        char hostname[1024];
        const char* username = getenv("USER");
        if (gethostname(hostname, sizeof(hostname)) == 0 && username != nullptr)
            fprintf(ingredient_stream, "Signature generated by %s@%s\n", username, hostname);
#endif
    }

    auto& dagDerived = queue->m_Config.m_DagDerived;
    std::function<const int32_t*(int)> funcToGetDependenciesForNode = [=](int index){return dagDerived->m_Dependencies[index].GetArray();};
    std::function<size_t(int)> funcToGetDependenciesCountForNode = [=](int index){return dagDerived->m_Dependencies[index].GetCount();};
    CalculateLeafInputHashOffline(queue->m_Config, node->m_DagNodeIndex, &thread_state->m_LocalHeap, ingredient_stream);

    if (ingredient_stream)
        fprintf(ingredient_stream, "Hot hash ingredients:\n");

   CalculateLeafInputSignatureRuntime_Impl(
            queue,
            node->m_DagNode,
            node,
            thread_state->m_Queue->m_Config.m_Heap,
            &thread_state->m_ScratchAlloc,
            thread_state->m_ProfilerThreadId,
            ingredient_stream);
}

static const Frozen::DagNode& FindRequestedNode(BuildQueue* queue)
{
    auto& nodes = queue->m_Config.m_RuntimeNodes;
    for (int i=0; i!=queue->m_Config.m_TotalRuntimeNodeCount;i++)
        if (RuntimeNodeIsExplicitlyRequested(nodes+i) && !RuntimeNodeIsExplicitlyRequestedThroughUseDependency(nodes+i))
            return *nodes[i].m_DagNode;

    Croak("Unable to find requested node for leaf input signature printing");
}

void PrintLeafInputSignature(BuildQueue* buildQueue)
{
    const Frozen::Dag* dag = buildQueue->m_Config.m_Dag;

    const Frozen::DagNode& dagNode = FindRequestedNode(buildQueue);

    if (0 == (dagNode.m_Flags & Frozen::DagNode::kFlagCacheableByLeafInputs))
    {
        Croak("Requested node %s is not cacheable by leaf inputs\n", dagNode.m_Annotation.Get());
    }

    printf("OffLine ingredients to the leaf input hash\n");
    CalculateLeafInputHashOffline(buildQueue->m_Config, dagNode.m_DagNodeIndex, buildQueue->m_Config.m_Heap, stdout);

    printf("\n\n\nRuntime ingredients to the leaf input hash\n");
    MemAllocLinear scratch;
    LinearAllocInit(&scratch, buildQueue->m_Config.m_Heap, MB(16), "PrintLeafInputSignature");
    DagRuntimeData runtimeData;
    DagRuntimeDataInit(&runtimeData, dag, buildQueue->m_Config.m_Heap);

    CalculateLeafInputSignatureRuntime_Impl(
        buildQueue,
        &dagNode,
        nullptr,
        buildQueue->m_Config.m_Heap,
        &scratch,
        0,
        stdout);

    DagRuntimeDataDestroy(&runtimeData);
    LinearAllocDestroy(&scratch);
}

struct HeaderValidationError
{
    RuntimeNode* runtimeNode;
    const char* included_file;
};

bool VerifyAllVersionedFilesIncludedByGeneratedHeaderFilesWereAlreadyPartOfTheLeafInputs(BuildQueue* queue, ThreadState* thread_state, RuntimeNode* node, const Frozen::DagDerived* dagDerived)
{
    HashSet<kFlagPathStrings> alreadyFound;
    HashSetInit(&alreadyFound, &thread_state->m_LocalHeap);
    Buffer<HeaderValidationError> illegalIncludesToReport;
    BufferInit(&illegalIncludesToReport);

    for(auto nodeWithScanner: dagDerived->DependentNodesWithScannerFor(node->m_DagNodeIndex))
    {
        int runtimeNodeIndex = queue->m_Config.m_DagNodeIndexToRuntimeNodeIndex_Table[nodeWithScanner];
        RuntimeNode* runtimeNodeWithScanner = &queue->m_Config.m_RuntimeNodes[runtimeNodeIndex];

        auto IsGeneratedOrIsLeafInput = [=](uint32_t hash, const char* filename) -> bool
        {
            const Frozen::DagNode* generatingNode;
            if (FindDagNodeForFile(&queue->m_Config.m_DagRuntimeData, hash, filename, &generatingNode))
                return true;

            if (HashSetLookup(&node->m_CurrentLeafInputSignature->m_ExplicitLeafInputs, hash,filename))
                return true;
            if (HashSetLookup(&node->m_CurrentLeafInputSignature->m_ImplicitLeafInputs, hash,filename))
                return true;
            return false;
        };

        switch (runtimeNodeWithScanner->m_BuildResult)
        {
            case NodeBuildResult::kUpToDate:
                for (auto& includedFile: runtimeNodeWithScanner->m_BuiltNode->m_ImplicitInputFiles)
                {
                    uint32_t pathHash = includedFile.m_FilenameHash;
                    const char* fileName = includedFile.m_Filename.Get();

                    if (!IsGeneratedOrIsLeafInput(pathHash, fileName))
                    {
                        if (HashSetInsertIfNotPresent(&alreadyFound, pathHash, fileName))
                        {
                            HeaderValidationError error;
                            error.runtimeNode = runtimeNodeWithScanner;
                            error.included_file = fileName;
                            BufferAppendOne(&illegalIncludesToReport, &thread_state->m_LocalHeap, error);
                        }
                    }
                }

                break;
            case NodeBuildResult::kRanSuccesfully:

                HashSetWalk(&runtimeNodeWithScanner->m_ImplicitInputs, [&](uint32_t, uint32_t hash, const char *filename) {
                    if (!IsGeneratedOrIsLeafInput(hash, filename))
                    {
                        HeaderValidationError error;
                        error.runtimeNode = runtimeNodeWithScanner;
                        error.included_file = filename;
                        BufferAppendOne(&illegalIncludesToReport, &thread_state->m_LocalHeap, error);
                    }
                });


                break;
            default:
                Croak("Unexpected build node result of dependent node while verifying headers");
        }
    }

    if (illegalIncludesToReport.m_Size > 0)
    {
        auto error = illegalIncludesToReport[0];

        ExecResult result = {0, false};
        result.m_ReturnCode = 1;

        InitOutputBuffer(&result.m_OutputBuffer, &thread_state->m_LocalHeap);

        {
            MemAllocLinearScope scope(&thread_state->m_ScratchAlloc);


            const char* formatString =
            "This node '%s' is marked as leaf input cacheable.\n"
            "%d implicit input files have been discovered that were not statically known ahead of time.\n"
            "For example:\n"
            "The dependency node '%s' ends up including '%s'.\n"
            "This usually only happens if a file is only included by generated files.\n"
            "You can fix this by either:\n"
            "- Making '%s' not be included by the generated files including it currently.\n"
            "- By ensuring that this build has a non-generated file that also includes '%s'.\n"
            "- Adding this headerfile to the .Sources property of your NativeProgram.\n";

            const int kStoragePerInclude = kMaxPathLength + 50;
            size_t errorStorageSize = strlen(formatString)*2 + illegalIncludesToReport.m_Size * kStoragePerInclude;
            char* errorStorage = LinearAllocateArray<char>(&thread_state->m_ScratchAlloc,errorStorageSize);

            char* endPtr = errorStorage + errorStorageSize;

            int written = snprintf(errorStorage,errorStorageSize, formatString,
                node->m_DagNode->m_Annotation.Get(),
                illegalIncludesToReport.m_Size,
                error.runtimeNode->m_DagNode->m_Annotation.Get(),
                error.included_file,
                error.included_file,
                error.included_file
            );
            char* cursor = errorStorage + written;

            auto appendToError = [&](const char* msg)
            {
                int remaining = endPtr - cursor;
                cursor += snprintf(cursor, remaining, "%s", msg);
            };

            appendToError("The full list of not statically known included files is:\n");
            for(auto& e: illegalIncludesToReport)
            {
                char tmp[kStoragePerInclude];
                snprintf(tmp, sizeof(tmp), "#include \"%s\"\n", e.included_file);
                char* c = tmp;
                while(*c != 0)
                {
                    if (*c == '\\')
                        *c = '/';
                    c++;
                }
                appendToError(tmp);
            }

            result.m_FrozenNodeData = node->m_DagNode;
            EmitOutputBytesToDestination(&result, errorStorage, strlen(errorStorage));

            MutexLock(&queue->m_Lock);
            PrintNodeResult(&result, node->m_DagNode, "Implicit input validation error", queue, thread_state, false, TimerGet(), ValidationResult::Pass, nullptr, true);
            MutexUnlock(&queue->m_Lock);
        }
        BufferDestroy(&illegalIncludesToReport, &thread_state->m_LocalHeap);
        ExecResultFreeMemory(&result);
        return false;
    }

    return true;
}
