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

typedef bool IgnoreFunc(void* userData, const char* filename);
struct IgnoreCallback
{
    void* userData;
    IgnoreFunc* callback;

    bool Invoke(const char* filename)
    {
        return callback(userData, filename);
    }
};

bool ScanImplicitDeps(StatCache *stat_cache, const ScanInput *input, ScanOutput *output, IgnoreCallback* ignoreCallback = nullptr);
