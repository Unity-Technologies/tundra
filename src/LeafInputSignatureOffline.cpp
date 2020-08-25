#include "LeafInputSignatureOffline.hpp"
#include "Hash.hpp"
#include "DagData.hpp"
#include "BuildQueue.hpp"

static HashDigest CalculateLeafInputHashOffline_Shared(const Frozen::Dag* dag,int nodeIndex, Buffer<int32_t>& all_dependent_nodes, FILE* ingredient_stream)
{
    HashState hashState;
    HashInit(&hashState);

    HashAddString(ingredient_stream, &hashState, "requested node", dag->m_DagNodes[nodeIndex].m_Annotation.Get());

    std::sort(all_dependent_nodes.begin(), all_dependent_nodes.end(), [dag](const int& a, const int& b) { return strcmp(dag->m_DagNodes[a].m_Annotation.Get(), dag->m_DagNodes[b].m_Annotation.Get()) < 0; });

    for(int32_t childNodeIndex : all_dependent_nodes)
    {
        auto& dagNode = dag->m_DagNodes[childNodeIndex];

        if (ingredient_stream)
            fprintf(ingredient_stream, "\nannotation: %s\n", dagNode.m_Annotation.Get());

        HashAddString(ingredient_stream, &hashState, "action", dagNode.m_Action.Get());

        for(auto& env: dagNode.m_EnvVars)
        {
            HashAddString(ingredient_stream, &hashState, "env_name", env.m_Name);
            HashAddString(ingredient_stream, &hashState, "env_value", env.m_Value);
        }
        for (auto& s: dagNode.m_AllowedOutputSubstrings)
            HashAddString(ingredient_stream, &hashState, "allowed_outputstring", s);
        for (auto& f: dagNode.m_OutputFiles)
            HashAddString(ingredient_stream, &hashState, "output", f.m_Filename.Get());

        int relevantFlags = dagNode.m_Flags & ~Frozen::DagNode::kFlagCacheableByLeafInputs;

        //if our flags are completely default, let's not add them to the stream, it makes the ingredient stream easier
        //to parse/compare for a human.
        if (relevantFlags != (Frozen::DagNode::kFlagOverwriteOutputs | Frozen::DagNode::kFlagAllowUnexpectedOutput))
            HashAddInteger(ingredient_stream, &hashState, "flags", relevantFlags);
    }

    HashDigest hashResult;
    HashFinalize(&hashState, &hashResult);

    if (ingredient_stream)
    {
        char digest[kDigestStringSize];
        DigestToString(digest, hashResult);
        fprintf(ingredient_stream, "Resulting Offline Hash: %s\n", digest);
    }

    return hashResult;
}

template<typename T>
static HashDigest CalculateLeafInputHashOffline_FromT(MemAllocHeap* heap, const Frozen::Dag* dag, T* thingToGetDependenciesFrom, int nodeIndex)
{
    Buffer<int32_t> all_dependent_nodes;
    BufferInit(&all_dependent_nodes);

    std::function<bool(int,int)> filterLeafInputCacheable = [&](int parentIndex, int childIndex)
    {
        bool isCacheable = dag->m_DagNodes[childIndex].m_Flags & Frozen::DagNode::kFlagCacheableByLeafInputs;
        return !isCacheable;
    };

    FindDependentNodesFromRootIndices(heap, dag, thingToGetDependenciesFrom, &filterLeafInputCacheable, &nodeIndex, 1, all_dependent_nodes);

    HashDigest result = CalculateLeafInputHashOffline_Shared(dag, nodeIndex, all_dependent_nodes, nullptr);
    BufferDestroy(&all_dependent_nodes, heap);
    return result;
}

HashDigest CalculateLeafInputHashOffline_FromDependencyBuffers(MemAllocHeap* heap, const Frozen::Dag* dag, Buffer<int32_t>* dependencyBuffers, int nodeIndex)
{
    return CalculateLeafInputHashOffline_FromT(heap, dag, dependencyBuffers, nodeIndex);
}

HashDigest CalculateLeafInputHashOffline_FromDagDerived(const Frozen::Dag* dag, const Frozen::DagDerived* dagDerived, int32_t nodeIndex, MemAllocHeap* heap, FILE* ingredient_stream)
{
    return CalculateLeafInputHashOffline_FromT(heap, dag, dagDerived, nodeIndex);
};
