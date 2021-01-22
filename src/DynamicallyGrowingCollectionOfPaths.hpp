#pragma once
#include "Buffer.hpp"
#include <stdint.h>
struct MemAllocHeap;

struct DynamicallyGrowingCollectionOfPaths
{
private:
    MemAllocHeap* m_Heap;

    //the storage for dynamically collected paths is done in two blocks. one block will store offsets, one per path. these offsets point into the payload block.
    Buffer<char> m_PathData;
    Buffer<uint32_t> m_PathOffsets;

public:
    uint32_t Count() const;
    void Initialize(MemAllocHeap* heap);
    void Destroy();
    void Add(const char* path);
    void AddFilesInDirectory(const char* directoryToList);
    const char* Get(uint32_t index) const;
};
