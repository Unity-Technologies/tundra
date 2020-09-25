#include <stdint.h>

namespace FileSystem
{
    extern volatile uint64_t g_LastSeenFileSystemTime;
}

void FileSystemInit(const char* lastSeenFileSystemTimeSampleFile);
void FileSystemDestroy();
uint64_t FileSystemUpdateLastSeenFileSystemTime();
void FileSystemWaitUntilFileModificationDateIsInThePast(uint64_t timeThatNeedsToBeInThePast);
