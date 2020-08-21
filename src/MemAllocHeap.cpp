#include "MemAllocHeap.hpp"
#include <stdlib.h>
#include <stdio.h>

void HeapInit(MemAllocHeap *heap)
{
#if DEBUG_HEAP    
    heap->m_Size = 0;
#endif    
}

void HeapDestroy(MemAllocHeap *heap)
{
#if DEBUG_HEAP    
    if (heap->m_Size != 0)
        Croak("Destroying heap %p which still contains %zu bytes of allocated memory, which indicates a memory leak.", heap, (size_t)heap->m_Size);
#endif        
}

void *HeapAllocate(MemAllocHeap *heap, size_t size)
{
#if DEBUG_HEAP    
    size_t* ptr = (size_t*)malloc(size + sizeof(size_t));
#ifdef LOG_ALLOC    
    printf("%p %p HeapAllocate %zu\n", heap, ptr, size);
    print_trace();
#endif
    *ptr = size;
    heap->m_Size += size;
    return ptr + 1;
#else
    return malloc(size);
#endif    
}

void HeapFree(MemAllocHeap *heap, const void *_ptr)
{
    if (_ptr == nullptr)
        return;
    size_t* ptr = (size_t*)_ptr;
#if DEBUG_HEAP    
    ptr--;
#ifdef LOG_ALLOC    
    printf("%p %p HeapFree %zu\n", heap, ptr, *ptr);
#endif    
    heap->m_Size -= *ptr;
#endif    
    free(ptr);
}

void *HeapReallocate(MemAllocHeap *heap, void *_ptr, size_t size)
{
#if DEBUG_HEAP    
    if (_ptr == nullptr)
        return HeapAllocate(heap, size);

    size_t* ptr = (size_t*)_ptr;
    ptr--;
    heap->m_Size -= *ptr;
    size_t* new_ptr = (size_t*)realloc(ptr, size + sizeof(size_t));
#ifdef LOG_ALLOC    
    printf("%p %p HeapFree (reallocate)\n", heap, ptr);
    printf("%p %p HeapAllocate (reallocate) %zu\n", heap, new_ptr, size);
#endif    
    if (!new_ptr)
        Croak("out of memory reallocating %d bytes at %p", (int)size, ptr);

    *new_ptr = size;
    heap->m_Size += size;
    return new_ptr + 1;
#else
    void* new_ptr = realloc(_ptr, size);
    if (!new_ptr)
        Croak("out of memory reallocating %d bytes at %p", (int)size, _ptr);
    return new_ptr;
#endif
}


