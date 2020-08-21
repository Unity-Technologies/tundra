#include "Common.hpp"
#include "DagData.hpp"
#include "AllBuiltNodes.hpp"
#include "ScanData.hpp"
#include "DigestCache.hpp"
#include "MemoryMappedFile.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void PrintNodeIndex(int index, const Frozen::Dag* dag)
{
    if (dag)
        printf("  %d: %s\n", index, dag->m_DagNodes[index].m_Annotation.Get());
    else
        printf("  %d\n", index);
}

static void DumpDagDerived(const Frozen::DagDerived* data, const Frozen::Dag* dag)
{
    printf("magic number: 0x%08x\n", data->m_MagicNumber);
    int node_count = data->m_NodeCount;

    printf("node count: %u\n", node_count);
    for (int nodeIndex=0; nodeIndex<node_count;nodeIndex++)
    {
        const Frozen::DagNode* dagNode = nullptr;
        if (dag)
            dagNode = &dag->m_DagNodes[nodeIndex];
        printf("\n");
        if (dag)
            printf("node %d %s:\n", nodeIndex, dagNode->m_Annotation.Get());
        else
            printf("node %d:\n", nodeIndex);

        if (dag)
        {
            printf("  flags:");
            if (dagNode->m_Flags & Frozen::DagNode::kFlagCacheableByLeafInputs)
                printf("    kFlagCacheableByLeafInputs");
           if (dagNode->m_Flags & Frozen::DagNode::kFlagOverwriteOutputs)
                printf("    kFlagOverwriteOutputs");
        }

        auto PrintNodeArray = [=](const char* title, const FrozenArray<uint32_t>& array)
        {
            if (array.GetCount() == 0)
                return;

            printf("\n  %s: (%d)\n", title, array.GetCount());
            for (int32_t b : array)
            {
                if (dag)
                    printf("  %s %d: %s\n", title, b, dag->m_DagNodes[b].m_Annotation.Get());
                else
                    printf("  %s %d\n", title, b);
            }
            printf("\n");
        };

        auto PrintFileAndHashArray = [=](const char* title, const FrozenArray<FrozenFileAndHash>& array)
        {
            if (array.GetCount() == 0)
                return;

            printf("\n  %s: (%d)\n", title, array.GetCount());
            for (auto& b : array)
                printf("   %s %s\n", title, b.m_Filename.Get());
            printf("\n");
        };

        PrintNodeArray("backlinks", data->m_NodeBacklinks[nodeIndex]);
        PrintFileAndHashArray("leafInputs", data->LeafInputsFor(nodeIndex));
        PrintNodeArray("dependentNodesThatThemselvesAreLeafInputCacheable", data->DependentNodesThatThemselvesAreLeafInputCacheableFor(nodeIndex));
        PrintNodeArray("RecursiveDependenciesWithScanners", data->DependentNodesWithScannerFor(nodeIndex));


        const FrozenArray<FrozenArray<FrozenFileAndHash>>& scannersWithListsOfFiles = data->ScannersWithListOfFilesFor(nodeIndex);
        for (int scannerIndex=0; scannerIndex!=scannersWithListsOfFiles.GetCount();scannerIndex++)
        for (auto& scannerWithListOfFiles: scannersWithListsOfFiles)
        {
            printf("  ScannerIndex %d will run on the following files:\n", scannerIndex);
            for(auto& file: scannerWithListOfFiles)
                printf("    %s\n",file.m_Filename.Get());
        }

        char tmp[kDigestStringSize];
        DigestToString(tmp, data->LeafInputHashOfflineFor(nodeIndex));
        printf("  leafInputsHash_OffLine: %s\n", tmp);
    }
}

