#pragma once
#include "Common.hpp"
#include "Hash.hpp"
#include "ReadWriteLock.hpp"

namespace Frozen { struct ScanData; }
struct MemAllocHeap;
struct MemAllocLinear;
struct MemoryMappedFile;
struct ScanInput;

void ComputeScanCacheKey(
    HashDigest *key_out,
    const char *filename,
    const HashDigest& hash_digest,
    bool safeToScanBeforeDependenciesAreProduced);

struct ScanCacheLookupResult
{
    int m_IncludedFileCount;
    FileAndHash *m_IncludedFiles;
};

struct ScanCache
{
    struct Record;

    const Frozen::ScanData *m_FrozenData;

    ReadWriteLock m_Lock;
    MemAllocHeap *m_Heap;
    MemAllocLinear *m_Allocator;
    uint32_t m_RecordCount;
    uint32_t m_TableSize;
    Record **m_Table;
    bool m_Initialized;
    // Table of bits to track whether frozen records have been accessed.
    uint8_t *m_FrozenAccess;
};

void ScanCacheInit(ScanCache *self, MemAllocHeap *heap, MemAllocLinear *allocator);

void ScanCacheSetCache(ScanCache *self, const Frozen::ScanData *frozen_data);

void ScanCacheDestroy(ScanCache *self);

bool ScanCacheLookup(ScanCache *self, const HashDigest &key, uint64_t timestamp, ScanCacheLookupResult *result_out, MemAllocLinear *scratch);

void ScanCacheInsert(ScanCache *self, const HashDigest &key, uint64_t timestamp, const char **included_files, int count);

bool ScanCacheDirty(ScanCache *self);

bool ScanCacheSave(ScanCache *self, const char *fn, MemAllocHeap *heap);
