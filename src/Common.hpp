#pragma once

#include "Config.hpp"

#include <cstddef>
#include <stdint.h>

// Allow the use of alloca() everywhere
#if defined(_MSC_VER)
#include <malloc.h>
#include <intrin.h>
#include <string>
#include <wchar.h>
#elif defined(TUNDRA_WIN32_MINGW)
#include <malloc.h>
#elif defined(TUNDRA_UNIX)
#if defined(TUNDRA_FREEBSD) || defined(TUNDRA_NETBSD) || defined(TUNDRA_OPENBSD)
#include <stdlib.h>
#else
#include <alloca.h>
#endif
#endif

#define MB(n) ((n)*1024 * 1024)
#define KB(n) ((n)*1024)

#define ARRAY_SIZE(a) (sizeof((a)) / sizeof((a)[0]))

#if ENABLED(CHECKED_BUILD)
#define CHECK(expr)                                                                  \
    do                                                                               \
    {                                                                                \
        if (!(expr))                                                                 \
            ::CroakAbort("%s(%d): check failure %s", __FILE__, __LINE__, #expr); \
    } while (0)
#else
#define CHECK(expr) \
    do              \
    {               \
    } while (0)
#endif

#define TD_ALIGN(v, alignment) (((v) + (alignment)-1) & ~((alignment)-1))

#if TUNDRA_WIN32

// Convert a string to a wide string. This is done as a macro so that we can use alloca() to allocate temporary memory for
// the converted string. dstLength will be set to the length of the string in characters, including null terminator.
#define CONVERT_TO_WIDE_PATH_ON_STACK(src, dst, dstLength) do { \
	const size_t mbcount = strlen(src) + 1; \
	dstLength = MultiByteToWideChar(CP_UTF8, 0, src, mbcount, NULL, 0); \
	dst = static_cast<wchar_t*>(_alloca(dstLength * sizeof(wchar_t))); \
	MultiByteToWideChar(CP_UTF8, 0, src, mbcount, dst, dstLength); \
} while (0)

// Convert a path to a long path, with appropriate special prefixes, if needed.
// Parameters:
// * input: the input string
// * inputLength: the length of the input string in characters, including the null terminator.
// * output: a pointer to a pointer to a buffer for storing the converted string. This should not be NULL,
//           but the pointer it points to can be NULL, in which case this function will
//             a) attempt to just assign the input to the output, if it believes the string is short enough, or
//             b) calculate the size of buffer required.
// * outputLength: a pointer to the length of the output buffer, in characters, including space for the null terminator.
//                 This should not be NULL. If the output buffer was NULL, or the size of the buffer is too small to hold
//                 the output, then when the function returns the variable this points to will have been updated to give
//                 the actual buffer size required. Note that the final string may be slightly smaller than the buffer.
// Returns:
// * true, if the path was converted successfully (or no conversion was needed); the buffer pointed to by output
//         is a valid string to use; outputLength is the length of the string (NOT including null terminator).
// * false, if no output buffer was provided or the buffer was too small, or there was an error. If outputLength
//          is zero in this situation it means there was an error; otherwise, allocate a buffer of the given size
//          and try again.
bool ConvertToLongPath(wchar_t* input, const size_t inputLength, wchar_t** output, size_t* outputLength);
#endif


void InitCommon(void);

//-----------------------------------------------------------------------------
// Error handling
//-----------------------------------------------------------------------------

// Print the most recent OS error (errno, and GetLastError on Windows)
void PrintErrno();

// Terminate the program with an error message on stderr
void NORETURN Croak(const char *fmt, ...);

// Terminate the program with an error message on stderr, also printing the errno/GetLastError() status
void NORETURN CroakErrno(const char *fmt, ...);

// Abort the program with an error message on stderr
void NORETURN CroakAbort(const char *fmt, ...);

// Abort the program with an error message on stderr, also printing the errno/GetLastError() status
void NORETURN CroakErrnoAbort(const char *fmt, ...);

//-----------------------------------------------------------------------------
// Logging
//-----------------------------------------------------------------------------

enum LogLevel
{
    kError = 1 << 0,
    kWarning = 1 << 1,
    kInfo = 1 << 2,
    kDebug = 1 << 3,
    kSpam = 1 << 4
};

int GetLogFlags();

void SetLogFlags(int log_level);

void Log(LogLevel level, const char *fmt, ...);

struct JsonWriter;

void SetStructuredLogFileName(const char *path);
bool IsStructuredLogActive();
void LogStructured(JsonWriter *writer);

//-----------------------------------------------------------------------------
// String hashing
//-----------------------------------------------------------------------------

inline int FoldCase(int c)
{
    // This generates branch-free code on GCC, Clang and MSVC
    unsigned int x = (unsigned int)c - 'A';
    int d = c + 0x20;
    return (x < 26 ? d : c);
}

// Compute 32-bit DJB-2 hash of a string.
uint32_t Djb2Hash(const char *str);

// Compute 32-bit DJB-2 hash of a string, treating ASCII A-Z as a-z.
uint32_t Djb2HashNoCase(const char *str);

// Compute 64-bit DJB-2 hash of a string.
uint64_t Djb2Hash64(const char *str);

// Compute 64-bit DJB-2 hash of a string, treating ASCII A-Z as a-z.
uint64_t Djb2HashNoCase64(const char *str);

// Compute 32-bit DJB-2 hash of a path string (ignoring case if appropriate).
#if ENABLED(TUNDRA_CASE_INSENSITIVE_FILESYSTEM)
inline uint32_t Djb2HashPath(const char *str)
{
    return Djb2HashNoCase(str);
}
inline uint64_t Djb2HashPath64(const char *str) { return Djb2HashNoCase64(str); }
#else
inline uint32_t Djb2HashPath(const char *str)
{
    return Djb2Hash(str);
}
inline uint64_t Djb2HashPath64(const char *str) { return Djb2Hash64(str); }
#endif

//-----------------------------------------------------------------------------
// Directories
//-----------------------------------------------------------------------------

void GetCwd(char *buffer, size_t buffer_size);
bool SetCwd(const char *dir);
bool MakeDirectory(const char *dir);

bool RemoveFileOrDir(const char *path);

// Like rename(), but also works when target file exists on Windows.
bool RenameFile(const char *oldf, const char *newf);

//-----------------------------------------------------------------------------
// Misc
//-----------------------------------------------------------------------------

uint32_t NextPowerOfTwo(uint32_t val);

uint64_t TimerGet();
double TimerToSeconds(uint64_t start);
uint64_t TimerFromSeconds(double seconds);
double TimerDiffSeconds(uint64_t start, uint64_t end);

int GetCpuCount();

int CountTrailingZeroes(uint32_t word);

#if ENABLED(USE_LITTLE_ENDIAN)

inline uint32_t LoadBigEndian32(uint32_t v)
{
#if defined(__GNUC__)
    return __builtin_bswap32(v);
#elif defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
#error unsupported compiler
#endif
}

inline uint64_t LoadBigEndian64(uint64_t v)
{
#if defined(__GNUC__)
    return __builtin_bswap64(v);
#elif defined(_MSC_VER)
    return _byteswap_uint64(v);
#else
#error unsupported compiler
#endif
}

#else

inline uint32_t LoadBigEndian32(uint32_t v)
{
    return v;
}

inline uint64_t LoadBigEndian64(uint64_t v)
{
    return v;
}

#endif

//-----------------------------------------------------------------------------

struct FileAndHash
{
    const char *m_Filename;
    uint32_t m_FilenameHash;
};
