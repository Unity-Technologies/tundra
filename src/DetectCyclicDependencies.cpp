#include "DagData.hpp"
#include "DetectCyclicDependencies.hpp"

//This file is an implementation of the algorithm described in https://www.youtube.com/watch?v=rKQaZuoUR4M

struct VisitState
{
    enum State
    {
        NotYetVisited,
        BeingVisited,
        GuaranteedNoCycles
    };
};

struct CycleDetector
{
    //stores the state/color of each node
    Buffer<VisitState::State> m_State;

    //bookkeeping so that when we do find a cycle, we can backtrack our steps to figure out the exact path
    Buffer<int32_t> m_ScheduledByWho;

    int m_NodeCount;
    MemAllocHeap* m_Heap;
    const Frozen::Dag* m_Dag;

public:
    CycleDetector(const Frozen::Dag* dag, MemAllocHeap* heap)
    {
        m_Dag = dag;
        m_Heap = heap;
        m_NodeCount = m_Dag->m_NodeCount;
        BufferInitWithCapacity(&m_State, m_Heap, m_NodeCount);
        BufferInitWithCapacity(&m_ScheduledByWho, m_Heap, m_NodeCount);
        for (int i=0; i!=m_NodeCount;i++)
        {
            m_State[i] = VisitState::NotYetVisited;
            m_ScheduledByWho[i] = -1;
        }
    }

    ~CycleDetector()
    {
        BufferDestroy(&m_State, m_Heap);
        BufferDestroy(&m_ScheduledByWho, m_Heap);
    }

    bool Detect()
    {
        while(true)
        {
            int root = FindUnvisitedNode();
            if (root == -1)
            {
                return false;
            }
            if (DepthFirstSearch(root))
                return true;
        }
    }

private:
    bool DepthFirstSearch(int node)
    {
        auto visitDependencies = [&](const FrozenArray<int32_t>& deps) -> bool
        {
            for (auto dep: deps)
            {
                int depState = m_State[dep];
                if (VisitState::GuaranteedNoCycles == depState)
                    continue;

                if (VisitState::BeingVisited == depState)
                {
                    PrintCycleFor(node, dep);
                    return true;
                }

                m_ScheduledByWho[dep] = node;
                if (DepthFirstSearch(dep))
                    return true;
            }
            return false;
        };

        m_State[node] = VisitState::BeingVisited;
        if (visitDependencies(m_Dag->m_DagNodes[node].m_ToBuildDependencies))
            return true;
        if (visitDependencies(m_Dag->m_DagNodes[node].m_ToUseDependencies))
            return true;
        m_State[node] = VisitState::GuaranteedNoCycles;
        return false;
    }

    int FindUnvisitedNode()
    {
        for (int i=0; i!=m_NodeCount;i++)
            if (m_State[i] == VisitState::NotYetVisited)
                return i;
        return -1;
    }

    const char* NameFor(int node)
    {
        return m_Dag->m_DagNodes[node].m_Annotation.Get();
    }

    void PrintCycleFor(int nodeReachingBack, int nodeBeingReachBackTo)
    {
        //ok, we know there's a cycle, now we just need to walk back the breadcrums in m_ScheduledByWho
        //to figure out the exact path. We cannot print out the path as we walk it back, because then
        //we'd print it out in the inverse order to what a human expects to read for a cycle.
        //so first thing we're going to do is just walk the breadcrumbs back, and add each one to a list:

        int cursor = nodeReachingBack;
        Buffer<uint32_t> exactPathOfCycle;
        BufferInitWithCapacity(&exactPathOfCycle, m_Heap, 100);
        BufferAppendOne(&exactPathOfCycle, m_Heap, nodeBeingReachBackTo);
        while(true)
        {
            BufferAppendOne(&exactPathOfCycle, m_Heap, cursor);
            if (cursor == nodeBeingReachBackTo)
                break;
            cursor = m_ScheduledByWho[cursor];
        }

        //Ok, now we have the exact path of the cycle, and we can print it out in reverse order:
        int indent = 0;
        printf("There is a cycle in the graph produced by your buildprogram:\n");
        for (int i=exactPathOfCycle.m_Size-1;i>=0;i--)
        {
            PrintIndent(indent += 2);
            printf("`%s`",NameFor(exactPathOfCycle[i]));

            if (i != 0)
              printf(" which depends on\n");
        }
        printf("\n\n");
        BufferDestroy(&exactPathOfCycle, m_Heap);
    }

    void PrintIndent(int amount)
    {
        for (int i=0; i!=amount; i++)
            printf(" ");
    }
};

//returns true if a cycle was found
bool DetectCyclicDependencies(const Frozen::Dag* dag, MemAllocHeap* heap)
{
    CycleDetector implementation(dag, heap);
    return implementation.Detect();
}
