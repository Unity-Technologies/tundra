#pragma once

struct ExecResult;
namespace Frozen { struct DagNode; }

namespace ValidationResult
{
    enum Enum
    {
        Pass,
        SwallowStdout,
        UnexpectedConsoleOutputFail,
        UnwrittenOutputFileFail
    };
}

ValidationResult::Enum ValidateExecResultAgainstAllowedOutput(ExecResult *result, const Frozen::DagNode *node_data);
