#pragma once;
#include <stdint.h>
struct MemAllocHeap;

struct DynamicallyGrowingCollectionOfPaths
{
private:
    MemAllocHeap* heap;

    //the storage for dynamically collected paths is done in two blocks. one block will store offsets, one per path. these offsets point into the payload block.

    //the start of the offsetblock
    uint32_t* offsetsBlock;
    //the current size (measured in number of offsets) of the offset block
    int currentOffsetsSizeInInts;
    //the amount of paths currently stored in this container.
    int numberOfStoredPaths;


    //this is the payload block where the actual string data for the paths is stored, including null terminators.
    char* payloadBlock;
    //the size of the current payload block in bytes
    uint32_t currentPayloadBlockSizeInBytes;
    //the offset into the payload block where we have room to write the next path
    uint32_t payloadOffsetForNextPath;

public:
    uint32_t Count() const;
    void Initialize(MemAllocHeap* heap);
    void Destroy();
    void Add(const char* path);
    void AddFilesInDirectory(const char* directoryToList);
    const char* Get(uint32_t index) const;
};
