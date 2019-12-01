#pragma once

struct ExecResult;
namespace Frozen { struct DagNode; }

enum ValidationResult
{
    Pass,
    SwallowStdout,
    UnexpectedConsoleOutputFail,
    UnwrittenOutputFileFail
};

ValidationResult ValidateExecResultAgainstAllowedOutput(ExecResult *result, const Frozen::DagNode *node_data);
