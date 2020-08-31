#pragma once
#include "BinaryWriter.hpp"
#include "HashTable.hpp"

struct CommonStringRecord
{
    BinaryLocator m_Pointer;
};

struct MemAllocLinear;

bool FreezeDagJson(const char* json_filename, const char* dag_filename);
void WriteCommonStringPtr(BinarySegment *segment, BinarySegment *str_seg, const char *ptr, HashTable<CommonStringRecord, 0> *table, MemAllocLinear *scratch);