static void DumpDag(const Frozen::Dag *data)
{
    int node_count = data->m_NodeCount;
    printf("magic number: 0x%08x\n", data->m_MagicNumber);
    printf("node count: %u\n", node_count);
    for (int i = 0; i < node_count; ++i)
    {
        printf("\n");
        printf("node %d:\n", i);
        char digest_str[kDigestStringSize];
        DigestToString(digest_str, data->m_NodeGuids[i]);

        const Frozen::DagNode &node = data->m_DagNodes[i];

        printf("  guid: %s\n", digest_str);
        printf("  flags:");
        if (node.m_Flags & Frozen::DagNode::kFlagPreciousOutputs)
            printf(" precious");
        if (node.m_Flags & Frozen::DagNode::kFlagOverwriteOutputs)
            printf(" overwrite");

        printf("\n  action: %s\n", node.m_Action.Get());
        printf("  annotation: %s\n", node.m_Annotation.Get());

        printf("  dependencies consumed during build:");
        for (int32_t dep : node.m_OriginalDependencies)
            printf(" %u", dep);
        printf("\n");

        printf("  dependencies consumed during usage:");
        for (int32_t dep : node.m_DependenciesConsumedDuringUsageOnly)
            printf(" %u", dep);
        printf("\n");

        printf("  inputs:\n");
        for (const FrozenFileAndHash &f : node.m_InputFiles)
            printf("    %s (0x%08x)\n", f.m_Filename.Get(), f.m_FilenameHash);

        printf("  outputs:\n");
        for (const FrozenFileAndHash &f : node.m_OutputFiles)
            printf("    %s (0x%08x)\n", f.m_Filename.Get(), f.m_FilenameHash);

        printf("  output directories:\n");
        for (const FrozenFileAndHash &f : node.m_OutputDirectories)
            printf("    %s (0x%08x)\n", f.m_Filename.Get(), f.m_FilenameHash);

        printf("  aux_outputs:\n");
        for (const FrozenFileAndHash &f : node.m_AuxOutputFiles)
            printf("    %s (0x%08x)\n", f.m_Filename.Get(), f.m_FilenameHash);

        printf("  environment:\n");
        for (const Frozen::EnvVarData &env : node.m_EnvVars)
        {
            printf("    %s = %s\n", env.m_Name.Get(), env.m_Value.Get());
        }

        printf("  scannerIndex: %d\n", node.m_ScannerIndex);
        if (node.m_ScannerIndex != -1)
        {
            auto s = data->m_Scanners[node.m_ScannerIndex].Get();
            printf("  scanner:\n");
            switch (s->m_ScannerType)
            {
            case Frozen::ScannerType::kCpp:
                printf("    type: cpp\n");
                break;
            case Frozen::ScannerType::kGeneric:
                printf("    type: generic\n");
                break;
            default:
                printf("    type: garbage!\n");
                break;
            }

            printf("    include paths:\n");
            for (const char *path : s->m_IncludePaths)
            {
                printf("      %s\n", path);
            }
            DigestToString(digest_str, s->m_ScannerGuid);
            printf("    scanner guid: %s\n", digest_str);

            if (Frozen::ScannerType::kGeneric == s->m_ScannerType)
            {
                const Frozen::GenericScannerData *gs = static_cast<const Frozen::GenericScannerData *>(s);
                printf("    flags:");
                if (Frozen::GenericScannerData::kFlagRequireWhitespace & gs->m_Flags)
                    printf(" RequireWhitespace");
                if (Frozen::GenericScannerData::kFlagUseSeparators & gs->m_Flags)
                    printf(" UseSeparators");
                if (Frozen::GenericScannerData::kFlagBareMeansSystem & gs->m_Flags)
                    printf(" BareMeansSystem");
                printf("\n");

                printf("    keywords:\n");
                for (const Frozen::KeywordData &kw : gs->m_Keywords)
                {
                    printf("      \"%s\" (%d bytes) follow: %s\n",
                           kw.m_String.Get(), kw.m_StringLength, kw.m_ShouldFollow ? "yes" : "no");
                }
            }
        }



        printf("\n");
    }

    printf("\nfile signatures:\n");
    for (const Frozen::DagFileSignature &sig : data->m_FileSignatures)
    {
        printf("file            : %s\n", sig.m_Path.Get());
        printf("timestamp       : %u\n", (unsigned int)sig.m_Timestamp);
    }
    printf("\nglob signatures:\n");
    for (const Frozen::DagGlobSignature &sig : data->m_GlobSignatures)
    {
        char digest_str[kDigestStringSize];
        DigestToString(digest_str, sig.m_Digest);
        printf("path            : %s\n", sig.m_Path.Get());
        printf("digest          : %s\n", digest_str);
    }

    for (const FrozenFileAndHash& directoryCausingImplicitDependencies: data->m_DirectoriesCausingImplicitDependencies)
        printf("directoryCausingImplicitDependencies: %s\n", directoryCausingImplicitDependencies.m_Filename.Get());

    printf("m_CacheSignatureDirectoryName : %s\n", data->m_CacheSignatureDirectoryName.Get());
    printf("m_StateFileName : %s\n", data->m_StateFileName.Get());
    printf("m_StateFileNameTmp : %s\n", data->m_StateFileNameTmp.Get());
    printf("m_ScanCacheFileName : %s\n", data->m_ScanCacheFileName.Get());
    printf("m_ScanCacheFileNameTmp : %s\n", data->m_ScanCacheFileNameTmp.Get());
    printf("m_DigestCacheFileName : %s\n", data->m_DigestCacheFileName.Get());
    printf("m_DigestCacheFileNameTmp : %s\n", data->m_DigestCacheFileNameTmp.Get());
    printf("m_BuildTitle : %s\n", data->m_BuildTitle.Get());

    printf("\nSHA-1 signatures enabled for extension hashes:\n");
    for (const uint32_t ext : data->m_ShaExtensionHashes)
    {
        printf("hash            : 0x%08x\n", ext);
    }

    printf("Magic number at end: 0x%08x\n", data->m_MagicNumberEnd);
}

