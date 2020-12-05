#include "DynamicallyGrowingCollectionOfPaths.hpp"
#include "MemAllocHeap.hpp"
#include "FileInfo.hpp"

void DynamicallyGrowingCollectionOfPaths::Add(const char* path)
{
    int stringLengthIncludingTerminator = strlen(path) + 1;
    char* payloadBlock = BufferAlloc(&m_PathData, m_Heap, stringLengthIncludingTerminator);
    memcpy(payloadBlock, path, stringLengthIncludingTerminator);

    uint32_t* offset = BufferAlloc(&m_PathOffsets, m_Heap, 1);
    *offset = payloadBlock - m_PathData.begin();
}

uint32_t DynamicallyGrowingCollectionOfPaths::Count() const
{
    return m_PathOffsets.m_Size;
}

const char* DynamicallyGrowingCollectionOfPaths::Get(uint32_t index) const
{
    return &m_PathData[m_PathOffsets[index]];
}

void DynamicallyGrowingCollectionOfPaths::Initialize(MemAllocHeap* heap)
{
    m_Heap = heap;
    BufferInitWithCapacity(&m_PathData, heap, 1024);
    BufferInitWithCapacity(&m_PathOffsets, heap, 8);
}

void DynamicallyGrowingCollectionOfPaths::Destroy()
{
    BufferDestroy(&m_PathData, m_Heap);
    BufferDestroy(&m_PathOffsets, m_Heap);
}

static void Callback(void* ctx, const FileInfo& fileInfo, const char* path)
{
    DynamicallyGrowingCollectionOfPaths& collection = *(DynamicallyGrowingCollectionOfPaths*)ctx;
    collection.Add(path);
}

void DynamicallyGrowingCollectionOfPaths::AddFilesInDirectory(const char* directoryToList)
{
    ListDirectory(directoryToList, "*", true, this, Callback);
}
