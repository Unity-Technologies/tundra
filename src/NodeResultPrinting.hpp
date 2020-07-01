#pragma once

#include <ctime>
#include "OutputValidation.hpp"
#include <stdint.h>


struct ExecResult;
namespace Frozen { struct DagNode; };
struct BuildQueue;
struct ThreadState;
struct DriverOptions;

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


void PrintMessage(MessageStatusLevel::Enum status_level, const char* message, ...);
void PrintMessage(MessageStatusLevel::Enum status_level, int duration, const char* message, ...);
void PrintMessage(MessageStatusLevel::Enum status_level, int duration, ExecResult *result, const char *message, ...);

void EmitColorForLevel(MessageStatusLevel::Enum status_level);
void EmitColorReset();
void InitNodeResultPrinting(const DriverOptions* driverOptions);
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
int PrintNodeInProgress(const Frozen::DagNode *node_data, uint64_t time_of_start, const BuildQueue *queue, const char* message = nullptr);
void PrintDeferredMessages(BuildQueue *queue);
void PrintServiceMessage(MessageStatusLevel::Enum statusLevel, const char *formatString, ...);
void StripAnsiColors(char *buffer);

