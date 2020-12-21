#include "LeafInputSignatureOffline.hpp"
#include "Hash.hpp"
#include "DagData.hpp"
#include "BuildQueue.hpp"

HashDigest CalculateLeafInputHashOffline(MemAllocHeap* heap, const Frozen::Dag* dag, int nodeIndex, FILE* ingredient_stream)
{
    Buffer<int32_t> filtered_dependencies;
    BufferInit(&filtered_dependencies);

    FindDependentNodesFromRootIndex_IncludingSelf_NotRecursingIntoCacheableNodes(heap, dag, dag->m_DagNodes[nodeIndex], filtered_dependencies, nullptr);

    HashState hashState;
    HashInit(&hashState);

    HashAddString(ingredient_stream, &hashState, "requested node", dag->m_DagNodes[nodeIndex].m_Annotation.Get());

    std::sort(filtered_dependencies.begin(), filtered_dependencies.end(), [dag](const int& a, const int& b) { return strcmp(dag->m_DagNodes[a].m_Annotation.Get(), dag->m_DagNodes[b].m_Annotation.Get()) < 0; });

    for(int32_t childNodeIndex : filtered_dependencies)
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

        int relevantFlags = dagNode.m_FlagsAndActionType & ~(Frozen::DagNode::kFlagCacheableByLeafInputs | Frozen::DagNode::kFlagActionTypeMask);

        //if our flags are completely default, let's not add them to the stream, it makes the ingredient stream easier
        //to parse/compare for a human.
        if (relevantFlags != (Frozen::DagNode::kFlagOverwriteOutputs | Frozen::DagNode::kFlagAllowUnexpectedOutput))
            HashAddInteger(ingredient_stream, &hashState, "flags", relevantFlags);
    }

    BufferDestroy(&filtered_dependencies, heap);

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
