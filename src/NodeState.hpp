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
    struct Node;
    struct NodeStateData;
}

struct NodeState
{
    uint16_t m_Flags;

#if ENABLED(CHECKED_BUILD)
    const char *m_DebugAnnotation;
#endif
    const Frozen::Node *m_MmapData;
    const Frozen::NodeStateData *m_MmapState;

    NodeBuildResult::Enum m_BuildResult;
    bool m_Finished;
    HashDigest m_InputSignature;
};

inline bool NodeStateIsQueued(const NodeState *state)
{
    return 0 != (state->m_Flags & NodeStateFlags::kQueued);
}

inline void NodeStateFlagQueued(NodeState *state)
{
    state->m_Flags |= NodeStateFlags::kQueued;
}

inline void NodeStateFlagUnqueued(NodeState *state)
{
    state->m_Flags &= ~NodeStateFlags::kQueued;
}

inline bool NodeStateIsActive(const NodeState *state)
{
    return 0 != (state->m_Flags & NodeStateFlags::kActive);
}

inline void NodeStateFlagActive(NodeState *state)
{
    state->m_Flags |= NodeStateFlags::kActive;
}

inline void NodeStateFlagInactive(NodeState *state)
{
    state->m_Flags &= ~NodeStateFlags::kActive;
}
