#pragma once
#include "RuntimeNode.hpp"

struct BuildQueue;
struct ThreadState;
struct Mutex;

NodeBuildResult::Enum RunAction(BuildQueue *queue, ThreadState *thread_state, RuntimeNode *node, Mutex *queue_lock);

