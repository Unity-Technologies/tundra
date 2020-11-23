#pragma once

struct ThreadState;
struct BuildQueue;
struct RuntimeNode;

bool CheckInputSignatureToSeeNodeNeedsExecuting(BuildQueue *queue, ThreadState *thread_state, RuntimeNode *node);

HashDigest HashTimestampsOfNonGeneratedInputFiles(BuildQueue* queue, RuntimeNode* node, const Frozen::DagDerived* dagDerived, bool bypassStatCache, bool waitForSafeTimesamp);
