#include "Common.hpp"
#include "PathUtil.hpp"
#include "FileInfo.hpp"
#include "Mutex.hpp"
#include "JsonWriter.hpp"
#include "Thread.hpp"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

#if defined(TUNDRA_UNIX)
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <libgen.h>
#include <errno.h>
#endif

#if defined(TUNDRA_FREEBSD)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

#if defined(TUNDRA_WIN32)
#include <windows.h>
#include <ctype.h>
#include <shlwapi.h>
#endif

#if defined(TUNDRA_APPLE)
#include <mach-o/dyld.h>
#endif



#if defined(TUNDRA_WIN32)
static double s_PerfFrequency;
#endif

static bool DebuggerAttached()
{
#if defined(TUNDRA_WIN32)
    return IsDebuggerPresent() ? true : false;
#else
    return false;
#endif
}

static void NORETURN FlushAndAbort()
{
    // The C standard does not require 'abort' to flush stream buffers.
    // On Windows at least, an explicit 'fflush' is required for stderr messages to actually be shown.
    fflush(NULL);
    abort();
}

void PrintErrno()
{
#if TUNDRA_WIN32
    wchar_t buf[256];
    DWORD lastError = GetLastError();
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, lastError, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   buf, sizeof(buf), NULL);
    fprintf(stderr, "errno: %d (%s) GetLastError: %d (0x%08X): %ls\n", errno, strerror(errno), (int)lastError, (unsigned int)lastError, buf);
#else
    fprintf(stderr, "errno: %d (%s)\n", errno, strerror(errno));
#endif
}

void InitCommon(void)
{
#if defined(TUNDRA_WIN32)
    ThreadSetName((ThreadId)GetCurrentThread(), "Tundra Main Thread");

    static LARGE_INTEGER freq;
    if (!QueryPerformanceFrequency(&freq))
        CroakErrno("QueryPerformanceFrequency failed");
    s_PerfFrequency = double(freq.QuadPart);

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX | SEM_NOALIGNMENTFAULTEXCEPT);
#endif
}

void NORETURN Croak(const char *fmt, ...)
{
    fputs("tundra: error: ", stderr);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    if (DebuggerAttached())
        FlushAndAbort();
    else
        exit(1);
}

void NORETURN CroakErrno(const char *fmt, ...)
{
    fputs("tundra: error: ", stderr);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    PrintErrno();
    if (DebuggerAttached())
        FlushAndAbort();
    else
        exit(1);
}

void NORETURN CroakAbort(const char *fmt, ...)
{
    fputs("tundra: error: ", stderr);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    FlushAndAbort();
}

void NORETURN CroakErrnoAbort(const char *fmt, ...)
{
    fputs("tundra: error: ", stderr);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    PrintErrno();
    FlushAndAbort();
}

uint32_t Djb2Hash(const char *str_)
{
    const uint8_t *str = (const uint8_t *)str_;
    uint32_t hash = 5381;
    int c;

    while (0 != (c = *str++))
    {
        hash = (hash * 33) + c;
    }

    return hash ? hash : 1;
}

uint64_t Djb2Hash64(const char *str_)
{
    const uint8_t *str = (const uint8_t *)str_;
    uint64_t hash = 5381;
    int c;

    while (0 != (c = *str++))
    {
        hash = (hash * 33) + c;
    }

    return hash ? hash : 1;
}

uint32_t Djb2HashNoCase(const char *str_)
{
    const uint8_t *str = (const uint8_t *)str_;
    uint32_t hash = 5381;
    int c;

    while (0 != (c = *str++))
    {
        hash = (hash * 33) + FoldCase(c);
    }

    return hash ? hash : 1;
}

uint64_t Djb2HashNoCase64(const char *str_)
{
    const uint8_t *str = (const uint8_t *)str_;
    uint64_t hash = 5381;
    int c;

    while (0 != (c = *str++))
    {
        hash = (hash * 33) + FoldCase(c);
    }

    return hash ? hash : 1;
}

static int s_LogFlags = 0;

int GetLogFlags()
{
    return s_LogFlags;
}

void SetLogFlags(int log_flags)
{
    s_LogFlags = log_flags;
}

void Log(LogLevel level, const char *fmt, ...)
{
    if (s_LogFlags & level)
    {
        const char *prefix = "?";

        switch (level)
        {
        case kError:
            prefix = "E";
            break;
        case kWarning:
            prefix = "W";
            break;
        case kInfo:
            prefix = "I";
            break;
        case kDebug:
            prefix = "D";
            break;
        case kSpam:
            prefix = "S";
            break;
        default:
            break;
        }
        fprintf(stderr, "[%s] ", prefix);

        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);

        fprintf(stderr, "\n");
    }
}

