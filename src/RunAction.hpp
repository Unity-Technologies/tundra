#pragma once
#include "NodeState.hpp"

struct BuildQueue;
struct ThreadState;
struct Mutex;

NodeBuildResult::Enum RunAction(BuildQueue *queue, ThreadState *thread_state, NodeState *node, Mutex *queue_lock);

