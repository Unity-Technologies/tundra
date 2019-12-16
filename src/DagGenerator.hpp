#pragma once
#include "BinaryWriter.hpp"
#include "HashTable.hpp"

struct CommonStringRecord
{
    BinaryLocator m_Pointer;
};

struct MemAllocLinear;

bool GenerateDag(const char *build_file, const char *dag_fn);
void WriteCommonStringPtr(BinarySegment *segment, BinarySegment *str_seg, const char *ptr, HashTable<CommonStringRecord, 0> *table, MemAllocLinear *scratch);
