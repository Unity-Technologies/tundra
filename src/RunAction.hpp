#pragma once
#include "RuntimeNode.hpp"

struct BuildQueue;
struct ThreadState;
struct Mutex;

void PostRunActionBookkeeping(RuntimeNode* node, ThreadState* thread_state);
NodeBuildResult::Enum RunAction(BuildQueue *queue, ThreadState *thread_state, RuntimeNode *node, Mutex *queue_lock);

struct SlowCallbackData
{
    Mutex *queue_lock;
    const Frozen::DagNode *node_data;
    uint64_t time_of_start;
    const BuildQueue *build_queue;
};

