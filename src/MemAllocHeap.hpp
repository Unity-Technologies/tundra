#pragma once

#include "Common.hpp"
#include "Thread.hpp"
#include "Mutex.hpp"
#include "Atomic.hpp"
#include <string.h>

#define DEBUG_HEAP 1

struct MemAllocHeap
{
#if DEBUG_HEAP
    uint64_t m_Size;
#endif
};

void HeapInit(MemAllocHeap *heap);
void HeapDestroy(MemAllocHeap *heap);
void HeapVerifyNoLeaks();
void *HeapAllocate(MemAllocHeap *heap, size_t size);
void *HeapAllocateAligned(MemAllocHeap *heap, size_t size, size_t alignment);

void HeapFree(MemAllocHeap *heap, const void *ptr);

void *HeapReallocate(MemAllocHeap *heap, void *ptr, size_t size);

template <typename T>
T *HeapAllocateArray(MemAllocHeap *heap, size_t count)
{
    return (T *)HeapAllocate(heap, sizeof(T) * count);
}

template <typename T>
T *HeapAllocateArrayZeroed(MemAllocHeap *heap, size_t count)
{
    T *result = HeapAllocateArray<T>(heap, sizeof(T) * count);
    memset(result, 0, sizeof(T) * count);
    return result;
}

