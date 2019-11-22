#include <cstdint>


struct MemAllocHeap;
struct SharedResourceData;
struct BuildQueue;

bool SharedResourceAcquire(BuildQueue *queue, MemAllocHeap *heap, uint32_t sharedResourceIndex);
void SharedResourceDestroy(BuildQueue *queue, MemAllocHeap *heap, uint32_t sharedResourceIndex);

