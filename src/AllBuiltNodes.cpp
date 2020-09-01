#include "AllBuiltNodes.hpp"
#include "StatCache.hpp"

bool OutputFilesMissingFor(const Frozen::BuiltNode* builtNode, StatCache *stat_cache)
{
    for (const FrozenFileAndHash &f : builtNode->m_OutputFiles)
    {
        FileInfo i = StatCacheStat(stat_cache, f.m_Filename, f.m_FilenameHash);

        if (!i.Exists())
            return true;
    }

    return false;
}
