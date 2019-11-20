#include "NodeState.hpp"

namespace t2
{
    struct BuildQueue;
    struct ThreadState;
    struct NodeState;
    struct Mutex;

    NodeBuildResult::Enum RunAction(BuildQueue* queue, ThreadState* thread_state, NodeState* node, Mutex* queue_lock);
}