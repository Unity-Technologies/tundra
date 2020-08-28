#pragma once
struct ThreadState;
struct MemAllocLinear;
struct RuntimeNode;
struct BuildQueue;


void BuildLoop(ThreadState *thread_state);
int EnqueueNodeWithoutWakingAwaiters(BuildQueue *queue, MemAllocLinear* scratch, RuntimeNode *runtime_node, RuntimeNode* queueing_node);
