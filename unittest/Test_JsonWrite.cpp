#include "TestHarness.hpp"
#include "JsonWriter.hpp"
#include "MemAllocLinear.hpp"
#include "MemAllocHeap.hpp"

class JsonWriteTest : public ::testing::Test
{
protected:
  MemAllocHeap heap;
  MemAllocLinear alloc;
  MemAllocLinear scratch;
  char error_msg[1024];

protected:
  void SetUp() override
  {
    HeapInit(&heap);
    LinearAllocInit(&alloc, &heap, MB(1), "json alloc");
    LinearAllocInit(&scratch, &heap, MB(1), "json scratch");
  }

  void TearDown() override
  {
    LinearAllocDestroy(&scratch);
    LinearAllocDestroy(&alloc);
    HeapDestroy(&heap);
  }

};

TEST_F(JsonWriteTest, NullString)
{
    JsonWriter writer;
    JsonWriteInit(&writer, &alloc);

    JsonWriteValueString(&writer, nullptr);

    const char* result = JsonWriteToString(&writer, &alloc);
    ASSERT_STREQ("null", result);
}

