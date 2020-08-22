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
#include <time.h>
#if !TUNDRA_WIN32
#include <unistd.h>
#endif

static bool FilterOutGeneratedIncludedFiles(void* _userData, const char* includingFile, const char* includedFile)
{
    //return true if we want to process the file, false if we want it to be skipped
    return !IsFileGenerated((DagRuntimeData*)_userData, Djb2HashPath(includedFile), includedFile);
}

static HashDigest CalculateLeafInputSignatureRuntime_Impl(
    const int32_t* dagNodeIndexToRuntimeNodeIndex_Table,
    RuntimeNode* runtimeNodesArray,
    const Frozen::Dag* dag,
    const Frozen::DagDerived* dagDerived,
    const DagRuntimeData *dagRuntime,
    const Frozen::DagNode* dagNode,
    RuntimeNode* runtimeNode,
    MemAllocHeap* heap,
    MemAllocLinear* scratch,
    int profilerThreadId,
    StatCache* stat_cache,
    DigestCache* digest_cache,
    ScanCache* scan_cache,
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

    for (auto& dependentNodeThatIsCacheableItself: dagDerived->DependentNodesThatThemselvesAreLeafInputCacheableFor(dagNode->m_DagNodeIndex))
    {
        int childRuntimeNodeIndex = dagNodeIndexToRuntimeNodeIndex_Table[dependentNodeThatIsCacheableItself];
        auto& childRuntimeNode = runtimeNodesArray[childRuntimeNodeIndex];
        const auto& childDagNode = dag->m_DagNodes[dependentNodeThatIsCacheableItself];

        if (childRuntimeNode.m_CurrentLeafInputSignature.m_Words64[0] == 0)
        {
            //this is racy, but it should be okay(tm). It's possible the leaf input signature isn't stored yet, but another thread is already calculating it.
            //in that case we calculate it too, and both threads will just store the same value.
            childRuntimeNode.m_CurrentLeafInputSignature = CalculateLeafInputSignatureRuntime_Impl(dagNodeIndexToRuntimeNodeIndex_Table, runtimeNodesArray, dag, dagDerived, dagRuntime, &childDagNode, &childRuntimeNode, heap, scratch, profilerThreadId, stat_cache, digest_cache, scan_cache, nullptr);
        }

        if (ingredient_stream)
        {
            char childLeafInputSignatureStr[kDigestStringSize];
            DigestToString(childLeafInputSignatureStr, childRuntimeNode.m_CurrentLeafInputSignature);
            fprintf(ingredient_stream, "cacheabledependentnode: %s %s\n", childLeafInputSignatureStr, childDagNode.m_Annotation.Get());
        }
        HashUpdate(&hashState, &childRuntimeNode.m_CurrentLeafInputSignature, sizeof(HashDigest));
    }

    //starting the profiler scope late here, because we do not support nested profiler scope, and at the top of this function we recurse.
    ProfilerScope profiler_scope("ComputeLeafInputSignature", profilerThreadId, dagNode->m_Annotation);

    HashDigest offlinePart = dagDerived->LeafInputHashOfflineFor(dagNode->m_DagNodeIndex);
    HashUpdate(&hashState, &offlinePart, sizeof(HashDigest));

    const FrozenArray<FrozenFileAndHash>& leafInputs = dagDerived->LeafInputsFor(dagNode->m_DagNodeIndex);

    HashSet<kFlagPathStrings> localExplicitLeafInputs;
    HashSet<kFlagPathStrings> localImplicitLeafInputs;

    auto& explicitLeafInputs = runtimeNode == nullptr ? localExplicitLeafInputs : runtimeNode->m_ExplicitLeafInputs;
    auto& implicitLeafInputs = runtimeNode == nullptr ? localImplicitLeafInputs : runtimeNode->m_ImplicitLeafInputs;

    HashSetInit(&explicitLeafInputs, heap);
    HashSetInit(&implicitLeafInputs, heap);
    for (auto& leafInput: leafInputs)
        HashSetInsert(&explicitLeafInputs, leafInput.m_FilenameHash, leafInput.m_Filename.Get());

    ScanInput scanInput;
    scanInput.m_SafeToScanBeforeDependenciesAreProduced = true;
    scanInput.m_ScanCache = scan_cache;
    scanInput.m_ScratchAlloc = scratch;
    scanInput.m_ScratchHeap = heap;

    MemAllocLinear scanResults;
    LinearAllocInit(&scanResults, heap, MB(4), "ComputeLeafInputSignature");

    IncludeFilterCallback ignoreCallback;
    ignoreCallback.userData = (void*)dagRuntime;
    ignoreCallback.callback = FilterOutGeneratedIncludedFiles;

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

    HashDigest result;
    HashFinalize(&hashState, &result);

    if (runtimeNode == nullptr)
    {
        HashSetDestroy(&localExplicitLeafInputs);
        HashSetDestroy(&localImplicitLeafInputs);
    }

    LinearAllocDestroy(&scanResults);

    return result;
}



