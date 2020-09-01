#include "Exec.hpp"
#include "DagData.hpp"
#include "re.h"

#include "OutputValidation.hpp"



static bool HasAnyNonNewLine(const char *buffer)
{
    while (true)
    {
        char c = *buffer;
        if (c == 0)
            return false;
        if (c == 0xD || c == 0xA)
            buffer++;
        else
            return true;
    }
}

ValidationResult::Enum ValidateExecResultAgainstAllowedOutput(ExecResult *result, const Frozen::DagNode *node_data)
{
    auto &allowed = node_data->m_AllowedOutputSubstrings;
    bool allowOutput = node_data->m_Flags & Frozen::DagNode::kFlagAllowUnexpectedOutput;

    if (allowOutput && allowed.GetCount() == 0)
        return ValidationResult::Pass;

    const char *buffer = result->m_OutputBuffer.buffer;
    if (!HasAnyNonNewLine(buffer))
        return ValidationResult::Pass;

    for (int i = 0; i != allowed.GetCount(); i++)
    {
        const char *allowedSubstring = allowed[i];

        if (re_match(allowedSubstring, result->m_OutputBuffer.buffer) != -1)
        {
            return ValidationResult::SwallowStdout;
        }
    }
    return allowOutput ? ValidationResult::Pass : ValidationResult::UnexpectedConsoleOutputFail;
}


