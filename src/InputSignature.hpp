namespace t2
{
struct ThreadState;
struct BuildQueue;
struct NodeState;

bool CheckInputSignatureToSeeNodeNeedsExecuting(BuildQueue *queue, ThreadState *thread_state, NodeState *node);
} // namespace t2
