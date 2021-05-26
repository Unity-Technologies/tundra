#pragma once

#include "Common.hpp"
#include "BinaryData.hpp"

struct StatCache;
struct Driver;

namespace Frozen
{
#pragma pack(push, 4)
struct NodeInputFileData
{
    uint64_t m_Timestamp;
    uint32_t m_FilenameHash;
    FrozenString m_Filename;
};
#pragma pack(pop)

static_assert(sizeof(NodeInputFileData) == 16, "struct layout");

namespace BuiltNodeResult
{
    enum Enum
    {
        kRanSuccessfullyWithGuaranteedCorrectInputSignature,
        kRanSuccessfullyButInputSignatureMightBeIncorrect,
        kRanFailed,
    };
}

struct BuiltNode
{
    BuiltNodeResult::Enum m_Result;
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
    static const uint32_t MagicNumber = 0x53533dc3 ^ kTundraHashMagic;

    uint32_t m_MagicNumber;

    int32_t m_NodeCount;
    FrozenPtr<HashDigest> m_NodeGuids;
    FrozenPtr<BuiltNode> m_BuiltNodes;

    uint32_t m_MagicNumberEnd;
};
}

bool OutputFilesMissingFor(const Frozen::BuiltNode* node, StatCache *stat_cache);
bool SaveAllBuiltNodes(Driver *self);
bool NodeWasUsedByThisDagPreviously(const Frozen::BuiltNode *previously_built_node, uint32_t current_dag_identifier);
