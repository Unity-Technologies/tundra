#include "Actions.hpp"
#include "MemAllocHeap.hpp"
#include "StatCache.hpp"
#include "BinaryData.hpp"

#if defined(TUNDRA_APPLE)

#include <copyfile.h>
#include <sys/time.h>
#include <sys/stat.h>

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
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The target path %s already exists and is read-only.", target_file);
                break;
            }
        }

        copyfile_state_t state = copyfile_state_alloc();
        result.m_ReturnCode = copyfile(src_file, target_file, state, COPYFILE_ALL | COPYFILE_UNLINK | COPYFILE_CLONE | COPYFILE_DATA_SPARSE);
        copyfile_state_free(state);

        // Mark the stat cache dirty regardless of whether we failed or not - the target file is in an unknown state now
        StatCacheMarkDirty(stat_cache, target_file, target_files[i].m_FilenameHash);

        if (result.m_ReturnCode < 0)
        {
            snprintf(tmpBuffer, sizeof(tmpBuffer), "Copying the file %s failed: %s", target_file, strerror(errno));
            break;
        }

        // If we copied a symbolic link, we don't need to do any more work
        if (src_file_info.IsSymlink())
            continue;

        // Force the file to have the current timestamp
        result.m_ReturnCode = utimes(target_file, NULL);
        if (result.m_ReturnCode < 0)
        {
            snprintf(tmpBuffer, sizeof(tmpBuffer), "Updating the timestamp on the file %s failed: %s", target_file, strerror(errno));
            break;
        }

        if (src_file_info.IsReadOnly())
        {
            // The source file was readonly, so we will need to wipe the read-only bit on the target file
            struct stat dst_stat;
            result.m_ReturnCode = stat(target_file, &dst_stat);
            if (result.m_ReturnCode < 0)
            {
                snprintf(tmpBuffer, sizeof(tmpBuffer), "stat on the target file %s after the copy failed: %s", target_file, strerror(errno));
                break;
            }

            result.m_ReturnCode = chmod(target_file, (dst_stat.st_mode & 0x00007777) | S_IWUSR);
            if (result.m_ReturnCode < 0)
            {
                snprintf(tmpBuffer, sizeof(tmpBuffer), "Making the target file %s writable failed: %s", target_file, strerror(errno));
                break;
            }
        }

        // Verify that the copied file is the same size as the source.
        // It's OK to use the statcache for this now because we've finished modifying the file
        dst_file_info = StatCacheStat(stat_cache, target_file);
        if (dst_file_info.m_Size != src_file_info.m_Size)
        {
            result.m_ReturnCode = -1;
            snprintf(tmpBuffer, sizeof(tmpBuffer), "The copied file %s is %llu bytes, but the source file %s was %llu bytes.", target_file, dst_file_info.m_Size, src_file, src_file_info.m_Size);
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
