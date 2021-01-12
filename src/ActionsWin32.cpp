
#include "Actions.hpp"
#include "StatCache.hpp"
#include "BinaryData.hpp"

#if defined(TUNDRA_WIN32)

#include <fileapi.h>
#if defined (CopyFile)
#undef CopyFile
#endif

ExecResult CopyFiles(const FrozenFileAndHash* src_files, const FrozenFileAndHash* target_files, size_t files_count, StatCache* stat_cache, MemAllocHeap* heap)
{
    ExecResult result;
    memset(&result, 0, sizeof(result));
    char tmpBuffer[1024];

    for(size_t i = 0; i < files_count; ++i)
    {
        const char* src_file = src_files[i].m_Filename;
        const char* target_file = target_files[i].m_Filename;

        FileInfo src_file_info = StatCacheStat(stat_cache, src_file);
        if (!src_file_info.Exists())
        {
            result.m_ReturnCode = -1;
            snprintf(tmpBuffer, sizeof(tmpBuffer), "The source path %s does not exist.", src_file);
            break;
        }

        if (src_file_info.IsDirectory())
        {
            result.m_ReturnCode = -1;
            snprintf(tmpBuffer, sizeof(tmpBuffer), "The source path %s is a directory, which is not supported.", src_file);
            break;
        }

        FileInfo dst_file_info = StatCacheStat(stat_cache, target_file);
        if (dst_file_info.Exists())
        {
            if (dst_file_info.IsDirectory())
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The target path %s already exists as a directory.", target_file);
                break;
            }

            if (dst_file_info.IsReadOnly())
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The target path already exists and is read-only.");
                break;
            }
        }

        std::wstring src_file_wide = ToWideString(src_file);
        std::wstring target_file_wide = ToWideString(target_file);

        BOOL cancel = false;
        if (CopyFileExW(src_file_wide.c_str(), target_file_wide.c_str(), NULL, NULL, &cancel, COPY_FILE_COPY_SYMLINK) == 0)
            result.m_ReturnCode = GetLastError();

        // Mark the stat cache dirty regardless of whether we failed or not - the target file is in an unknown state now
        StatCacheMarkDirty(stat_cache, target_file, target_files[i].m_FilenameHash);

        if (result.m_ReturnCode != 0)
        {
            LPWSTR messageBuffer = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL, result.m_ReturnCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)& messageBuffer, 0, NULL);
            snprintf(tmpBuffer, sizeof(tmpBuffer), "Copying the file failed: %s", ToMultiByteUTF8String(messageBuffer).c_str());
            LocalFree(messageBuffer);
            break;
        }

        // If we copied a symbolic link, we don't need to do any more work
        if (src_file_info.IsSymlink())
            continue;

        // Force the file to have the current timestamp
        HANDLE hFile = CreateFileW(target_file_wide.c_str(), FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            result.m_ReturnCode = GetLastError();
        }
        else
        {
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            if (!SetFileTime(hFile, NULL, NULL, &now))
                result.m_ReturnCode = GetLastError();
            CloseHandle(hFile);
        }

        if (result.m_ReturnCode != 0)
        {
            LPWSTR messageBuffer = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL, result.m_ReturnCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)& messageBuffer, 0, NULL);
            snprintf(tmpBuffer, sizeof(tmpBuffer), "Updating the timestamp on the file failed: %s", ToMultiByteUTF8String(messageBuffer).c_str());
            LocalFree(messageBuffer);
            break;
        }

        if (src_file_info.IsReadOnly())
        {
            DWORD currentAttributes = GetFileAttributesW(target_file_wide.c_str());
            if (!SetFileAttributesW(target_file_wide.c_str(), currentAttributes & ~FILE_ATTRIBUTE_READONLY))
            {
                result.m_ReturnCode = GetLastError();
                LPWSTR messageBuffer = nullptr;
                FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL, result.m_ReturnCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)& messageBuffer, 0, NULL);
                snprintf(tmpBuffer, sizeof(tmpBuffer), "Clearing the readonly flag on the file failed: %s", ToMultiByteUTF8String(messageBuffer).c_str());
                LocalFree(messageBuffer);
                break;
            }
        }

        // Verify that the size of the copied file is correct
        // It's OK to use the stat cache for this as we've finished modifying the file
        dst_file_info = StatCacheStat(stat_cache, target_file);

        if (dst_file_info.m_Size != src_file_info.m_Size)
        {
            result.m_ReturnCode = -1;
            snprintf(tmpBuffer, sizeof(tmpBuffer), "The copied file is %llu bytes, but the source file was %llu bytes.", dst_file_info.m_Size, src_file_info.m_Size);
            break;
        }
    }

    if (result.m_ReturnCode != 0)
    {
        InitOutputBuffer(&result.m_OutputBuffer, heap);
        EmitOutputBytesToDestination(&result, tmpBuffer, strlen(tmpBuffer));
    }

    return result;
}

#endif
