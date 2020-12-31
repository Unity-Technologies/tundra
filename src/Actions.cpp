#include "Actions.hpp"
#include <string.h>

// This order should match the order of enum members in Actions.hpp
constexpr const char* kCommandNames[] = {
    "<unknown>",
    "RunShellCommand",
    "WriteTextFile",
    "CopyFiles"
};
constexpr size_t kNumCommandNames = sizeof(kCommandNames) / sizeof(kCommandNames[0]);

namespace ActionType {

    Enum FromString(const char* name)
    {
        for (size_t i = 0; i < kNumCommandNames; ++i)
        {
            if (strcmp(name, kCommandNames[i]) == 0)
            {
                return static_cast<Enum>(i);
            }
        }

        return ActionType::kUnknown;
    }

    const char* ToString(Enum value)
    {
        size_t index = static_cast<size_t>(value);
        if (index >= kNumCommandNames)
            return kCommandNames[kUnknown];

        return kCommandNames[index];
    }
}

ExecResult WriteTextFile(const char* payload, const char* target_file, MemAllocHeap* heap)
{
    ExecResult result;
    char tmpBuffer[1024];

    memset(&result, 0, sizeof(result));

    FILE* f = fopen(target_file, "wb");
    if (!f)
    {
        InitOutputBuffer(&result.m_OutputBuffer, heap);

        snprintf(tmpBuffer, sizeof(tmpBuffer), "Error opening for writing the file: %s, error: %s", target_file, strerror(errno));
        EmitOutputBytesToDestination(&result, tmpBuffer, strlen(tmpBuffer));

        result.m_ReturnCode = 1;
        return result;
    }
    int length = strlen(payload);
    int written = fwrite(payload, sizeof(char), length, f);
    fclose(f);

    if (written == length)
        return result;

    InitOutputBuffer(&result.m_OutputBuffer, heap);

    snprintf(tmpBuffer, sizeof(tmpBuffer), "fwrite was supposed to write %d bytes to %s, but wrote %d bytes", length, target_file, written);
    EmitOutputBytesToDestination(&result, tmpBuffer, strlen(tmpBuffer));

    result.m_ReturnCode = 1;
    return result;
}

#if !defined(TUNDRA_APPLE) && !defined(TUNDRA_WIN32) && !defined(TUNDRA_LINUX)

ExecResult CopyFiles(const FrozenFileAndHash* src_files, const FrozenFileAndHash* target_files, size_t files_count, StatCache* stat_cache, MemAllocHeap* heap)
{
    ExecResult result;
    memset(&result, 0, sizeof(result));

    const char notImplMsg[] = "CopyFile is not implemented yet.";

    result.m_ReturnCode = -1;
    InitOutputBuffer(&result.m_OutputBuffer, heap);
    EmitOutputBytesToDestination(&result, notImplMsg, strlen(notImplMsg));

    return result;
}

#endif