static FILE *s_StructuredLog = nullptr;
static Mutex s_StructuredLogMutex;

void SetStructuredLogFileName(const char *path)
{
    if (s_StructuredLog != nullptr)
    {
        fclose(s_StructuredLog);
        MutexDestroy(&s_StructuredLogMutex);
        s_StructuredLog = nullptr;
    }

    if (path != nullptr)
    {
        s_StructuredLog = fopen(path, "w");
        MutexInit(&s_StructuredLogMutex);
    }
}

bool IsStructuredLogActive()
{
    return s_StructuredLog != nullptr;
}

void LogStructured(JsonWriter *writer)
{
    if (s_StructuredLog == nullptr)
        return;

    MutexLock(&s_StructuredLogMutex);

    JsonWriteToFile(writer, s_StructuredLog);
    fputc('\n', s_StructuredLog);

    MutexUnlock(&s_StructuredLogMutex);

    fflush(s_StructuredLog);
}

void GetCwd(char *buffer, size_t buffer_size)
{
#if defined(TUNDRA_WIN32)
    DWORD res = GetCurrentDirectoryA((DWORD)buffer_size, buffer);
    if (0 == res || ((DWORD)buffer_size) <= res)
        CroakErrno("couldn't get working directory");
#elif defined(TUNDRA_UNIX)
    if (NULL == getcwd(buffer, buffer_size))
        CroakErrno("couldn't get working directory");
#else
#error Unsupported platform
#endif
}

bool SetCwd(const char *dir)
{
#if defined(TUNDRA_WIN32)
    return TRUE == SetCurrentDirectoryA(dir);
#elif defined(TUNDRA_UNIX)
    return 0 == chdir(dir);
#else
#error Unsupported platform
#endif
}

static char s_ExePath[kMaxPathLength];

const char *GetExePath()
{
    if (s_ExePath[0] == '\0')
    {
#if defined(TUNDRA_WIN32)
        if (!GetModuleFileNameA(NULL, s_ExePath, (DWORD)sizeof s_ExePath))
            CroakErrno("couldn't get module filename");
#elif defined(TUNDRA_APPLE)
        uint32_t size = sizeof s_ExePath;
        if (0 != _NSGetExecutablePath(s_ExePath, &size))
            Croak("_NSGetExecutablePath failed");
#elif defined(TUNDRA_LINUX)
        if (-1 == readlink("/proc/self/exe", s_ExePath, (sizeof s_ExePath) - 1))
            CroakErrno("couldn't read /proc/self/exe to get exe path");
#elif defined(TUNDRA_FREEBSD)
        size_t cb = sizeof s_ExePath;
        int mib[4];
        mib[0] = CTL_KERN;
        mib[1] = KERN_PROC;
        mib[2] = KERN_PROC_PATHNAME;
        mib[3] = -1;
        if (0 != sysctl(mib, 4, s_ExePath, &cb, NULL, 0))
            CroakErrno("KERN_PROC_PATHNAME syscall failed");
#else
#error "unsupported platform"
#endif
    }

    return s_ExePath;
}

uint32_t NextPowerOfTwo(uint32_t val)
{
    uint32_t mask = val - 1;

    mask |= mask >> 16;
    mask |= mask >> 8;
    mask |= mask >> 4;
    mask |= mask >> 2;
    mask |= mask >> 1;

    uint32_t pow2 = mask + 1;
    CHECK(pow2 >= val);
    CHECK((pow2 & mask) == 0);
    CHECK((pow2 & ~mask) == pow2);

    return pow2;
}

uint64_t TimerGet()
{
#if defined(TUNDRA_UNIX)
    struct timeval t;
    if (0 != gettimeofday(&t, NULL))
        CroakErrno("gettimeofday failed");
    return t.tv_usec + uint64_t(t.tv_sec) * 1000000;
#elif defined(TUNDRA_WIN32)
    LARGE_INTEGER c;
    if (!QueryPerformanceCounter(&c))
        CroakErrno("QueryPerformanceCounter failed");
    return c.QuadPart;
#endif
}

double TimerToSeconds(uint64_t t)
{
#if defined(TUNDRA_UNIX)
    return t / 1000000.0;
#else
    return double(t) / s_PerfFrequency;
#endif
}

uint64_t TimerFromSeconds(double seconds)
{
#if defined(TUNDRA_UNIX)
    return seconds * 1000000.0;
#else
    return (uint64_t)(seconds * s_PerfFrequency);
#endif
}

