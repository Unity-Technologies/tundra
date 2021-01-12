#include <fcntl.h>

#include "Actions.hpp"
#include "MemAllocHeap.hpp"
#include "StatCache.hpp"
#include "BinaryData.hpp"
#include "MemAllocLinear.hpp"

#if defined(TUNDRA_LINUX)

#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fs.h>
#ifdef FICLONE
#include <sys/ioctl.h>
#endif

ExecResult CopyFiles(const FrozenFileAndHash* src_files, const FrozenFileAndHash* target_files, size_t files_count, StatCache* stat_cache, MemAllocHeap* heap)
{
    ExecResult result;
    memset(&result, 0, sizeof(result));
    char tmpBuffer[1024];

    int in_file = -1, out_file = -1;
    int temporary_pipe[2] = { -1, -1 };
    pipe(temporary_pipe);
    const size_t splice_chunk_size = 4096;

    MemAllocLinear scratch;
    LinearAllocInit(&scratch, heap, 4096, "CopyFiles scratch memory");

    for(size_t i = 0; i < files_count; ++i)
    {
        LinearAllocReset(&scratch);

        const char* src_file = src_files[i].m_Filename;
        const char* dst_file = target_files[i].m_Filename;

        // The StatCache is not used for the file info because we need full stat info (file modes, etc) which FileInfo doesn't give us
        struct stat src_stat;
        if (lstat(src_file, &src_stat) != 0)
        {
            result.m_ReturnCode = -1;
            snprintf(tmpBuffer, sizeof(tmpBuffer), "The properties of source file %s could not be retrieved: %s", src_file, strerror(errno));
            break;
        }

        if (S_ISDIR(src_stat.st_mode))
        {
            result.m_ReturnCode = -1;
            snprintf(tmpBuffer, sizeof(tmpBuffer), "The source path %s is a directory, which is not supported.", src_file);
            break;
        }

        FileInfo dst_file_info = StatCacheStat(stat_cache, dst_file);
        if (dst_file_info.Exists())
        {
            if (dst_file_info.IsDirectory())
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The target path %s already exists as a directory.", dst_file);
                break;
            }

            if (dst_file_info.IsReadOnly())
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The target path %s already exists and is read-only.", dst_file);
                break;
            }
        }

        if ((src_stat.st_mode & S_IFMT) == S_IFLNK)
        {
            // It's a symlink

            // Add 2 bytes, 1 for the null character and one so we can tell that the readlink return value did not get truncated
            size_t bufferSize = src_stat.st_size + 2; 
            char* link_target = static_cast<char*>(LinearAllocate(&scratch, bufferSize, 1));
            int link_size = readlink(src_file, link_target, bufferSize);
            if (link_size == bufferSize)
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The source symlink %s could not be read with a consistent size.", src_file);
                break;
            }

            if (dst_file_info.Exists())
                unlink(dst_file);

            if (symlink(link_target, dst_file) != 0)
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The target symlink %s could not be created.", dst_file);
                break;
            }

            // Mark the stat cache dirty
            StatCacheMarkDirty(stat_cache, dst_file, target_files[i].m_FilenameHash);
        }
        else
        {
            // It's a regular file
            in_file = open(src_file, O_RDONLY);
            if (in_file == -1)
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The source file %s could not be opened for reading: %s", src_file, strerror(errno));
                break;
            }

            // Ensure that the target file is opened with a writable mode, even if the input file was readonly
            out_file = open(dst_file, O_WRONLY | O_CREAT | O_TRUNC, src_stat.st_mode | S_IWUSR);
            if (out_file == -1)
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The destination file %s could not be opened for writing: %s", dst_file, strerror(errno));
                break;
            }

            do {
#ifdef FICLONE
                // Try the IOCTL. We don't particularly care about errors here, we'll just fall back if this fails
                if (ioctl(out_file, FICLONE, in_file) != -1)
                {
                    // It worked!
                    break;
                }
#endif

                // Otherwise, next fastest method is to ask the kernel to do the copy using splice
                size_t bytes_in, bytes_out;
                do
                {
                    bytes_in = splice(in_file, NULL, temporary_pipe[1], NULL, src_stat.st_blksize, 0);
                    if (bytes_in == -1)
                    {
                        result.m_ReturnCode = -1;
                        snprintf(tmpBuffer, sizeof(tmpBuffer), "Reading from the source file %s failed: %s", src_file, strerror(errno));
                        break;
                    }

                    bytes_out = splice(temporary_pipe[0], NULL, out_file, NULL, bytes_in, 0);
                    if (bytes_out == -1)
                    {
                        result.m_ReturnCode = -1;
                        snprintf(tmpBuffer, sizeof(tmpBuffer), "Writing to the destination file %s failed: %s", dst_file, strerror(errno));
                        break;
                    }
                } while (bytes_out > 0);

            } while (false);

            close(in_file);
            in_file = -1;

            close(out_file);
            out_file = -1;

            // Mark the stat cache dirty regardless of whether we failed or not - the target file is in an unknown state now
            StatCacheMarkDirty(stat_cache, dst_file, target_files[i].m_FilenameHash);

            if (result.m_ReturnCode == 0)
            {
                // Verify that the copied file is the same size as the source.
                // It's OK to use the statcache for this now because we've finished modifying the file
                dst_file_info = StatCacheStat(stat_cache, dst_file);
                if (dst_file_info.m_Size != src_stat.st_size)
                {
                    result.m_ReturnCode = -1;
                    snprintf(tmpBuffer, sizeof(tmpBuffer), "The copied file %s is %llu bytes, but the source file %s was %llu bytes.", dst_file, dst_file_info.m_Size, src_file, src_stat.st_size);
                    break;
                }
            }
        }

        if (result.m_ReturnCode < 0)
            break;
    }

    close(temporary_pipe[0]);
    close(temporary_pipe[1]);
    if (in_file != -1)
        close(in_file);
    if (out_file != -1)
        close(out_file);

    if (result.m_ReturnCode != 0)
    {
        InitOutputBuffer(&result.m_OutputBuffer, heap);
        EmitOutputBytesToDestination(&result, tmpBuffer, strlen(tmpBuffer));
    }

    LinearAllocDestroy(&scratch);

    return result;
}

#endif
