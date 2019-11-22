#include "StatCache.hpp"
#include "PathUtil.hpp"

namespace t2 {
bool MakeDirectoriesRecursive(StatCache* stat_cache, const PathBuffer& dir)
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

  if (info.Exists())
  {
    // Just assume this is a directory. We could check it - but there's currently no way via _stat64() calls
    // on Windows to check if a file is a symbolic link (to a directory).
    return true;
  }
  else
  {
    Log(kSpam, "create dir \"%s\"", path);
    bool success = MakeDirectory(path);
    StatCacheMarkDirty(stat_cache, path, Djb2HashPath(path));
    return success;
  }
}

bool MakeDirectoriesForFile(StatCache* stat_cache, const PathBuffer& buffer)
{
  PathBuffer path = buffer;
  PathStripLast(&path);
  return MakeDirectoriesRecursive(stat_cache, path);
}
}