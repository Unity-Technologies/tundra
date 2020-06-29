#pragma once
#include "BinaryWriter.hpp"
#include "HashTable.hpp"

namespace Frozen { struct Dag; }

struct CommonStringRecord
{
    BinaryLocator m_Pointer;
};

struct MemAllocLinear;

bool FreezeDagJson(const char* json_filename, const char* dag_filename);
void WriteCommonStringPtr(BinarySegment *segment, BinarySegment *str_seg, const char *ptr, HashTable<CommonStringRecord, 0> *table, MemAllocLinear *scratch);
bool CompileDagDerived(const Frozen::Dag* dag, MemAllocHeap* heap, MemAllocLinear* scratch, const char* dagderived_filename);
