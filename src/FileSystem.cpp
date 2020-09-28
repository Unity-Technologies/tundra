#include "FileInfo.hpp"
#include "Mutex.hpp"
#include "PathUtil.hpp"
#include <stdio.h>
#include <sys/stat.h>

namespace FileSystem
{
    volatile uint64_t g_LastSeenFileSystemTime;
}

char s_LastSeenFileSystemTimeSampleFile[kMaxPathLength];
static Mutex s_LastSeenFileSystemTimeLock;

void FileSystemInit(const char *dag_fn)
{
    MutexInit(&s_LastSeenFileSystemTimeLock);

    snprintf(s_LastSeenFileSystemTimeSampleFile, sizeof s_LastSeenFileSystemTimeSampleFile, "%s_fsmtime", dag_fn);
    s_LastSeenFileSystemTimeSampleFile[sizeof(s_LastSeenFileSystemTimeSampleFile) - 1] = '\0';
}

void FileSystemDestroy()
{
    MutexDestroy(&s_LastSeenFileSystemTimeLock);
}

uint64_t FileSystemUpdateLastSeenFileSystemTime()
{
    MutexLock(&s_LastSeenFileSystemTimeLock);

    // One may think that a better (performance wise) implementation would be to open
    // the file in FileSystemInit and only operate on the file descriptor. Don't do this! :P
    // Windows has a feature where file modifications only gets flushed to disk every 8 seconds
    // or so - unless some other process also has the file open. Which means we won't see our own
    // mtime updates until x seconds after making them. And based on some local testing only if
    // contents of the file changed.
    //
    // To workaround this cache behavior we need to open and close the file for each mtime update.

    uint64_t valueToWrite = FileSystem::g_LastSeenFileSystemTime; // not important what we write, just that we write something.
    FILE* lastSeenFileSystemTimeSampleFileFd = fopen(s_LastSeenFileSystemTimeSampleFile, "w");
    if (lastSeenFileSystemTimeSampleFileFd == nullptr)
        CroakErrno("Unable to create timestamp file '%s'", lastSeenFileSystemTimeSampleFileFd);

    fwrite(&valueToWrite, sizeof valueToWrite, 1, lastSeenFileSystemTimeSampleFileFd);
    fclose(lastSeenFileSystemTimeSampleFileFd);

    FileSystem::g_LastSeenFileSystemTime = GetFileInfo(s_LastSeenFileSystemTimeSampleFile).m_Timestamp;
    MutexUnlock(&s_LastSeenFileSystemTimeLock);
    return FileSystem::g_LastSeenFileSystemTime;
}

#if TUNDRA_UNIX
#include <unistd.h>
#define Sleep(x) usleep(x * 1000)
#endif

void FileSystemWaitUntilFileModificationDateIsInThePast(uint64_t timeThatNeedsToBeInThePast)
{
    // Wait for next file system tick
    while (timeThatNeedsToBeInThePast >= FileSystemUpdateLastSeenFileSystemTime())
        Sleep(100);
}
