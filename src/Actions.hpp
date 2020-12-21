#pragma once

#include "Config.hpp"
#include "Exec.hpp"

#include <stdint.h>

// Windows.h defines CopyFile as a macro
#if defined(TUNDRA_WIN32) && defined(CopyFile)
#undef CopyFile
#endif

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

struct StatCache;

ExecResult WriteTextFile(const char* payload, const char* target_file, MemAllocHeap* heap);
ExecResult CopyFile(const char* src_file, const char* target_file, StatCache* stat_cache, MemAllocHeap* heap);

