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

namespace NodeStateFlags
{
static const uint16_t kQueued = 1 << 0;
static const uint16_t kActive = 1 << 1;
} // namespace NodeStateFlags

namespace Frozen
{
    struct DagNode;
    struct BuiltNode;
}

struct RuntimeNode
{
    uint16_t m_Flags;

#if ENABLED(CHECKED_BUILD)
    const char *m_DebugAnnotation;
#endif
    const Frozen::DagNode *m_DagNode;
    const Frozen::BuiltNode *m_MmapState;

    NodeBuildResult::Enum m_BuildResult;
    bool m_Finished;
    HashDigest m_InputSignature;
};

inline bool NodeStateIsQueued(const RuntimeNode *state)
{
    return 0 != (state->m_Flags & NodeStateFlags::kQueued);
}

inline void NodeStateFlagQueued(RuntimeNode *state)
{
    state->m_Flags |= NodeStateFlags::kQueued;
}

inline void NodeStateFlagUnqueued(RuntimeNode *state)
{
    state->m_Flags &= ~NodeStateFlags::kQueued;
}

inline bool NodeStateIsActive(const RuntimeNode *state)
{
    return 0 != (state->m_Flags & NodeStateFlags::kActive);
}

inline void NodeStateFlagActive(RuntimeNode *state)
{
    state->m_Flags |= NodeStateFlags::kActive;
}

inline void NodeStateFlagInactive(RuntimeNode *state)
{
    state->m_Flags &= ~NodeStateFlags::kActive;
}
