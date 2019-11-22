#pragma once

#include "Common.hpp"

struct GenericScannerData;
struct MemAllocLinear;


struct IncludeData
{
    const char *m_String;
    size_t m_StringLen;
    bool m_IsSystemInclude;
    bool m_ShouldFollow;
    IncludeData *m_Next;
};

// Scan C/C++ style #includes from buffer.
// Buffer must be null-terminated and will be modified in place.
IncludeData *
ScanIncludesCpp(char *buffer, MemAllocLinear *allocator);

// Scan generic includes from buffer (slower, customizable).
// Buffer must be null-terminated and will be modified in place.
IncludeData *
ScanIncludesGeneric(char *buffer, MemAllocLinear *allocator, const GenericScannerData &config);

