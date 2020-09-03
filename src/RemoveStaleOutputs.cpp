#include "RemoveStaleOutputs.hpp"
#include "Driver.hpp"
#include "HashTable.hpp"
#include "Stats.hpp"
#include "Profiler.hpp"
#include "AllBuiltNodes.hpp"
#include "NodeResultPrinting.hpp"

// Returns true if the path was actually cleaned up.
// Does NOT delete symlinks.
static bool CleanupPath(const char *path)
{
    FileInfo info = GetFileInfo(path);
    if (!info.Exists())
        return false;
    if (info.IsSymlink())
        return false;
#if defined(TUNDRA_UNIX)
    return 0 == remove(path);
#else
    else if (info.IsDirectory())
        return TRUE == RemoveDirectoryA(path);
    else
        return TRUE == DeleteFileA(path);
#endif
}

void RemoveStaleOutputs(Driver *self)
{
    TimingScope timing_scope(nullptr, &g_Stats.m_StaleCheckTimeCycles);
    ProfilerScope prof_scope("Tundra RemoveStaleOutputs", 0);

    const Frozen::Dag *dag = self->m_DagData;
    const Frozen::AllBuiltNodes *all_built_nodes = self->m_AllBuiltNodes;
    MemAllocLinear *scratch = &self->m_Allocator;

    MemAllocLinearScope scratch_scope(scratch);

    if (!all_built_nodes)
    {
        Log(kDebug, "unable to clean up stale output files - no previous build state");
        return;
    }

    HashSet<kFlagPathStrings> file_table;
    HashSetInit(&file_table, &self->m_Heap);
    HashSet<kFlagPathStrings> directory_table;
    HashSetInit(&directory_table, &self->m_Heap);

    // Insert all current regular and aux output files into the hash table.
    auto add_file = [&file_table](const FrozenFileAndHash &p) -> void {
        const uint32_t hash = p.m_FilenameHash;

        if (!HashSetLookup(&file_table, hash, p.m_Filename))
        {
            HashSetInsert(&file_table, hash, p.m_Filename);
        }
    };

    auto add_directory = [&directory_table](const FrozenFileAndHash &p) -> void {
        const uint32_t hash = p.m_FilenameHash;

        if (!HashSetLookup(&directory_table, hash, p.m_Filename))
        {
            HashSetInsert(&directory_table, hash, p.m_Filename);
        }
    };

    for (int i = 0, node_count = dag->m_NodeCount; i < node_count; ++i)
    {
        const Frozen::DagNode *node = dag->m_DagNodes + i;

        for (const FrozenFileAndHash &p : node->m_OutputFiles)
            add_file(p);

        for (const FrozenFileAndHash &p : node->m_AuxOutputFiles)
            add_file(p);

        for (const FrozenFileAndHash &p : node->m_OutputDirectories)
            add_directory(p);
    }

    HashSet<kFlagPathStrings> nuke_table;
    HashSetInit(&nuke_table, &self->m_Heap);

    auto add_parent_directories_to_nuke_table = [&nuke_table, &scratch](const char* path)
    {
        PathBuffer buffer;
        PathInit(&buffer, path);

        while (PathStripLast(&buffer))
        {
            if (buffer.m_SegCount == 0)
                break;

            char dir[kMaxPathLength];
            PathFormat(dir, &buffer);
            uint32_t dir_hash = Djb2HashPath(dir);

            if (!HashSetLookup(&nuke_table, dir_hash, dir))
            {
                HashSetInsert(&nuke_table, dir_hash, StrDup(scratch, dir));
            }
        }
    };

    auto startsWith = [](const char *pre, const char *str) -> bool
    {
        return strncmp(pre, str, strlen(pre)) == 0;
    };

    // Check all output files in the state if they're still around.
    // Otherwise schedule them (and all their parent dirs) for nuking.
    // We will rely on the fact that we can't rmdir() non-empty directories.
    auto check_file = [&file_table, &nuke_table, add_parent_directories_to_nuke_table, &directory_table, startsWith](const FrozenFileAndHash& fileAndHash) {
        uint32_t path_hash = fileAndHash.m_FilenameHash;
        const char* path = fileAndHash.m_Filename.Get();

        if (HashSetLookup(&file_table, path_hash, path))
            return;

        bool wasChild = false;
        HashSetWalk(&directory_table, [&](uint32_t index, uint32_t hash, const char* dir) {
            if (startsWith(dir,path))
                wasChild = true;
            });
        if (wasChild)
            return;

        if (!HashSetLookup(&nuke_table, path_hash, path))
        {
            HashSetInsert(&nuke_table, path_hash, path);
        }

        add_parent_directories_to_nuke_table(path);
    };


    HashSet<kFlagPathStrings> outputdir_nuke_table;
    HashSetInit(&outputdir_nuke_table, &self->m_Heap);

    for (int i = 0, state_count = all_built_nodes->m_NodeCount; i < state_count; ++i)
    {
        const Frozen::BuiltNode *built_node = all_built_nodes->m_BuiltNodes + i;

        if (!NodeWasUsedByThisDagPreviously(built_node, dag->m_HashedIdentifier))
            continue;

        for (const FrozenFileAndHash& fileAndHash : built_node->m_OutputFiles)
        {
            check_file(fileAndHash);
        }

        for (const FrozenFileAndHash& fileAndHash : built_node->m_AuxOutputFiles)
        {
            check_file(fileAndHash);
        }
    }

    //actually do the directory deletion
    const char* any_nuked_dir = nullptr;
    HashSetWalk(&outputdir_nuke_table, [&](uint32_t index, uint32_t hash, const char* path) {
        DeleteDirectory(path);
        any_nuked_dir = path;
    });

    // Create list of files and dirs, sort descending by path length. This sorts
    // files and subdirectories before their parent directories.
    const char **paths = LinearAllocateArray<const char *>(scratch, nuke_table.m_RecordCount);
    HashSetWalk(&nuke_table, [paths](uint32_t index, uint32_t hash, const char *str) {
        paths[index] = str;
    });

    std::sort(paths, paths + nuke_table.m_RecordCount, [](const char *l, const char *r) {
        return strlen(r) < strlen(l);
    });

    uint32_t file_nuke_count = nuke_table.m_RecordCount;
    uint64_t time_exec_started = TimerGet();
    for (uint32_t i = 0; i < file_nuke_count; ++i)
    {
        if (CleanupPath(paths[i]))
        {
            Log(kDebug, "cleaned up %s", paths[i]);
        }
    }

    uint32_t nuke_count = file_nuke_count + outputdir_nuke_table.m_RecordCount;

    if (nuke_count > 0)
    {
        char buffer[2000];
        snprintf(buffer, sizeof(buffer), "Delete %d artifact files that are no longer in use. (like %s)", nuke_count, any_nuked_dir == nullptr ? paths[0] : any_nuked_dir);
        PrintMessage(MessageStatusLevel::Success, (int)TimerDiffSeconds(time_exec_started, TimerGet()),buffer);
    }

    HashSetDestroy(&nuke_table);
    HashSetDestroy(&directory_table);
    HashSetDestroy(&outputdir_nuke_table);
    HashSetDestroy(&file_table);
}