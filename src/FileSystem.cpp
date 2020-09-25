#include "FileInfo.hpp"
#include "Mutex.hpp"
#include "PathUtil.hpp"
#include <stdio.h>
#include <sys/stat.h>

namespace FileSystem
{
    volatile uint64_t g_LastSeenFileSystemTime;
}

static FILE *s_LastSeenFileSystemTimeSampleFile;
static Mutex s_LastSeenFileSystemTimeLock;

void FileSystemInit(const char *dag_fn)
{
    MutexInit(&s_LastSeenFileSystemTimeLock);

    char lastSeenFileSystemTimeSampleFile[kMaxPathLength];
    snprintf(lastSeenFileSystemTimeSampleFile, sizeof lastSeenFileSystemTimeSampleFile, "%s_fsmtime", dag_fn);
    lastSeenFileSystemTimeSampleFile[sizeof(lastSeenFileSystemTimeSampleFile) - 1] = '\0';

    s_LastSeenFileSystemTimeSampleFile = fopen(lastSeenFileSystemTimeSampleFile, "w");
    if (s_LastSeenFileSystemTimeSampleFile == nullptr)
        CroakErrno("Unable to create timestamp file '%s'", lastSeenFileSystemTimeSampleFile);
}

void FileSystemDestroy()
{
    fclose(s_LastSeenFileSystemTimeSampleFile);
    MutexDestroy(&s_LastSeenFileSystemTimeLock);
}

uint64_t FileSystemUpdateLastSeenFileSystemTime()
{
    MutexLock(&s_LastSeenFileSystemTimeLock);

    uint64_t valueToWrite = FileSystem::g_LastSeenFileSystemTime; // not important what we write, just that we write something.

    rewind(s_LastSeenFileSystemTimeSampleFile);
    fwrite(&valueToWrite, sizeof valueToWrite, 1, s_LastSeenFileSystemTimeSampleFile);
    fflush(s_LastSeenFileSystemTimeSampleFile);

#if defined(TUNDRA_UNIX)
    struct stat fileInformation;
    if (fstat(fileno(s_LastSeenFileSystemTimeSampleFile), &fileInformation) != 0)
        CroakErrno("Unable to read file timestamp");
#elif defined(TUNDRA_WIN32)
    struct __stat64 stbuf;
    if (_fstat64(fileno(s_LastSeenFileSystemTimeSampleFile), &fileInformation) != 0)
        CroakErrno("Unable to read file timestamp");
#endif
    FileSystem::g_LastSeenFileSystemTime = fileInformation.st_mtime;
    MutexUnlock(&s_LastSeenFileSystemTimeLock);

    return FileSystem::g_LastSeenFileSystemTime;
}

#if TUNDRA_UNIX
#include <unistd.h> // usleep
#endif

void FileSystemWaitUntilFileModificationDateIsInThePast(uint64_t timeThatNeedsToBeInThePast)
{
    // Wait for next file system tick
    while (timeThatNeedsToBeInThePast >= FileSystemUpdateLastSeenFileSystemTime())
    {
#if TUNDRA_WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
    }
}