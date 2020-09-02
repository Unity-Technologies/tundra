#include "Driver.hpp"
#include "Hash.hpp"
#include "DagData.hpp"
#include "BinaryData.hpp"
#include "ScanCache.hpp"
#include "ScanData.hpp"
#include "SortedArrayUtil.hpp"

static void GetIncludesRecursive(const HashDigest &scannerGuid, const char *fn, uint32_t fnHash, const Frozen::ScanData *scan_data, int depth, HashTable<HashDigest, kFlagPathStrings> &seen, HashSet<kFlagPathStrings> &direct)
{
    if (depth == 0 && !HashSetLookup(&direct, fnHash, fn))
        HashSetInsert(&direct, fnHash, fn);

    if (HashTableLookup(&seen, fnHash, fn))
        return;
    HashTableInsert(&seen, fnHash, fn, scannerGuid);

    HashDigest scan_key;
    ComputeScanCacheKey(&scan_key, fn, scannerGuid, false);

    const int32_t count = scan_data->m_EntryCount;
    if (const HashDigest *ptr = BinarySearch(scan_data->m_Keys.Get(), count, scan_key))
    {
        int index = int(ptr - scan_data->m_Keys.Get());
        const Frozen::ScanCacheEntry *entry = scan_data->m_Data.Get() + index;
        int file_count = entry->m_IncludedFiles.GetCount();
        for (int i = 0; i < file_count; ++i)
        {
            GetIncludesRecursive(scannerGuid, entry->m_IncludedFiles[i].m_Filename.Get(), entry->m_IncludedFiles[i].m_FilenameHash, scan_data, depth + 1, seen, direct);
        }
    }
}

bool ReportIncludes(Driver *self)
{
    MemAllocLinearScope allocScope(&self->m_Allocator);

    const Frozen::Dag *dag = self->m_DagData;
    if (dag == nullptr)
    {
        Log(kError, "No build DAG data");
        return false;
    }

    const Frozen::ScanData *scan_data = self->m_ScanData;
    if (scan_data == nullptr)
    {
        Log(kError, "No build file scan data (there was no previous build done?)");
        return false;
    }

    // For each file, we have to remember which include scanner hash digest was used.
    HashTable<HashDigest, kFlagPathStrings> seen;
    HashTableInit(&seen, &self->m_Heap);
    // Which files were directly compiled in DAG? all others are included indirectly.
    HashSet<kFlagPathStrings> direct;
    HashSetInit(&direct, &self->m_Heap);

    // Crawl the DAG and include scanner data to find all direct and indirect files.
    int node_count = dag->m_NodeCount;
    for (int i = 0; i < node_count; ++i)
    {
        const Frozen::DagNode &node = dag->m_DagNodes[i];


        if (node.m_ScannerIndex != -1 && node.m_InputFiles.GetCount() > 0)
        {
            const char *fn = node.m_InputFiles[0].m_Filename.Get();
            uint32_t fnHash = node.m_InputFiles[0].m_FilenameHash;
            const Frozen::ScannerData *s = dag->m_Scanners[node.m_ScannerIndex];
            GetIncludesRecursive(s->m_ScannerGuid, fn, fnHash, scan_data, 0, seen, direct);
        }
    }

    // Create JSON structure of includes report.
    JsonWriter msg;
    JsonWriteInit(&msg, &self->m_Allocator);
    JsonWriteStartObject(&msg);

    JsonWriteKeyName(&msg, "dagFile");
    JsonWriteValueString(&msg, self->m_Options.m_DAGFileName);

    JsonWriteKeyName(&msg, "files");
    JsonWriteStartArray(&msg);
    JsonWriteNewline(&msg);

    HashTableWalk(&seen, [&](uint32_t index, uint32_t hash, const char *filename, const HashDigest &scannerguid) {
        HashDigest scan_key;
        ComputeScanCacheKey(&scan_key, filename, scannerguid, false);
        const int32_t count = scan_data->m_EntryCount;
        if (const HashDigest *ptr = BinarySearch(scan_data->m_Keys.Get(), count, scan_key))
        {
            int index = int(ptr - scan_data->m_Keys.Get());
            const Frozen::ScanCacheEntry *entry = scan_data->m_Data.Get() + index;
            int file_count = entry->m_IncludedFiles.GetCount();
            JsonWriteStartObject(&msg);
            JsonWriteKeyName(&msg, "file");
            JsonWriteValueString(&msg, filename);
            if (HashSetLookup(&direct, hash, filename))
            {
                JsonWriteKeyName(&msg, "direct");
                JsonWriteValueInteger(&msg, 1);
            }
            JsonWriteKeyName(&msg, "includes");
            JsonWriteStartArray(&msg);
            JsonWriteNewline(&msg);
            for (int i = 0; i < file_count; ++i)
            {
                const char *fn = entry->m_IncludedFiles[i].m_Filename.Get();
                JsonWriteValueString(&msg, fn);
                JsonWriteNewline(&msg);
            }
            JsonWriteEndArray(&msg);
            JsonWriteEndObject(&msg);
        }
    });

    JsonWriteEndArray(&msg);
    JsonWriteEndObject(&msg);

    // Write into file.
    FILE *f = fopen(self->m_Options.m_IncludesOutput, "w");
    if (!f)
    {
        Log(kError, "Failed to create includes report file '%s'", self->m_Options.m_IncludesOutput);
        return false;
    }
    JsonWriteToFile(&msg, f);
    fclose(f);

    HashTableDestroy(&seen);
    HashSetDestroy(&direct);

    return true;
}