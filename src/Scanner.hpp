#pragma once

#include "Common.hpp"
#include "Buffer.hpp"

// High-level include scanner

namespace Frozen { struct ScannerData; }
struct MemAllocLinear;
struct MemAllocHeap;
struct ScanCache;
struct StatCache;

struct ScanInput
{
    const Frozen::ScannerData *m_ScannerConfig;
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

typedef bool IncludeFunc(void* userData, const char* includingFile, const char *includedFile);
struct IncludeCallback
{
    void* userData;
    IncludeFunc* callback;

    bool Invoke(const char* includingFile, const char *includedFile)
    {
        return callback(userData, includingFile, includedFile);
    }
};

bool ScanImplicitDeps(StatCache *stat_cache, const ScanInput *input, ScanOutput *output, IncludeCallback* includeCallback = nullptr);
