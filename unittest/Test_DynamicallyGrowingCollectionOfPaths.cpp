#include "Common.hpp"
#include "TestHarness.hpp"
#include <cstring>
#include "DynamicallyGrowingCollectionOfPaths.hpp"
#include "MemAllocHeap.hpp"

TEST(DynamicallyGrowingCollectionOfPaths, Vanilla)
{
    MemAllocHeap heap;
    HeapInit(&heap);

    DynamicallyGrowingCollectionOfPaths mycollection;
    mycollection.Initialize(&heap);

    char buffer[100];

    //we're going to add many many strings, enough to cause the collections reallocation codepaths to be hit
    for (int i=0; i!=5000;i++)
    {
        snprintf(buffer, sizeof(buffer), "some test string %d", i);
        mycollection.Add(buffer);
    }

    //after adding all those strings, we're going to ask for all of them back and verify they match
    for (int i=0; i!=5000;i++)
    {
        snprintf(buffer, sizeof(buffer), "some test string %d", i);

        bool matches = strcmp(mycollection.Get(i), buffer) == 0;
        ASSERT_TRUE(matches);
    }

    ASSERT_EQ(5000, mycollection.Count());

    HeapDestroy(&heap);
}


