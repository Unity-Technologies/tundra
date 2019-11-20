#ifndef NODERESULTPRINTING_HPP
#define NODERESULTPRINTING_HPP
#include <ctime>
#include "OutputValidation.hpp"
#include <stdint.h>

namespace t2
{
struct ExecResult;
struct NodeData;
struct BuildQueue;

namespace MessageStatusLevel
{
  enum Enum
  {
    Success     = 0,
    Failure     = 1,
    Warning     = 2,
    Info        = 3,
  };
}

void InitNodeResultPrinting();
void PrintNodeResult(
  ExecResult* result,
  const NodeData* node_data,
  const char* cmd_line,
  BuildQueue* queue,
  bool always_verbose,
  uint64_t time_exec_started,
  ValidationResult validationResult,
  const bool* untouched_outputs);
int PrintNodeInProgress(const NodeData* node_data, uint64_t time_of_start, const BuildQueue* queue);
void PrintDeferredMessages(BuildQueue* queue);
void PrintNonNodeActionResult(double duration, int max_nodes, MessageStatusLevel::Enum status_level, const char* annotation, ExecResult* result = nullptr);
void PrintServiceMessage(MessageStatusLevel::Enum statusLevel, const char* formatString, ...);
void StripAnsiColors(char* buffer);
}
#endif