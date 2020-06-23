#pragma once

struct ThreadState;
struct BuildQueue;
struct RuntimeNode;
struct StatCache;

bool CheckInputSignatureToSeeNodeNeedsExecuting(BuildQueue *queue, ThreadState *thread_state, RuntimeNode *node);
bool OutputFilesMissing(StatCache *stat_cache, RuntimeNode* node);

