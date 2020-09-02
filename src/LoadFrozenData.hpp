#pragma once
#include "MemoryMappedFile.hpp"

// Helper routine to load frozen data into RAM via memory mapping
template <typename FrozenType>
static bool LoadFrozenData(const char *fn, MemoryMappedFile *result, const FrozenType **ptr)
{
    MemoryMappedFile mapping;

    MmapFileInit(&mapping);

    MmapFileMap(&mapping, fn);

    if (MmapFileValid(&mapping))
    {
        char *mmap_buffer = static_cast<char *>(mapping.m_Address);
        const FrozenType *data = reinterpret_cast<const FrozenType *>(mmap_buffer);

        Log(kDebug, "%s: successfully mapped at %p (%d bytes)", fn, data, (int)mapping.m_Size);

        // Check size
        if (mapping.m_Size < sizeof(FrozenType))
        {
            Log(kWarning, "%s: Bad mmap size %d - need at least %d bytes",
                fn, (int)mapping.m_Size, (int)sizeof(FrozenType));
            goto error;
        }

        // Check magic number
        if (data->m_MagicNumber != FrozenType::MagicNumber)
        {
            Log(kDebug, "%s: Bad magic number %08x - current is %08x",
                fn, data->m_MagicNumber, FrozenType::MagicNumber);
            goto error;
        }

        // Check magic number
        if (data->m_MagicNumberEnd != FrozenType::MagicNumber)
        {
            Log(kError, "Did not find expected magic number marker at the end of %s. This most likely means data writing code for that file is writing too much or too little data", fn);
            goto error;
        }

        // Move ownership of memory mapping to member variable.
        *result = mapping;

        *ptr = data;

        return true;
    }

    Log(kDebug, "%s: mmap failed", fn);

error:
    MmapFileDestroy(&mapping);
    return false;
}