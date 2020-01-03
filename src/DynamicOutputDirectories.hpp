#pragma once

struct SinglyLinkedPathListEntry
{
    SinglyLinkedPathListEntry* next;
    const char* path;
};

struct SinglyLinkedPathList
{
    SinglyLinkedPathListEntry* head;
    int count;
};

void InitializeDynamicOutputDirectories(int workerThreadCount);
SinglyLinkedPathList* AllocateEmptyPathList(int threadIndex);
void AppendDirectoryListingToList(const char* directoryToList, int threadIndex, SinglyLinkedPathList& appendToList);
void DestroyDynamicOutputDirectories();
