#ifndef OUTPUT_VALIDATION_HPP
#define OUTPUT_VALIDATION_HPP


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

#endif
