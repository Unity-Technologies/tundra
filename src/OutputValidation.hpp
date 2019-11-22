#pragma once

struct ExecResult;
namespace Frozen { struct Node; }

enum ValidationResult
{
    Pass,
    SwallowStdout,
    UnexpectedConsoleOutputFail,
    UnwrittenOutputFileFail
};

ValidationResult ValidateExecResultAgainstAllowedOutput(ExecResult *result, const Frozen::Node *node_data);
