#pragma once;

struct PathBuffer;
struct StatCache;

bool MakeDirectoriesRecursive(StatCache *stat_cache, const PathBuffer &dir);
bool MakeDirectoriesForFile(StatCache *stat_cache, const PathBuffer &buffer);

