#pragma once

struct ThreadState;
struct BuildQueue;
struct RuntimeNode;

bool CheckInputSignatureToSeeNodeNeedsExecuting(BuildQueue *queue, ThreadState *thread_state, RuntimeNode *node);
