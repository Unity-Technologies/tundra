#pragma once

#include "Common.hpp"

// High-level include scanner

namespace Frozen { struct ScannerData; }
struct MemAllocLinear;
struct MemAllocHeap;
struct ScanCache;
struct StatCache;

struct ScanInput
{
    const Frozen::ScannerData *m_ScannerConfig;
    bool m_SafeToScanBeforeDependenciesAreProduced;
    MemAllocLinear *m_ScratchAlloc;
    MemAllocHeap *m_ScratchHeap;
    const char *m_FileName;
    ScanCache *m_ScanCache;
};

struct ScanOutput
{
    int m_IncludedFileCount;
    const FileAndHash *m_IncludedFiles;
};

// This callback will be called for any newly discovered (ie, not loaded from cache)
// includes found during scanning.
// `includingFile` is the file which contains the include statement
// `includedFile` is the file being included.
// The callback returns a bool, which tells the scanner if it should ignore this included file.
// If the callback returns true, the included file will be ignored, which means it will not be added
// to the list of found includes, and it will not be traversed for further scanning.
typedef bool IncludeFilterCallbackFunc(void* userData, const char* includingFile, const char *includedFile);
struct IncludeFilterCallback
{
    void* userData;
    IncludeFilterCallbackFunc* callback;

    bool Invoke(const char* includingFile, const char *includedFile)
    {
        return callback(userData, includingFile, includedFile);
    }
};

bool ScanImplicitDeps(StatCache *stat_cache, const ScanInput *input, ScanOutput *output, IncludeFilterCallback* includeCallback = nullptr);
