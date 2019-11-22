#pragma once

struct ExecResult;
namespace Frozen { struct NodeData; }

enum ValidationResult
{
    Pass,
    SwallowStdout,
    UnexpectedConsoleOutputFail,
    UnwrittenOutputFileFail
};

ValidationResult ValidateExecResultAgainstAllowedOutput(ExecResult *result, const Frozen::NodeData *node_data);
