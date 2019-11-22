#include "Common.hpp"
#include "TestHarness.hpp"
#include <cstring>



TEST(Djb2, Vanilla)
{
  ASSERT_EQ(3007235198, Djb2Hash("FooBar"));
}

TEST(Djb2, NoCase)
{
  ASSERT_EQ(Djb2Hash("foobar"), Djb2HashNoCase("FooBar"));
  ASSERT_EQ(Djb2Hash("foobar"), Djb2HashNoCase("foobar"));
}