static void DumpState(const Frozen::AllBuiltNodes *data)
{
    int node_count = data->m_NodeCount;
    printf("magic number: 0x%08x\n", data->m_MagicNumber);
    printf("node count: %u\n", node_count);
    for (int i = 0; i < node_count; ++i)
    {
        printf("node %d:\n", i);
        char digest_str[kDigestStringSize];

        const Frozen::BuiltNode &node = data->m_BuiltNodes[i];

        DigestToString(digest_str, data->m_NodeGuids[i]);
        printf("  guid: %s\n", digest_str);
        printf("  m_WasBuiltSuccessfully: %d\n", node.m_WasBuiltSuccessfully);
        DigestToString(digest_str, node.m_InputSignature);
        printf("  input_signature: %s\n", digest_str);
        printf("  outputs:\n");
        for (const FrozenFileAndHash& fileAndHash : node.m_OutputFiles)
            printf("    (0x%08x) %s\n", fileAndHash.m_FilenameHash, fileAndHash.m_Filename.Get());
        printf("  aux outputs:\n");
        for (const FrozenFileAndHash& fileAndHash : node.m_AuxOutputFiles)
            printf("    (0x%08x) %s\n", fileAndHash.m_FilenameHash, fileAndHash.m_Filename.Get());

        printf("  input files:\n");
        for (int i=0; i!=node.m_InputFiles.GetCount(); i++)
            printf("    %lld %s\n", node.m_InputFiles[i].m_Timestamp, node.m_InputFiles[i].m_Filename.Get());

        printf("  Implicit inputs:\n");
        for (int i=0; i!=node.m_ImplicitInputFiles.GetCount(); i++)
            printf("    %lld %s\n", node.m_ImplicitInputFiles[i].m_Timestamp, node.m_ImplicitInputFiles[i].m_Filename.Get());

        printf("\n");
    }
}

static void DumpScanCache(const Frozen::ScanData *data)
{
    int entry_count = data->m_EntryCount;
    printf("magic number: 0x%08x\n", data->m_MagicNumber);
    printf("entry count: %d\n", entry_count);
    for (int i = 0; i < entry_count; ++i)
    {
        printf("entry %d:\n", i);
        char digest_str[kDigestStringSize];

        const Frozen::ScanCacheEntry &entry = data->m_Data[i];

        DigestToString(digest_str, data->m_Keys[i]);
        printf("  guid: %s\n", digest_str);
        printf("  access time stamp: %llu\n", (long long unsigned int)data->m_AccessTimes[i]);
        printf("  file time stamp: %llu\n", (long long unsigned int)entry.m_FileTimestamp);
        printf("  included files:\n");
        for (const FrozenFileAndHash &path : entry.m_IncludedFiles)
            printf("    %s (0x%08x)\n", path.m_Filename.Get(), path.m_FilenameHash);
    }
}

