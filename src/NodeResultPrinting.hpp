#pragma once

#include <ctime>
#include "OutputValidation.hpp"
#include <stdint.h>


struct ExecResult;
namespace Frozen { struct DagNode; };
struct BuildQueue;
struct ThreadState;

namespace MessageStatusLevel
{
enum Enum
{
    Success = 0,
    Failure = 1,
    Warning = 2,
    Info = 3,
};
}

void InitNodeResultPrinting();
void PrintNodeResult(
    ExecResult *result,
    const Frozen::DagNode *node_data,
    const char *cmd_line,
    BuildQueue *queue,
    ThreadState *thread_state,
    bool always_verbose,
    uint64_t time_exec_started,
    ValidationResult validationResult,
    const bool *untouched_outputs,
    bool was_preparation_error);
int PrintNodeInProgress(const Frozen::DagNode *node_data, uint64_t time_of_start, const BuildQueue *queue);
void PrintDeferredMessages(BuildQueue *queue);
void PrintNonNodeActionResult(double duration, int max_nodes, MessageStatusLevel::Enum status_level, const char *annotation, ExecResult *result = nullptr);
void PrintServiceMessage(MessageStatusLevel::Enum statusLevel, const char *formatString, ...);
void StripAnsiColors(char *buffer);

