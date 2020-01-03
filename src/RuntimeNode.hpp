#pragma once

#include "Common.hpp"
#include "Hash.hpp"

namespace NodeBuildResult
{
    enum Enum
    {
        kDidNotRun = 0,
        kUpToDate,
        kRanSuccesfully,
        kRanFailed,
        kRanSuccessButDependeesRequireFrontendRerun
    };
}

namespace RuntimeNodeFlags
{
    static const uint16_t kQueued = 1 << 0;
    static const uint16_t kActive = 1 << 1;
}

namespace Frozen
{
    struct DagNode;
    struct BuiltNode;
}

struct SinglyLinkedPathList;

struct RuntimeNode
{
    uint16_t m_Flags;

#if ENABLED(CHECKED_BUILD)
    const char *m_DebugAnnotation;
#endif
    const Frozen::DagNode *m_DagNode;
    const Frozen::BuiltNode *m_BuiltNode;

    NodeBuildResult::Enum m_BuildResult;
    bool m_Finished;
    HashDigest m_InputSignature;

    SinglyLinkedPathList* m_DynamicallyDiscoveredOutputFiles;
};

inline bool RuntimeNodeIsQueued(const RuntimeNode *runtime_node)
{
    return 0 != (runtime_node->m_Flags & RuntimeNodeFlags::kQueued);
}

inline void RuntimeNodeFlagQueued(RuntimeNode *runtime_node)
{
    runtime_node->m_Flags |= RuntimeNodeFlags::kQueued;
}

inline void RuntimeNodeFlagUnqueued(RuntimeNode *runtime_node)
{
    runtime_node->m_Flags &= ~RuntimeNodeFlags::kQueued;
}

inline bool RuntimeNodeIsActive(const RuntimeNode *runtime_node)
{
    return 0 != (runtime_node->m_Flags & RuntimeNodeFlags::kActive);
}

inline void RuntimeNodeFlagActive(RuntimeNode *runtime_node)
{
    runtime_node->m_Flags |= RuntimeNodeFlags::kActive;
}

inline void RuntimeNodeFlagInactive(RuntimeNode *runtime_node)
{
    runtime_node->m_Flags &= ~RuntimeNodeFlags::kActive;
}
