#include "MemAllocHeap.hpp"
#include <stdlib.h>

void HeapInit(MemAllocHeap *heap)
{
    heap->m_Size = 0;
}

void HeapDestroy(MemAllocHeap *heap)
{
    if (heap->m_Size != 0)
        Croak("Destroying a heap which still contains %zu bytes of allocated memory, which indicates a memory leak.", heap->m_Size);
}

void *HeapAllocate(MemAllocHeap *heap, size_t size)
{
    size_t* ptr = (size_t*)malloc(size + sizeof(size_t));
    *ptr = size;
    heap->m_Size += size;
    return ptr + 1;
}

void HeapFree(MemAllocHeap *heap, const void *_ptr)
{
    if (_ptr == nullptr)
        return;
    size_t* ptr = (size_t*)_ptr;
    ptr--;
    heap->m_Size -= *ptr;
    free(ptr);
}

void *HeapReallocate(MemAllocHeap *heap, void *_ptr, size_t size)
{
    if (_ptr == nullptr)
        return HeapAllocate(heap, size);

    size_t* ptr = (size_t*)_ptr;
    ptr--;
    heap->m_Size -= *ptr;
    size_t* new_ptr = (size_t*)realloc(ptr, size + sizeof(size_t));
    if (!new_ptr)
        Croak("out of memory reallocating %d bytes at %p", (int)size, ptr);

    *new_ptr = size;
    heap->m_Size += size;
    return new_ptr + 1;
}


