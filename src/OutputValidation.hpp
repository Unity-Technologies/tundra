#pragma once

struct ExecResult;
struct NodeData;

enum ValidationResult
{
    Pass,
    SwallowStdout,
    UnexpectedConsoleOutputFail,
    UnwrittenOutputFileFail
};

ValidationResult ValidateExecResultAgainstAllowedOutput(ExecResult *result, const NodeData *node_data);
