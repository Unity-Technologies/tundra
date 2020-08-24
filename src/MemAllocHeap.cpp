#include "MemAllocHeap.hpp"
#include <stdlib.h>
#include <stdio.h>

#if TUNDRA_WIN32
#include "DbgHelp.h"
#endif

#define LOG_ALLOC 0

#if LOG_ALLOC && TUNDRA_WIN32

static bool GetStackWalk( std::string &outWalk )
{
    // Set up the symbol options so that we can gather information from the current
    // executable's PDB files, as well as the Microsoft symbol servers.  We also want
    // to undecorate the symbol names we're returned.  If you want, you can add other
    // symbol servers or paths via a semi-colon separated list in SymInitialized.
    ::SymSetOptions( SYMOPT_DEFERRED_LOADS | SYMOPT_INCLUDE_32BIT_MODULES | SYMOPT_UNDNAME );
    if (!::SymInitialize( ::GetCurrentProcess(), "http://msdl.microsoft.com/download/symbols", TRUE )) return false;

    // Capture up to 25 stack frames from the current call stack.  We're going to
    // skip the first stack frame returned because that's the GetStackWalk function
    // itself, which we don't care about.
    PVOID addrs[ 25 ] = { 0 };
    USHORT frames = CaptureStackBackTrace( 1, 25, addrs, NULL );

    for (USHORT i = 0; i < frames; i++) {
        // Allocate a buffer large enough to hold the symbol information on the stack and get
        // a pointer to the buffer.  We also have to set the size of the symbol structure itself
        // and the number of bytes reserved for the name.
        ULONG64 buffer[ (sizeof( SYMBOL_INFO ) + 1024 + sizeof( ULONG64 ) - 1) / sizeof( ULONG64 ) ] = { 0 };
        SYMBOL_INFO *info = (SYMBOL_INFO *)buffer;
        info->SizeOfStruct = sizeof( SYMBOL_INFO );
        info->MaxNameLen = 1024;

        // Attempt to get information about the symbol and add it to our output parameter.
        DWORD64 displacement = 0;
        if (::SymFromAddr( ::GetCurrentProcess(), (DWORD64)addrs[ i ], &displacement, info )) {
            outWalk.append( info->Name, info->NameLen );
            outWalk.append( "\n" );
        }
    }

        ::SymCleanup( ::GetCurrentProcess() );

    return true;
}

static void print_trace()
{
    std::string output;
    GetStackWalk(output);
    printf("trace: %s\n", output.c_str());
}

#endif

static uint64_t s_ActiveHeaps = 0;

void HeapVerifyNoLeaks()
{
    if (s_ActiveHeaps != 0)
        Croak("%d heaps have been initialized but not destroyed.", s_ActiveHeaps);
}

void HeapInit(MemAllocHeap *heap)
{
    AtomicAdd(&s_ActiveHeaps, 1);
#if DEBUG_HEAP
    heap->m_Size = 0;
#endif
}

void HeapDestroy(MemAllocHeap *heap)
{
    AtomicAdd(&s_ActiveHeaps, -1);
#if DEBUG_HEAP
    if (heap->m_Size != 0)
        Croak("Destroying heap %p which still contains %zu bytes of allocated memory, which indicates a memory leak.", heap, (size_t)heap->m_Size);
#endif
}

void *HeapAllocate(MemAllocHeap *heap, size_t size)
{
#if DEBUG_HEAP
    size_t* ptr = (size_t*)malloc(size + sizeof(size_t));
#if LOG_ALLOC
    printf("%p %p HeapAllocate %zu\n", heap, ptr, size);
    print_trace();
#endif
    *ptr = size;
    AtomicAdd(&heap->m_Size, size);
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
#if LOG_ALLOC
    printf("%p %p HeapFree %zu\n", heap, ptr, *ptr);
#endif
    AtomicAdd(&heap->m_Size, -*ptr);
#endif
    //free(ptr);
}

void *HeapReallocate(MemAllocHeap *heap, void *_ptr, size_t size)
{
#if DEBUG_HEAP
    if (_ptr == nullptr)
        return HeapAllocate(heap, size);

    size_t* ptr = (size_t*)_ptr;
    ptr--;
    AtomicAdd(&heap->m_Size, -*ptr);
    size_t* new_ptr = (size_t*)realloc(ptr, size + sizeof(size_t));
#if LOG_ALLOC
    printf("%p %p HeapFree (reallocate)\n", heap, ptr);
    printf("%p %p HeapAllocate (reallocate) %zu\n", heap, new_ptr, size);
#endif
    if (!new_ptr)
        Croak("out of memory reallocating %d bytes at %p", (int)size, ptr);

    *new_ptr = size;
    AtomicAdd(&heap->m_Size, size);
    return new_ptr + 1;
#else
    void* new_ptr = realloc(_ptr, size);
    if (!new_ptr)
        Croak("out of memory reallocating %d bytes at %p", (int)size, _ptr);
    return new_ptr;
#endif
}


