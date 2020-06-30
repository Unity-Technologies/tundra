#pragma once

#include "Common.hpp"
#include "BinaryData.hpp"
#include "Hash.hpp"
#include "PathUtil.hpp"
#include "Buffer.hpp"

namespace Frozen
{
namespace ScannerType
{
enum Enum
{
    kCpp = 0,
    kGeneric = 1
};
}

struct ScannerData
{
    FrozenEnum<ScannerType::Enum, int32_t> m_ScannerType;
    FrozenArray<FrozenString> m_IncludePaths;
    HashDigest m_ScannerGuid;
};

struct KeywordData
{
    FrozenString m_String;
    int16_t m_StringLength;
    int8_t m_ShouldFollow;
    int8_t m_Padding;
};

struct GenericScannerData : ScannerData
{
    enum
    {
        kFlagRequireWhitespace = 1 << 0,
        kFlagUseSeparators = 1 << 1,
        kFlagBareMeansSystem = 1 << 2
    };

    uint32_t m_Flags;
    FrozenArray<KeywordData> m_Keywords;
};

struct NamedNodeData
{
    FrozenString m_Name;
    int32_t m_NodeIndex;
};

struct DagFileSignature
{
    FrozenString m_Path;
    uint8_t m_Padding[4];
    uint64_t m_Timestamp;
};
static_assert(offsetof(DagFileSignature, m_Timestamp) == 8, "struct layout");
static_assert(sizeof(DagFileSignature) == 16, "struct layout");

struct DagGlobSignature
{
    FrozenString m_Path;
    FrozenString m_Filter;
    HashDigest m_Digest;
    uint32_t m_Recurse;
};
static_assert(sizeof(HashDigest) + sizeof(FrozenString) + sizeof(FrozenString) + sizeof(uint32_t) == sizeof(DagGlobSignature), "struct layout");

struct EnvVarData
{
    FrozenString m_Name;
    FrozenString m_Value;
};

struct DagNode
{
    enum
    {
        // Set in m_Flags if it is safe to overwrite the output files in place.  If
        // this flag is not present, the build system will remove the output files
        // before running the action. This is useful to prevent tools that
        // sometimes misbehave in the presence of old output files. ar is a good
        // example.
        kFlagOverwriteOutputs = 1 << 0,

        // Keep output files even if the build fails. Useful mostly to retain files
        // for incremental linking.
        kFlagPreciousOutputs = 1 << 1,

        //if not set, we fail the build when a command prints anything unexpected to stdout or stderr
        kFlagAllowUnexpectedOutput = 1 << 3,

        kFlagIsWriteTextFileAction = 1 << 4,
        kFlagAllowUnwrittenOutputFiles = 1 << 5,
        kFlagBanContentDigestForInputs = 1 << 6
    };

    FrozenString m_Action;
    FrozenString m_Annotation;
    FrozenArray<int32_t> m_Dependencies;
    FrozenArray<FrozenFileAndHash> m_InputFiles;
    FrozenArray<FrozenFileAndHash> m_OutputFiles;
    FrozenArray<FrozenFileAndHash> m_OutputDirectories;
    FrozenArray<FrozenFileAndHash> m_AuxOutputFiles;
    FrozenArray<FrozenFileAndHash> m_FrontendResponseFiles;
    FrozenArray<FrozenString> m_AllowedOutputSubstrings;
    FrozenArray<EnvVarData> m_EnvVars;
    FrozenPtr<ScannerData> m_Scanner;
    FrozenArray<int32_t> m_SharedResources;
    FrozenArray<DagFileSignature> m_FileSignatures;
    FrozenArray<DagGlobSignature> m_GlobSignatures;
    uint32_t m_Flags;
    uint32_t m_OriginalIndex;
};

struct SharedResourceData
{
    FrozenString m_Annotation;
    FrozenString m_CreateAction;
    FrozenString m_DestroyAction;
    FrozenArray<EnvVarData> m_EnvVars;
};

struct Dag
{
    static const uint32_t MagicNumber = 0xfac92242 ^ kTundraHashMagic;

    uint32_t m_MagicNumber;

    uint32_t m_HashedIdentifier;

    int32_t m_NodeCount;
    FrozenPtr<HashDigest> m_NodeGuids;
    FrozenPtr<DagNode> m_DagNodes;

    FrozenArray<NamedNodeData> m_NamedNodes;
    FrozenArray<int32_t> m_DefaultNodes;

    FrozenArray<SharedResourceData> m_SharedResources;

    FrozenArray<DagFileSignature> m_FileSignatures;
    FrozenArray<DagGlobSignature> m_GlobSignatures;

    //we should remove this feature, and exluseively use the new .TargetDirectories that live on DagNode.
    FrozenArray<FrozenString> m_DirectoriesCausingImplicitDependencies;

    // Hashes of filename extensions to use SHA-1 digest signing instead of timestamp signing.
    FrozenArray<uint32_t> m_ShaExtensionHashes;

    int32_t m_DaysToKeepUnreferencedNodesAround;

    FrozenString m_StateFileName;
    FrozenString m_StateFileNameTmp;
    FrozenString m_ScanCacheFileName;
    FrozenString m_ScanCacheFileNameTmp;
    FrozenString m_DigestCacheFileName;
    FrozenString m_DigestCacheFileNameTmp;
    FrozenString m_BuildTitle;
    FrozenString m_StructuredLogFileName;

    uint32_t m_MagicNumberEnd;
};


//the *Derived dag data is data that is stored in a separate file, can be calculated from the original dag data.
//The reason it's in a separate file is that it's much faster to generate it from the binary dag than from the json dag.
struct DagNodeDerived
{
    FrozenArray<int32_t> m_BackLinks;
};
struct DagDerived
{
    static const uint32_t MagicNumber = 0x9dead123 ^ kTundraHashMagic;

    uint32_t m_MagicNumber;
    int32_t m_NodeCount;
    FrozenPtr<DagNodeDerived> m_NodesDerived;
    FrozenArray<FrozenString> m_AllOutputDirectories;
    uint32_t m_MagicNumberEnd;
};

}

void FindDependentNodesFromRootIndices(MemAllocHeap* heap, const Frozen::Dag* dag, int32_t* searchRootIndices, int32_t searchRootCount, Buffer<int32_t>& results);
