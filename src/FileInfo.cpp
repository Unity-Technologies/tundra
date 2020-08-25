#include "FileInfo.hpp"
#include "Stats.hpp"
#include "PathUtil.hpp"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>


#if defined(TUNDRA_WIN32_MINGW)
// mingw's sys/stat.h is broken and doesn't wrap structs in the extern "C" block
extern "C"
{
#endif

#include <sys/stat.h>

#if defined(TUNDRA_WIN32_MINGW)
}
#endif

#include <errno.h>

#if defined(TUNDRA_UNIX)
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <ftw.h>
#elif defined(TUNDRA_WIN32)
#include <windows.h>
#include <shlwapi.h>
#include <filesystem>
#endif

struct StatCache;

const uint64_t kDirectoryTimestamp = 1;

FileInfo GetFileInfo(const char *path)
{
    TimingScope timing_scope(&g_Stats.m_StatCount, &g_Stats.m_StatTimeCycles);

    FileInfo result;
#if defined(TUNDRA_UNIX)
    struct stat stbuf;
#elif defined(TUNDRA_WIN32)
    struct __stat64 stbuf;
#endif

    uint32_t flags = 0;

#if defined(TUNDRA_UNIX)

    if (0 != lstat(path, &stbuf))
        goto Failure;

#elif defined(TUNDRA_WIN32)

    DWORD attrs;

    std::wstring widePath(ToWideString(path));
    //// To work around maximum path length limitations on Windows, we have to use the wide-character version of the API, with a special prefix
    if (!ConvertToLongPath(&widePath))
        goto Failure;

    if (0 != _wstat64(widePath.c_str(), &stbuf))
        goto Failure;

    attrs = GetFileAttributesW(widePath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
        goto Failure;
  

    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        flags |= FileInfo::kFlagSymlink;
#endif

    flags |= FileInfo::kFlagExists;

    if ((stbuf.st_mode & S_IFMT) == S_IFDIR)
        flags |= FileInfo::kFlagDirectory;
    else if ((stbuf.st_mode & S_IFMT) == S_IFREG)
        flags |= FileInfo::kFlagFile;
#ifdef S_IFLNK
    else if ((stbuf.st_mode & S_IFMT) == S_IFLNK)
        flags |= FileInfo::kFlagSymlink;
#endif

    result.m_Flags = flags;
    // Do not allow directories to expose real timestamps, as it's not reliable behaviour across platforms
    result.m_Timestamp = (flags & FileInfo::kFlagDirectory) ? kDirectoryTimestamp : stbuf.st_mtime;
    result.m_Size = stbuf.st_size;

    return result;

Failure:
    result.m_Flags = errno == ENOENT ? flags : FileInfo::kFlagError;
    result.m_Timestamp = 0;
    result.m_Size = 0;

    return result;
}

bool ShouldFilter(const char *name)
{
    return ShouldFilter(name, strlen(name));
}

bool ShouldFilter(const char *name, size_t len)
{
    // Filter out some common noise entries that only serve to cause DAG regeneration.

    if (1 == len && name[0] == '.')
        return true;

    if (2 == len && name[0] == '.' && name[1] == '.')
        return true;

    // Vim .foo.swp files
    if (len >= 4 && name[0] == '.' && 0 == memcmp(name + len - 4, ".swp", 4))
        return true;

    // Weed out '.tundra2.*' files too, as the .json file gets removed in between
    // regenerating, messing up glob signatures.
    static const char t2_prefix[] = ".tundra2.";
    if (len >= (sizeof t2_prefix) - 1 && 0 == memcmp(name, t2_prefix, (sizeof t2_prefix) - 1))
        return true;

    // Emacs foo~ files
    if (len > 1 && name[len - 1] == '~')
        return true;

    return false;
}

void ListDirectory(
    const char *path,
    const char *filter,
    bool recurse,
    void *user_data,
    void (*callback)(void *user_data, const FileInfo &info, const char *path))
{
#if defined(TUNDRA_UNIX)
    char full_fn[512];
    struct dirent entry;
    struct dirent *result = NULL;
    const size_t path_len = strlen(path);

    if (path_len + 1 > sizeof(full_fn))
    {
        Log(kWarning, "path too long: %s", path);
        return;
    }

    strcpy(full_fn, path);

    DIR *dir = opendir(path);

    if (!dir)
    {
        Log(kWarning, "opendir() failed: %s", path);
        return;
    }

    while (0 == readdir_r(dir, &entry, &result) && result)
    {
        size_t len = strlen(entry.d_name);

        if (ShouldFilter(entry.d_name, len))
            continue;

        bool matchesFilter = !filter || fnmatch(filter, entry.d_name, 0) == 0;

        // If we are recursing, we need to continue to find out whether this is a directory
        if (!matchesFilter && !recurse)
            continue;

        if (len + path_len + 2 >= sizeof(full_fn))
        {
            Log(kWarning, "%s: name too long\n", entry.d_name);
            continue;
        }

        full_fn[path_len] = '/';
        strcpy(full_fn + path_len + 1, entry.d_name);

        FileInfo info = GetFileInfo(full_fn);

        if (matchesFilter)
            (*callback)(user_data, info, full_fn);

        if (recurse && info.m_Flags & FileInfo::kFlagDirectory)
            ListDirectory(full_fn, filter, recurse, user_data, callback);
    }

    closedir(dir);

#else
    WIN32_FIND_DATAA find_data;
    char scan_path[MAX_PATH];

    const size_t path_length = strlen(path);
    if (path_length >= sizeof(scan_path) + 3)
    {
        Log(kWarning, "Path too long: %s", path);
        return;
    }

    memcpy(scan_path, path, path_length);
    strcpy(scan_path + path_length, "/*");

    for (int i = 0; i < MAX_PATH; ++i)
    {
        char ch = scan_path[i];
        if ('/' == ch)
            scan_path[i] = '\\';
        else if ('\0' == ch)
            break;
    }

    HANDLE h = FindFirstFileA(scan_path, &find_data);

    if (INVALID_HANDLE_VALUE == h)
    {
        Log(kWarning, "FindFirstFile() failed: %s", path);
        return;
    }

    do
    {
        if (ShouldFilter(find_data.cFileName, strlen(find_data.cFileName)))
            continue;
        bool matchesFilter = !filter || PathMatchSpec(find_data.cFileName, filter);
        if (!matchesFilter && !recurse)
            continue;

        if (path_length + strlen(find_data.cFileName) + 2 > MAX_PATH)
        {
            Log(kWarning, "Path too long: %s/%s", path, find_data.cFileName);
            continue;
        }

        static const uint64_t kEpochDiff = 0x019DB1DED53E8000LL; // 116444736000000000 nsecs
        static const uint64_t kRateDiff = 10000000;              // 100 nsecs

        uint64_t ft = uint64_t(find_data.ftLastWriteTime.dwHighDateTime) << 32 | find_data.ftLastWriteTime.dwLowDateTime;

        FileInfo info;
        info.m_Flags = FileInfo::kFlagExists;
        info.m_Size = uint64_t(find_data.nFileSizeHigh) << 32 | find_data.nFileSizeLow;
        info.m_Timestamp = (ft - kEpochDiff) / kRateDiff;

        if (FILE_ATTRIBUTE_DIRECTORY & find_data.dwFileAttributes)
        {
            info.m_Flags |= FileInfo::kFlagDirectory;
            info.m_Timestamp = kDirectoryTimestamp;
        }
        else
            info.m_Flags |= FileInfo::kFlagFile;

        strcpy(scan_path + path_length + 1, find_data.cFileName);

        if (matchesFilter)
            (*callback)(user_data, info, scan_path);

        if (recurse && info.m_Flags & FileInfo::kFlagDirectory)
            ListDirectory(scan_path, filter, recurse, user_data, callback);

    } while (FindNextFileA(h, &find_data));

    if (!FindClose(h))
        CroakErrno("couldn't close FindFile handle");
#endif
}

bool DeleteDirectory(const char* path)
{
#if TUNDRA_WIN32
    std::filesystem::path filesystempath(path);
    if (!std::filesystem::is_directory(path))
        return false;
    std::error_code error;
    int result = std::filesystem::remove_all(path, error);
    return result != -1;
#else

#if TUNDRA_APPLE
    #define FTW_STOP 1
    #define FTW_CONTINUE 0
#endif

    FileInfo fileInfo = GetFileInfo(path);
    if (!fileInfo.IsDirectory())
        return false;

    auto unlink_cb = [] (const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) -> int
    {
        return remove(fpath) == -1 ? FTW_STOP : FTW_CONTINUE;
    };

    if (FTW_STOP == nftw(path, unlink_cb, 64, FTW_DEPTH | FTW_PHYS))
        return false;
    return true;
#endif
}
