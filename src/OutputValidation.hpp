#pragma once

struct ExecResult;
namespace Frozen { struct DagNode; }

namespace ValidationResult
{
    enum Enum
    {
        Pass = 0,
        SwallowStdout = 1,
        UnexpectedConsoleOutputFail = 2,
        UnwrittenOutputFileFail = 3
    };
}

ValidationResult::Enum ValidateExecResultAgainstAllowedOutput(ExecResult *result, const Frozen::DagNode *node_data);
