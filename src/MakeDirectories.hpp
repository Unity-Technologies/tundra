namespace t2
{
struct PathBuffer;
struct StatCache;

bool MakeDirectoriesRecursive(StatCache *stat_cache, const PathBuffer &dir);
bool MakeDirectoriesForFile(StatCache *stat_cache, const PathBuffer &buffer);
} // namespace t2
