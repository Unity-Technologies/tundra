#include "TestHarness.hpp"
#include "Common.hpp"



TEST(BitFuncs, PopLsb)
{
  ASSERT_EQ(32, CountTrailingZeroes(0));

  ASSERT_EQ(1, CountTrailingZeroes(2));

  ASSERT_EQ(0, CountTrailingZeroes(1));

  ASSERT_EQ(0, CountTrailingZeroes(0xffffffff));
  ASSERT_EQ(1, CountTrailingZeroes(0xfffffffe));
  ASSERT_EQ(31, CountTrailingZeroes(0x80000000));
}