static const char *FmtTime(uint64_t t)
{
    time_t tt = (time_t)t;
    static char time_buf[128];
    strftime(time_buf, sizeof time_buf, "%F %H:%M:%S", localtime(&tt));
    return time_buf;
}

static void DumpDigestCache(const Frozen::DigestCacheState *data)
{
    printf("record count: %d\n", data->m_Records.GetCount());
    for (const Frozen::DigestRecord &r : data->m_Records)
    {
        char digest_str[kDigestStringSize];
        printf("  filename     : %s\n", r.m_Filename.Get());
        printf("  filename hash: %08x\n", r.m_FilenameHash);
        DigestToString(digest_str, r.m_ContentDigest);
        printf("  digest SHA1  : %s\n", digest_str);
        printf("  access time  : %s\n", FmtTime(r.m_AccessTime));
        printf("  timestamp    : %s\n", FmtTime(r.m_Timestamp));
        printf("\n");
    }
}

const Frozen::Dag* dag_data = nullptr;
const Frozen::DagDerived* dag_derived_data = nullptr;

int main(int argc, char *argv[])
{
    for (int i=1; i!= argc; i++)
    {
        const char* fn = argv[i];
        MemoryMappedFile f;
        MmapFileInit(&f);
        MmapFileMap(&f, fn);

        if (MmapFileValid(&f))
        {
            const char *suffix = strrchr(fn, '.');

            if (0 == strcmp(suffix, ".dag"))
            {
                dag_data = (const Frozen::Dag *)f.m_Address;
                if (dag_data->m_MagicNumber != Frozen::Dag::MagicNumber)
                {
                    fprintf(stderr, "%s: bad magic number\n", fn);
                    exit(1);
                }
            }
            else if (0 == strcmp(suffix, ".dag_derived"))
            {
                dag_derived_data = (const Frozen::DagDerived *)f.m_Address;
                if (dag_derived_data->m_MagicNumber != Frozen::DagDerived::MagicNumber)
                {
                    fprintf(stderr, "%s: bad magic number\n", fn);
                    exit(1);
                }
            }
            else if (0 == strcmp(suffix, ".state"))
            {
                const Frozen::AllBuiltNodes *data = (const Frozen::AllBuiltNodes *)f.m_Address;
                if (data->m_MagicNumber == Frozen::AllBuiltNodes::MagicNumber)
                {
                    DumpState(data);
                }
                else
                {
                    fprintf(stderr, "%s: bad magic number\n", fn);
                }
            }
            else if (0 == strcmp(suffix, ".scancache"))
            {
                const Frozen::ScanData *data = (const Frozen::ScanData *)f.m_Address;
                if (data->m_MagicNumber == Frozen::ScanData::MagicNumber)
                {
                    DumpScanCache(data);
                }
                else
                {
                    fprintf(stderr, "%s: bad magic number\n", fn);
                }
            }
            else if (0 == strcmp(suffix, ".digestcache"))
            {
                const Frozen::DigestCacheState *data = (const Frozen::DigestCacheState *)f.m_Address;
                if (data->m_MagicNumber == Frozen::DigestCacheState::MagicNumber)
                {
                    DumpDigestCache(data);
                }
                else
                {
                    fprintf(stderr, "%s: bad magic number\n", fn);
                }
            }
            else
            {
                fprintf(stderr, "%s: unknown file type\n", fn);
            }
        }
        else
        {
            fprintf(stderr, "%s: couldn't mmap file\n", fn);
        }
    }

    if (dag_derived_data != nullptr)
    {
        DumpDagDerived(dag_derived_data, dag_data);
        exit(0);
    }
    if (dag_data != nullptr)
    {
        DumpDag(dag_data);
    }


    return 0;
}
