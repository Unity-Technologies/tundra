#include "LeafInputSignatureOffline.hpp"
#include "Hash.hpp"
#include "DagData.hpp"
#include "BuildQueue.hpp"

//This function calculates the offline part of the signature, which we store in the dag-derived file
HashDigest CalculateLeafInputHashOffline(const BuildQueueConfig& queueConfig, int32_t nodeIndex, MemAllocHeap* heap, FILE* ingredient_stream)
{
    auto& dag = queueConfig.m_Dag;
    auto& dagDerived = queueConfig.m_DagDerived;

    HashDigest hashResult = {};

    Buffer<int32_t> all_dependent_nodes;
    BufferInit(&all_dependent_nodes);

    std::function<bool(int,int)> filterLeafInputCacheable = [&](int parentIndex, int childIndex)
    {
        bool isCacheable = dag->m_DagNodes[childIndex].m_Flags & Frozen::DagNode::kFlagCacheableByLeafInputs;
        return !isCacheable;
    };

    FindDependentNodesFromRootIndices(heap, dag, dagDerived, &filterLeafInputCacheable, &nodeIndex, 1, all_dependent_nodes);

    HashState hashState;
    HashInit(&hashState);

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
    HashFinalize(&hashState, &hashResult);

    BufferDestroy(&all_dependent_nodes, heap);
    return hashResult;
};