double TimerDiffSeconds(uint64_t start, uint64_t end)
{
    return TimerToSeconds(end - start);
}

#if defined(TUNDRA_WIN32)
int LongPathToPrefixedWidePath(const char* path, wchar_t* buffer, int bufferSize)
{
    const int longPathPrefixLength = 4;
    const wchar_t longPathPrefix[longPathPrefixLength] = { L'\\', L'\\', L'?', L'\\' };

    const int wideStringLength = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    bool isAbsolute = path[1] == ':' && (path[2] == '\\' || path[2] == '/');

    if (bufferSize > longPathPrefixLength)
    {
        memcpy(buffer, longPathPrefix, sizeof(wchar_t) * longPathPrefixLength);
        buffer += longPathPrefixLength;
    }

    if (!isAbsolute)
    {
        // We cannot use Win32 API methods for converting the path from relative to absolute because those methods do not handle long paths
        const DWORD currentDirectorySize = GetCurrentDirectoryW(0, NULL);

        int requiredBufferSize = longPathPrefixLength + currentDirectorySize + wideStringLength;
        if (buffer == nullptr || bufferSize < requiredBufferSize)
            return requiredBufferSize;

        GetCurrentDirectoryW(currentDirectorySize, buffer);
        buffer[currentDirectorySize - 1] = '\\';
        buffer += currentDirectorySize;
    }
    else
    {
        if (bufferSize < longPathPrefixLength + wideStringLength)
            return longPathPrefixLength + wideStringLength;
    }

    MultiByteToWideChar(CP_UTF8, 0, path, -1, buffer, wideStringLength);
    return 0;
}
#endif

bool MakeDirectory(const char *path)
{
#if defined(TUNDRA_UNIX)
    int rc = mkdir(path, 0777);
    if (0 == rc || EEXIST == errno)
        return true;
    else
        return false;
#elif defined(TUNDRA_WIN32)
    /* pretend we can always create device roots */
    if (isalpha(path[0]) && 0 == memcmp(&path[1], ":\\\0", 3))
        return true;

    const int wideStringLength = LongPathToPrefixedWidePath(path, NULL, 0);
    wchar_t* widePath = static_cast<wchar_t*>(alloca(sizeof(wchar_t) * wideStringLength));
    LongPathToPrefixedWidePath(path, widePath, wideStringLength);

    if (!CreateDirectoryW(widePath, NULL))
    {
        switch (GetLastError())
        {
        case ERROR_ALREADY_EXISTS:
            return PathIsDirectoryA(path);
        default:
            return false;
        }
    }
    else
        return true;
#endif
}

int GetCpuCount()
{
#if defined(TUNDRA_WIN32)
    // Note: GetSystemInfo dwNumberOfProcessors is capped at 64, even if CPU has more.
    // GetActiveProcessorCount with ALL_PROCESSOR_GROUPS will return all of them.
    // Each process is still limited to only using 64 CPUs by default, so our own worker
    // threads will be running on max 64 cores, if we create more. However our own worker threads
    // are not doing "expensive work"; the expensive work is build processes launched by them.
    // So Should Be Fine.
    DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return (int)cpus;
#else
    long nprocs_max = sysconf(_SC_NPROCESSORS_CONF);
    if (nprocs_max < 0)
        CroakErrno("couldn't get CPU count");
    return (int)nprocs_max;
#endif
}

int CountTrailingZeroes(uint32_t v)
{
    v &= -int32_t(v);

    int bit_index = 32;

    if (v)
        --bit_index;

    if (v & 0x0000ffff)
        bit_index -= 16;
    if (v & 0x00ff00ff)
        bit_index -= 8;
    if (v & 0x0f0f0f0f)
        bit_index -= 4;
    if (v & 0x33333333)
        bit_index -= 2;
    if (v & 0x55555555)
        bit_index -= 1;

    return bit_index;
}

bool RemoveFileOrDir(const char *path)
{
#if defined(TUNDRA_UNIX)
    return 0 == remove(path);
#else
    FileInfo info = GetFileInfo(path);
    if (!info.Exists())
        return true;
    else if (info.IsDirectory())
        return TRUE == RemoveDirectoryA(path);
    else
        return TRUE == DeleteFileA(path);
#endif
}

// Like rename(), but also works when target file exists on Windows.
bool RenameFile(const char *oldf, const char *newf)
{
#if defined(TUNDRA_UNIX)
    return 0 == rename(oldf, newf);
#else
    return FALSE != MoveFileExA(oldf, newf, MOVEFILE_REPLACE_EXISTING);
#endif
}


