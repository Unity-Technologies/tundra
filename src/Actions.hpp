#pragma once

#include <stdint.h>

namespace ActionType
{
    enum Enum : uint8_t
    {
        kUnknown = 0,
        kRunShellCommand = 1,
        kWriteTextFile = 2
    };

    Enum FromString(const char* name);
    const char* ToString(Enum value);
}
