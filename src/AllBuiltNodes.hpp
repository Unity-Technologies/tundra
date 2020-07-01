#pragma once

#include "Common.hpp"
#include "BinaryData.hpp"

struct StatCache;

namespace Frozen
{
#pragma pack(push, 4)
struct NodeInputFileData
{
    uint64_t m_Timestamp;
    FrozenString m_Filename;
};
#pragma pack(pop)

static_assert(sizeof(NodeInputFileData) == 12, "struct layout");

struct BuiltNode
{
    uint32_t m_WasBuiltSuccessfully;
    HashDigest m_InputSignature;
    HashDigest m_LeafInputSignature;
    FrozenArray<FrozenFileAndHash> m_OutputFiles;
    FrozenArray<FrozenFileAndHash> m_AuxOutputFiles;
    FrozenString m_Action;
    FrozenArray<NodeInputFileData> m_InputFiles;
    FrozenArray<NodeInputFileData> m_ImplicitInputFiles;

    FrozenArray<uint32_t> m_DagsWeHaveSeenThisNodeInPreviously;
};

struct AllBuiltNodes
{
    static const uint32_t MagicNumber = 0xc1a24bc1 ^ kTundraHashMagic;

    uint32_t m_MagicNumber;

    int32_t m_NodeCount;
    FrozenPtr<HashDigest> m_NodeGuids;
    FrozenPtr<BuiltNode> m_BuiltNodes;

    uint32_t m_MagicNumberEnd;
};
}

bool OutputFilesMissingFor(const Frozen::BuiltNode* node, StatCache *stat_cache);
