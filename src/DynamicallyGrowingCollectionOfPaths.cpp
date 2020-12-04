#include "DynamicallyGrowingCollectionOfPaths.hpp"
#include "MemAllocHeap.hpp"
#include "FileInfo.hpp"

void DynamicallyGrowingCollectionOfPaths::Add(const char* path)
{
    int stringLengthIncludingTerminator = strlen(path) + 1;
    uint32_t requiredPayloadBytes = stringLengthIncludingTerminator;

    while (requiredPayloadBytes > (currentPayloadBlockSizeInBytes - payloadOffsetForNextPath))
    {
        payloadBlock = (char*) HeapReallocate(heap, payloadBlock, currentPayloadBlockSizeInBytes * 2);
        currentPayloadBlockSizeInBytes *= 2;
    }
    memcpy(payloadBlock + payloadOffsetForNextPath, path, stringLengthIncludingTerminator);

    if (currentOffsetsSizeInInts == numberOfStoredPaths)
    {
        offsetsBlock = (uint32_t*) HeapReallocate(heap, offsetsBlock, currentOffsetsSizeInInts * 2 * sizeof(uint32_t));
        currentOffsetsSizeInInts *= 2;
    }

    offsetsBlock[numberOfStoredPaths++] = payloadOffsetForNextPath;
    payloadOffsetForNextPath += stringLengthIncludingTerminator;
}

uint32_t DynamicallyGrowingCollectionOfPaths::Count() const
{
    return numberOfStoredPaths;
}

const char* DynamicallyGrowingCollectionOfPaths::Get(uint32_t index) const
{
    return &payloadBlock[offsetsBlock[index]];
}

void DynamicallyGrowingCollectionOfPaths::Initialize(MemAllocHeap* heap)
{
    this->heap = heap;

    offsetsBlock = (uint32_t*) HeapAllocate(heap, 8 * sizeof(uint32_t));
    currentOffsetsSizeInInts = 8;
    numberOfStoredPaths = 0;

    payloadBlock = (char*) HeapAllocate(heap, 1024);
    currentPayloadBlockSizeInBytes = 1024;
    payloadOffsetForNextPath = 0;
}

void DynamicallyGrowingCollectionOfPaths::Destroy()
{
    HeapFree(heap, offsetsBlock);
    HeapFree(heap, payloadBlock);
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
