#include "FindNodesByName.hpp"
#include <algorithm>
#include "DagData.hpp"
#include <sstream>

static int LevenshteinDistanceNoCase(const char *s, const char *t)
{
    int n = (int)strlen(s);
    int m = (int)strlen(t);

    if (n == 0)
        return m;
    if (m == 0)
        return n;

    int xSize = n + 1;
    int ySize = m + 1;
    int *d = (int *)alloca(xSize * ySize * sizeof(int));

    for (int x = 0; x <= n; x++)
        d[ySize * x] = x;
    for (int y = 0; y <= m; y++)
        d[y] = y;

    for (int y = 1; y <= m; y++)
    {
        for (int x = 1; x <= n; x++)
        {
            if (tolower(s[x - 1]) == tolower(t[y - 1]))        // Case insensitive
                d[ySize * x + y] = d[ySize * (x - 1) + y - 1]; // no operation
            else
                d[ySize * x + y] = std::min(std::min(
                                                d[ySize * (x - 1) + y] + 1, // a deletion
                                                d[ySize * x + y - 1] + 1),  // an insertion
                                            d[ySize * (x - 1) + y - 1] + 1  // a substitution
                );
        }
    }
    return d[ySize * n + m];
}

//searching in inputs prevents useful single object builds, as the requested object gets found as an input of the linker
#define SUPPORT_SEARCHING_IN_INPUTS 0

// Match their source files and output files against the names specified.
void FindNodesByName(
    const Frozen::Dag *dag,
    Buffer<int32_t> *out_nodes,
    MemAllocHeap *heap,
    const char **names,
    size_t name_count,
    const FrozenArray<Frozen::NamedNodeData> &named_nodes)
{
    size_t node_bits_size = (dag->m_NodeCount + 31) / 32 * sizeof(uint32_t);
    uint32_t *node_bits = (uint32_t *)alloca(node_bits_size);

    memset(node_bits, 0, node_bits_size);

    for (size_t name_i = 0; name_i < name_count; ++name_i)
    {
        const char *name = names[name_i];

        bool found = false;

        // Try all named nodes first
        bool foundMatchingPrefix = false;
        bool prefixIsAmbigious = false;
        const Frozen::NamedNodeData *nodeDataForMatchingPrefix = nullptr;
        struct StringWithScore
        {
            StringWithScore(int _score, const char *_string) : score(_score), string(_string) {}
            int score;
            const char *string;
        };
        std::vector<StringWithScore> fuzzyMatches;
        fuzzyMatches.reserve(named_nodes.GetCount());
        for (const Frozen::NamedNodeData &named_node : named_nodes)
        {
            const int distance = LevenshteinDistanceNoCase(named_node.m_Name, name);
            const int fuzzyMatchLimit = std::max(0, std::min((int)strlen(name) - 2, 4));
            bool isFuzzyMatch = distance <= fuzzyMatchLimit;

            // Exact match?
            if (distance == 0)
            {
                if (strcmp(named_node.m_Name, name) != 0)
                    Log(kInfo, "found case insensitive match for %s, mapping to %s", name, named_node.m_Name.Get());

                BufferAppendOne(out_nodes, heap, named_node.m_NodeIndex);
                Log(kDebug, "mapped %s to node %d", name, named_node.m_NodeIndex);
                found = true;
                break;
            }
            // Fuzzy match?
            else if (isFuzzyMatch)
                fuzzyMatches.emplace_back(distance, named_node.m_Name.Get());
            // Prefix match?
            if (strncasecmp(named_node.m_Name, name, strlen(name)) == 0)
            {
                prefixIsAmbigious = foundMatchingPrefix;
                if (!foundMatchingPrefix)
                {
                    foundMatchingPrefix = true;
                    nodeDataForMatchingPrefix = &named_node;
                }
                if (!isFuzzyMatch)
                    fuzzyMatches.emplace_back(strlen(named_node.m_Name) - strlen(name), named_node.m_Name.Get());
            }
        }

        // If the given name is an unambigious prefix of one of our named nodes, we go with it, but warn the user.
        if (!found && foundMatchingPrefix && !prefixIsAmbigious)
        {
            Log(kWarning, "autocompleting %s to %s", name, nodeDataForMatchingPrefix->m_Name.Get());
            BufferAppendOne(out_nodes, heap, nodeDataForMatchingPrefix->m_NodeIndex);
            found = true;
        }

        if (found)
            continue;

        //since outputs in the dag are "cleaned paths", with forward slashes converted to backward ones,
        //make sure we convert our searchstring in the same way
        PathBuffer pathbuf;
        PathInit(&pathbuf, name);
        char cleaned_path[kMaxPathLength];
        PathFormat(cleaned_path, &pathbuf);

        const uint32_t filename_hash = Djb2HashPath(cleaned_path);
        for (int node_index = 0; node_index != dag->m_NodeCount; node_index++)
        {
            const Frozen::DagNode &node = dag->m_DagNodes[node_index];
            for (const FrozenFileAndHash &output : node.m_OutputFiles)
            {
                if (filename_hash == output.m_FilenameHash && 0 == PathCompare(output.m_Filename, cleaned_path))
                {
                    BufferAppendOne(out_nodes, heap, node_index);
                    Log(kDebug, "mapped %s to node %d (based on output file)", name, node_index);
                    found = true;
                    break;
                }
            }
        }

        if (!found)
        {
            std::stringstream errorOutput;
            errorOutput << "unable to map " << name << " to any named node or input/output file";
            if (!fuzzyMatches.empty())
            {
                std::sort(fuzzyMatches.begin(), fuzzyMatches.end(), [](const StringWithScore &a, const StringWithScore &b) { return a.score < b.score; });
                errorOutput << "\nmaybe you meant:\n";
                for (int i = 0; i < fuzzyMatches.size() - 1; ++i)
                    errorOutput << "- " << fuzzyMatches[i].string << "\n";
                errorOutput << "- " << fuzzyMatches[fuzzyMatches.size() - 1].string;
            }
            Croak(errorOutput.str().c_str());
        }
    }
}
