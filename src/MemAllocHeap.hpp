#pragma once

#include "Common.hpp"
#include "Thread.hpp"
#include "Mutex.hpp"

#include <string.h>



struct MemAllocHeap
{
    size_t m_Size;
};

void HeapInit(MemAllocHeap *heap);
void HeapDestroy(MemAllocHeap *heap);

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


inline char *StrDupN(MemAllocHeap *allocator, const char *str, size_t len)
{
    size_t sz = len + 1;
    char *buffer = static_cast<char *>(HeapAllocate(allocator, sz));
    memcpy(buffer, str, sz - 1);
    buffer[sz - 1] = '\0';
    return buffer;
}

inline char *StrDup(MemAllocHeap *allocator, const char *str)
{
    return StrDupN(allocator, str, strlen(str));
}
