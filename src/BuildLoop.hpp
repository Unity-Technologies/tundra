#pragma once
struct ThreadState;
struct MemAllocLinear;
struct RuntimeNode;

void BuildLoop(ThreadState *thread_state);
void LogEnqueue(MemAllocLinear* scratch, RuntimeNode* enqueuedNode, RuntimeNode* enqueueingNode);

