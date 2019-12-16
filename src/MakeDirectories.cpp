#include "StatCache.hpp"
#include "PathUtil.hpp"


bool MakeDirectoriesRecursive(StatCache *stat_cache, const PathBuffer &dir)
{
    PathBuffer parent_dir = dir;
    PathStripLast(&parent_dir);

    // Can't go any higher.
    if (dir == parent_dir)
        return true;

    if (!MakeDirectoriesRecursive(stat_cache, parent_dir))
        return false;

    char path[kMaxPathLength];
    PathFormat(path, &dir);

    FileInfo info = StatCacheStat(stat_cache, path);


    if (info.IsDirectory())
        return true;

    if (info.IsFile())
        return false;

    Log(kSpam, "create dir \"%s\"", path);
    bool success = MakeDirectory(path);
    StatCacheMarkDirty(stat_cache, path, Djb2HashPath(path));
    return success;
}

bool MakeDirectoriesForFile(StatCache *stat_cache, const PathBuffer &buffer)
{
    PathBuffer path = buffer;
    PathStripLast(&path);
    return MakeDirectoriesRecursive(stat_cache, path);
}