HashDigest CalculateLeafInputSignatureRuntime(BuildQueue* queue, ThreadState* thread_state, RuntimeNode* node)
{
    char path[kMaxPathLength];
    snprintf(path, sizeof(path), "%s/tmp-%d", queue->m_Config.m_Dag->m_CacheSignatureDirectoryName.Get(), node->m_DagNodeIndex);

    char offlinePath[kMaxPathLength];
    snprintf(offlinePath, sizeof(offlinePath), "%s/offline-%d", queue->m_Config.m_Dag->m_CacheSignatureDirectoryName.Get(), node->m_DagNodeIndex);

    FILE *sig = fopen(path, "w");
    FILE *sigoffline = fopen(offlinePath, "r");

    if (sig == NULL)
        CroakErrno("Failed opening signature ingredients for writing.");

#if TUNDRA_WIN32
    const char* username = getenv("USERNAME");
    if (username != nullptr)
        fprintf(sig, "Signature generated by %s\n", username);
#else
    char hostname[1024];
    const char* username = getenv("USER");
    if (gethostname(hostname, sizeof(hostname)) == 0 && username != nullptr)
        fprintf(sig, "Signature generated by %s@%s\n", username, hostname);
#endif

    time_t rawtime;
    struct tm *info;
    time( &rawtime );
    info = localtime( &rawtime );
    fprintf(sig, "Current local time and date: %s", asctime(info));
    fprintf(sig, "Annotation %s\n\n", node->m_DagNode->m_Annotation.Get());

    if (sigoffline != nullptr)
    {
        fprintf(sig, "Cold hash ingredients (anything below this line is hashed and must not change to get a cache hit):\n");

        char            buffer[1024];
        size_t          n;

        while ((n = fread(buffer, sizeof(char), sizeof(buffer), sigoffline)) > 0)
        {
            if (fwrite(buffer, sizeof(char), n, sig) != n)
                CroakErrno("Failed writing cold signature ingredients.");
        }
        fclose(sigoffline);
    }

    fprintf(sig, "Hot hash ingredients:\n");
    HashDigest res = CalculateLeafInputSignatureRuntime_Impl(
            queue->m_Config.m_DagNodeIndexToRuntimeNodeIndex_Table,
            queue->m_Config.m_RuntimeNodes,
            queue->m_Config.m_Dag,
            queue->m_Config.m_DagDerived,
            &queue->m_Config.m_DagRuntimeData,
            node->m_DagNode,
            node,
            thread_state->m_Queue->m_Config.m_Heap,
            &thread_state->m_ScratchAlloc,
            thread_state->m_ProfilerThreadId,
            queue->m_Config.m_StatCache,
            queue->m_Config.m_DigestCache,
            queue->m_Config.m_ScanCache,
            sig);
    fclose(sig);

    char digestString[kDigestStringSize];
    DigestToString(digestString, res);

    char new_path[kMaxPathLength];
    snprintf(new_path, sizeof(path), "%s/%s", queue->m_Config.m_Dag->m_CacheSignatureDirectoryName.Get(), digestString);
    if (!RenameFile(path, new_path))
        CroakErrno("Failed moving signature ingredients file.");

    return res;
}

void PrintLeafInputSignature(Driver* driver, const char **argv, int argc)
{
    Buffer<int32_t> requestedNodes;
    BufferInit(&requestedNodes);
    const Frozen::Dag* dag = driver->m_DagData;

    DriverSelectNodes(dag, argv, argc, &requestedNodes, &driver->m_Heap);
    if (requestedNodes.m_Size == 0)
        Croak("Cannot find requested target");
    if (requestedNodes.m_Size > 1)
        Croak("You can only print the leaf input signature for a single node, but %d are requested", requestedNodes.m_Size);

    int32_t requestedNode = requestedNodes[0];
    const Frozen::DagNode& dagNode = dag->m_DagNodes[requestedNode];

    if (0 == (dagNode.m_Flags & Frozen::DagNode::kFlagCacheableByLeafInputs))
    {
        Croak("Requested node %s is not cacheable by leaf inputs\n", dagNode.m_Annotation.Get());
    }

    printf("OffLine ingredients to the leaf input hash\n");
    std::function<const int32_t*(int)> funcToGetDependenciesForNode = [=](int index){return driver->m_DagDerivedData->m_Dependencies[index].GetArray();};
    std::function<size_t(int)> funcToGetDependenciesCountForNode = [=](int index){return driver->m_DagDerivedData->m_Dependencies[index].GetCount();};
    CalculateLeafInputHashOffline(dag, funcToGetDependenciesForNode, funcToGetDependenciesCountForNode, requestedNode, &driver->m_Heap, stdout);

    printf("\n\n\nRuntime ingredients to the leaf input hash\n");
    MemAllocLinear scratch;
    LinearAllocInit(&scratch, &driver->m_Heap, MB(16), "PrintLeafInputSignature");
    DagRuntimeData runtimeData;
    DagRuntimeDataInit(&runtimeData, driver->m_DagData, &driver->m_Heap);

    CalculateLeafInputSignatureRuntime_Impl(
        driver->m_DagNodeIndexToRuntimeNodeIndex_Table.begin(),
        driver->m_RuntimeNodes.begin(),
        driver->m_DagData,
        driver->m_DagDerivedData,
        &runtimeData,
        &dagNode,
        nullptr,
        &driver->m_Heap,
        &scratch,
        0,
        &driver->m_StatCache,
        &driver->m_DigestCache,
        &driver->m_ScanCache,
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

            if (HashSetLookup(&node->m_ExplicitLeafInputs, hash,filename))
                return true;
            if (HashSetLookup(&node->m_ImplicitLeafInputs, hash,filename))
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
