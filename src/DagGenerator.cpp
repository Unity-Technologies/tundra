#include "DagGenerator.hpp"
#include "Hash.hpp"
#include "PathUtil.hpp"
#include "Exec.hpp"
#include "FileInfo.hpp"
#include "MemAllocHeap.hpp"
#include "MemAllocLinear.hpp"
#include "JsonParse.hpp"
#include "BinaryWriter.hpp"
#include "DagData.hpp"
#include "HashTable.hpp"
#include "FileSign.hpp"
#include "BuildQueue.hpp"
#include "LeafInputSignature.hpp"
#include "Actions.hpp"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <algorithm>

#ifdef _MSC_VER
#define snprintf _snprintf
#endif



static void WriteStringPtr(BinarySegment *seg, BinarySegment *str_seg, const char *text)
{
    if (text)
    {
        BinarySegmentWritePointer(seg, BinarySegmentPosition(str_seg));
        BinarySegmentWriteStringData(str_seg, text);
    }
    else
    {
        BinarySegmentWriteNullPointer(seg);
    }
}

static const char *FindStringValue(const JsonValue *obj, const char *key, const char *default_value = nullptr)
{
    if (JsonValue::kObject != obj->m_Type)
        return default_value;

    const JsonValue *node = obj->Find(key);

    if (!node)
        return default_value;

    if (JsonValue::kString != node->m_Type)
        return default_value;

    return static_cast<const JsonStringValue *>(node)->m_String;
}

static const JsonArrayValue *FindArrayValue(const JsonObjectValue *obj, const char *key)
{
    if (obj == nullptr)
        return nullptr;

    const JsonValue *node = obj->Find(key);

    if (!node)
        return nullptr;
    if (JsonValue::kArray != node->m_Type)
        return nullptr;
    return static_cast<const JsonArrayValue *>(node);
}

static const JsonObjectValue *FindObjectValue(const JsonObjectValue *obj, const char *key)
{
    const JsonValue *node = obj->Find(key);
    if (!node)
        return nullptr;
    if (JsonValue::kObject != node->m_Type)
        return nullptr;
    return static_cast<const JsonObjectValue *>(node);
}

static int64_t FindIntValue(const JsonObjectValue *obj, const char *key, int64_t def_value)
{
    const JsonValue *node = obj->Find(key);
    if (!node)
        return def_value;
    if (JsonValue::kNumber != node->m_Type)
        return def_value;
    return (int64_t) static_cast<const JsonNumberValue *>(node)->m_Number;
}

static bool WriteFileArray(
    BinarySegment *seg,
    BinarySegment *ptr_seg,
    BinarySegment *str_seg,
    const JsonArrayValue *files)
{
    if (!files || 0 == files->m_Count)
    {
        BinarySegmentWriteInt32(seg, 0);
        BinarySegmentWriteNullPointer(seg);
        return true;
    }

    BinarySegmentWriteInt32(seg, (int)files->m_Count);
    BinarySegmentWritePointer(seg, BinarySegmentPosition(ptr_seg));

    for (size_t i = 0, count = files->m_Count; i < count; ++i)
    {
        const JsonStringValue *path = files->m_Values[i]->AsString();
        if (!path)
            return false;

        PathBuffer pathbuf;
        PathInit(&pathbuf, path->m_String);

        char cleaned_path[kMaxPathLength];
        PathFormat(cleaned_path, &pathbuf);

        WriteStringPtr(ptr_seg, str_seg, cleaned_path);
        BinarySegmentWriteUint32(ptr_seg, Djb2HashPath(cleaned_path));
    }

    return true;
}

static bool EmptyArray(const JsonArrayValue *a)
{
    return nullptr == a || a->m_Count == 0;
}

struct TempNodeGuid
{
    HashDigest m_Digest;
    int32_t m_Node;

    bool operator<(const TempNodeGuid &other) const
    {
        return m_Digest < other.m_Digest;
    }
};

void WriteCommonStringPtr(BinarySegment *segment, BinarySegment *str_seg, const char *ptr, HashTable<CommonStringRecord, 0> *table, MemAllocLinear *scratch)
{
    uint32_t hash = Djb2Hash(ptr);
    CommonStringRecord *r;
    if (nullptr == (r = HashTableLookup(table, hash, ptr)))
    {
        CommonStringRecord r;
        r.m_Pointer = BinarySegmentPosition(str_seg);
        HashTableInsert(table, hash, ptr, r);
        BinarySegmentWriteStringData(str_seg, ptr);
        BinarySegmentWritePointer(segment, r.m_Pointer);
    }
    else
    {
        BinarySegmentWritePointer(segment, r->m_Pointer);
    }
}

static bool GetNodeFlagBool(const JsonObjectValue *node, const char *name, bool defaultValue = false)
{
    if (const JsonValue *val = node->Find(name))
    {
        if (const JsonBooleanValue *flag = val->AsBoolean())
        {
            return flag->m_Boolean;
        }
    }
    return defaultValue;
}

static uint32_t GetNodeFlag(const JsonObjectValue *node, const char *name, uint32_t value, bool defaultValue = false)
{
    return GetNodeFlagBool(node, name, defaultValue) ? value : 0;
}

static void EmitFileSignatures(const JsonObjectValue *json, BinarySegment *main_seg, BinarySegment *aux_seg, BinarySegment *str_seg)
{
    if (const JsonArrayValue *file_sigs = FindArrayValue(json, "FileSignatures"))
    {
        size_t count = file_sigs->m_Count;
        BinarySegmentWriteInt32(main_seg, (int)count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(aux_seg));
        for (size_t i = 0; i < count; ++i)
        {
            if (const JsonObjectValue *sig = file_sigs->m_Values[i]->AsObject())
            {
                const char *path = FindStringValue(sig, "File");

                if (!path)
                {
                    Croak("bad FileSignatures data: could not get 'File' member for object at index %zu\n", i);
                }

                int64_t timestamp = GetFileInfo(path).m_Timestamp;
                WriteStringPtr(aux_seg, str_seg, path);
                char padding[4] = {0, 0, 0, 0};
                BinarySegmentWrite(aux_seg, padding, 4);
                BinarySegmentWriteUint64(aux_seg, uint64_t(timestamp));
            }
            else
            {
                Croak("bad FileSignatures data: array entry at index %zu was not an Object\n", i);
            }
        }
    }
    else
    {
        BinarySegmentWriteInt32(main_seg, 0);
        BinarySegmentWriteNullPointer(main_seg);
    }
};

static void EmitGlobSignatures(const JsonObjectValue *json, BinarySegment *main_seg, BinarySegment *aux_seg, BinarySegment *str_seg, MemAllocHeap *heap, MemAllocLinear *scratch)
{
    if (const JsonArrayValue *glob_sigs = FindArrayValue(json, "GlobSignatures"))
    {
        size_t count = glob_sigs->m_Count;
        BinarySegmentWriteInt32(main_seg, (int)count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(aux_seg));
        for (size_t i = 0; i < count; ++i)
        {
            if (const JsonObjectValue *sig = glob_sigs->m_Values[i]->AsObject())
            {
                const char *path = FindStringValue(sig, "Path");
                if (!path)
                {
                    Croak("bad GlobSignatures data\n");
                }

                const char *filter = FindStringValue(sig, "Filter");
                bool recurse = FindIntValue(sig, "Recurse", 0) == 1;

                HashDigest digest = CalculateGlobSignatureFor(path, filter, recurse, heap, scratch);

                WriteStringPtr(aux_seg, str_seg, path);
                WriteStringPtr(aux_seg, str_seg, filter);
                BinarySegmentWrite(aux_seg, (char *)&digest, sizeof digest);
                BinarySegmentWriteInt32(aux_seg, recurse ? 1 : 0);
            }
        }
    }
    else
    {
        BinarySegmentWriteInt32(main_seg, 0);
        BinarySegmentWriteNullPointer(main_seg);
    }
}

static bool WriteNodes(
    const JsonArrayValue *nodes,
    BinarySegment *main_seg,
    BinarySegment *node_data_seg,
    BinarySegment *array2_seg,
    BinarySegment *str_seg,
    BinarySegment *writetextfile_payloads_seg,
    BinaryLocator scanner_ptrs[],
    MemAllocHeap *heap,
    HashTable<CommonStringRecord, kFlagCaseSensitive> *shared_strings,
    MemAllocLinear *scratch,
    const TempNodeGuid *order,
    const int32_t *remap_table)
{
    BinarySegmentWritePointer(main_seg, BinarySegmentPosition(node_data_seg)); // m_DagNodes

    MemAllocLinearScope scratch_scope(scratch);

    size_t node_count = nodes->m_Count;

    uint32_t *reverse_remap = (uint32_t *)HeapAllocate(heap, node_count * sizeof(uint32_t));
    for (uint32_t i = 0; i < node_count; ++i)
    {
        reverse_remap[remap_table[i]] = i;
    }

    for (size_t ni = 0; ni < node_count; ++ni)
    {
        const int32_t i = order[ni].m_Node;
        const JsonObjectValue *node = nodes->m_Values[i]->AsObject();

        const char* type = FindStringValue(node, "ActionType");
        const char *action = FindStringValue(node, "Action");
        const char *annotation = FindStringValue(node, "Annotation");
        const JsonArrayValue *toBuildDependencies = FindArrayValue(node, "ToBuildDependencies");
        if (toBuildDependencies == nullptr)
            toBuildDependencies = FindArrayValue(node, "Deps");
        const JsonArrayValue *toUseDependencies = FindArrayValue(node, "ToUseDependencies");
        const JsonArrayValue *inputs = FindArrayValue(node, "Inputs");
        const JsonArrayValue *filesThatMightBeIncluded = FindArrayValue(node, "FilesThatMightBeIncluded");
        const JsonArrayValue *outputs = FindArrayValue(node, "Outputs");
        const JsonArrayValue *output_dirs = FindArrayValue(node, "TargetDirectories");
        const JsonArrayValue *aux_outputs = FindArrayValue(node, "AuxOutputs");
        const JsonArrayValue *env_vars = FindArrayValue(node, "Env");
        const int scanner_index = (int)FindIntValue(node, "ScannerIndex", -1);
        const JsonArrayValue *shared_resources = FindArrayValue(node, "SharedResources");
        const JsonArrayValue *frontend_rsps = FindArrayValue(node, "FrontendResponseFiles");
        const JsonArrayValue *allowedOutputSubstrings = FindArrayValue(node, "AllowedOutputSubstrings");
        const JsonArrayValue *cachingInputIgnoreList = FindArrayValue(node, "CachingInputIgnoreList");
        const char *writetextfile_payload = FindStringValue(node, "WriteTextFilePayload");

        // For compatibility with DAG.json writer code which isn't emitting this field yet, we allow it to be
        // omitted and pick a sensible default.
        //
        // In the future we will still want to allow the field to be omitted for the most common type of action,
        // but we will want to get rid of 'magically detect that it is a WriteTextFile action if there is a
        // WriteTextFilePayload' and enforce that all actions other than the most common one actually specify what
        // kind of action they are.
        ActionType::Enum actionType = (writetextfile_payload != nullptr) ? ActionType::kWriteTextFile : ActionType::kRunShellCommand;
        if (type != nullptr)
            actionType = ActionType::FromString(type);

        // WriteTextFilePayload for non-WriteTextFile actions is invalid, and so is a WriteTextFile action with no WriteTextFilePayload
        if ((writetextfile_payload != nullptr) ^ (actionType == ActionType::kWriteTextFile))
            return false;

        switch (actionType) {
            case ActionType::kRunShellCommand:
                WriteStringPtr(node_data_seg, str_seg, action);
                break;
            case ActionType::kWriteTextFile:            
                WriteStringPtr(node_data_seg, writetextfile_payloads_seg, writetextfile_payload);
                break;
            case ActionType::kUnknown:
                return false;
        }

        WriteStringPtr(node_data_seg, str_seg, annotation);

        auto writeDependencyIndexList = [=](const JsonArrayValue* deps)->void{
            if (deps)
            {
                BinarySegmentAlign(array2_seg, 4);
                BinarySegmentWriteInt32(node_data_seg, (int)deps->m_Count);
                BinarySegmentWritePointer(node_data_seg, BinarySegmentPosition(array2_seg));
                for (size_t i = 0, count = deps->m_Count; i < count; ++i)
                {
                    if (const JsonNumberValue *dep_index = deps->m_Values[i]->AsNumber())
                    {
                        int index = (int)dep_index->m_Number;
                        int remapped_index = remap_table[index];
                        BinarySegmentWriteInt32(array2_seg, remapped_index);
                    }
                    else
                    {
                        Croak("dependency node index out of range for node %s.", annotation);
                    }
                }
            }
            else
            {
                BinarySegmentWriteInt32(node_data_seg, 0);
                BinarySegmentWriteNullPointer(node_data_seg);
            }
        };

        writeDependencyIndexList(toBuildDependencies);
        writeDependencyIndexList(toUseDependencies);

        WriteFileArray(node_data_seg, array2_seg, str_seg, inputs);
        WriteFileArray(node_data_seg, array2_seg, str_seg, filesThatMightBeIncluded);
        WriteFileArray(node_data_seg, array2_seg, str_seg, outputs);
        WriteFileArray(node_data_seg, array2_seg, str_seg, output_dirs);

        WriteFileArray(node_data_seg, array2_seg, str_seg, aux_outputs);
        WriteFileArray(node_data_seg, array2_seg, str_seg, frontend_rsps);

        if (allowedOutputSubstrings)
        {
            int count = allowedOutputSubstrings->m_Count;
            BinarySegmentWriteInt32(node_data_seg, count);
            BinarySegmentAlign(array2_seg, 4);
            BinarySegmentWritePointer(node_data_seg, BinarySegmentPosition(array2_seg));
            for (int i = 0; i != count; i++)
                WriteCommonStringPtr(array2_seg, str_seg, allowedOutputSubstrings->m_Values[i]->AsString()->m_String, shared_strings, scratch);
        }
        else
        {
            BinarySegmentWriteInt32(node_data_seg, 0);
            BinarySegmentWriteNullPointer(node_data_seg);
        }

        // Environment variables
        if (env_vars && env_vars->m_Count > 0)
        {
            BinarySegmentAlign(array2_seg, 4);
            BinarySegmentWriteInt32(node_data_seg, (int)env_vars->m_Count);
            BinarySegmentWritePointer(node_data_seg, BinarySegmentPosition(array2_seg));
            for (size_t i = 0, count = env_vars->m_Count; i < count; ++i)
            {
                const char *key = FindStringValue(env_vars->m_Values[i], "Key");
                const char *value = FindStringValue(env_vars->m_Values[i], "Value");

                if (!key || !value)
                    return false;

                WriteCommonStringPtr(array2_seg, str_seg, key, shared_strings, scratch);
                WriteCommonStringPtr(array2_seg, str_seg, value, shared_strings, scratch);
            }
        }
        else
        {
            BinarySegmentWriteInt32(node_data_seg, 0);
            BinarySegmentWriteNullPointer(node_data_seg);
        }

        BinarySegmentWriteInt32(node_data_seg, scanner_index);

        if (shared_resources && shared_resources->m_Count > 0)
        {
            BinarySegmentAlign(array2_seg, 4);
            BinarySegmentWriteInt32(node_data_seg, static_cast<int>(shared_resources->m_Count));
            BinarySegmentWritePointer(node_data_seg, BinarySegmentPosition(array2_seg));
            for (size_t i = 0, count = shared_resources->m_Count; i < count; ++i)
            {
                if (const JsonNumberValue *res_index = shared_resources->m_Values[i]->AsNumber())
                {
                    BinarySegmentWriteInt32(array2_seg, static_cast<int>(res_index->m_Number));
                }
                else
                {
                    return false;
                }
            }
        }
        else
        {
            BinarySegmentWriteInt32(node_data_seg, 0);
            BinarySegmentWriteNullPointer(node_data_seg);
        }

        EmitFileSignatures(node, node_data_seg, array2_seg, str_seg);
        EmitGlobSignatures(node, node_data_seg, array2_seg, str_seg, heap, scratch);

        WriteFileArray(node_data_seg, array2_seg, str_seg, cachingInputIgnoreList);

        uint32_t flags = 0;

        flags |= static_cast<uint8_t>(actionType);

        flags |= GetNodeFlag(node, "OverwriteOutputs", Frozen::DagNode::kFlagOverwriteOutputs, true);
        flags |= GetNodeFlag(node, "PreciousOutputs", Frozen::DagNode::kFlagPreciousOutputs);
        flags |= GetNodeFlag(node, "AllowUnexpectedOutput", Frozen::DagNode::kFlagAllowUnexpectedOutput, false);
        flags |= GetNodeFlag(node, "AllowUnwrittenOutputFiles", Frozen::DagNode::kFlagAllowUnwrittenOutputFiles, false);
        flags |= GetNodeFlag(node, "BanContentDigestForInputs", Frozen::DagNode::kFlagBanContentDigestForInputs, false);

        const char* cachingMode = FindStringValue(node, "CachingMode");
        if (cachingMode != nullptr)
        {
            if (0==strcmp(cachingMode, "ByLeafInputs"))
                flags |= Frozen::DagNode::kFlagCacheableByLeafInputs;
        }

        BinarySegmentWriteUint32(node_data_seg, flags);

        //write m_OriginalIndex
        BinarySegmentWriteUint32(node_data_seg, reverse_remap[ni]);

        //write dagNodeIndex
        BinarySegmentWriteUint32(node_data_seg, ni);
    }

    HeapFree(heap, reverse_remap);

    return true;
}

static bool WriteNodeArray(BinarySegment *top_seg, BinarySegment *data_seg, const JsonArrayValue *ints, const int32_t remap_table[])
{
    BinarySegmentWriteInt32(top_seg, (int)ints->m_Count);
    BinarySegmentWritePointer(top_seg, BinarySegmentPosition(data_seg));

    for (size_t i = 0, count = ints->m_Count; i < count; ++i)
    {
        if (const JsonNumberValue *num = ints->m_Values[i]->AsNumber())
        {
            int index = remap_table[(int)num->m_Number];
            BinarySegmentWriteInt32(data_seg, index);
        }
        else
            return false;
    }

    return true;
}

static bool GetBoolean(const JsonObjectValue *obj, const char *name)
{
    if (const JsonValue *val = obj->Find(name))
    {
        if (const JsonBooleanValue *b = val->AsBoolean())
        {
            return b->m_Boolean;
        }
    }

    return false;
}

static bool WriteScanner(BinaryLocator *ptr_out, BinarySegment *seg, BinarySegment *array_seg, BinarySegment *str_seg, const JsonObjectValue *data, HashTable<CommonStringRecord, kFlagCaseSensitive> *shared_strings, MemAllocLinear *scratch)
{
    if (!data)
        return false;

    const char *kind = FindStringValue(data, "Kind");
    const JsonArrayValue *incpaths = FindArrayValue(data, "IncludePaths");

    if (!kind || !incpaths)
        return false;

    BinarySegmentAlign(seg, 4);
    *ptr_out = BinarySegmentPosition(seg);

    Frozen::ScannerType::Enum type;
    if (0 == strcmp(kind, "cpp"))
        type = Frozen::ScannerType::kCpp;
    else if (0 == strcmp(kind, "generic"))
        type = Frozen::ScannerType::kGeneric;
    else
        return false;

    BinarySegmentWriteInt32(seg, type);
    BinarySegmentWriteInt32(seg, (int)incpaths->m_Count);
    BinarySegmentWritePointer(seg, BinarySegmentPosition(array_seg));
    HashState h;
    HashInit(&h);
    HashAddString(&h, kind);
    for (size_t i = 0, count = incpaths->m_Count; i < count; ++i)
    {
        const char *path = incpaths->m_Values[i]->GetString();
        if (!path)
            return false;
        HashAddPath(&h, path);
        WriteCommonStringPtr(array_seg, str_seg, path, shared_strings, scratch);
    }

    void *digest_space = BinarySegmentAlloc(seg, sizeof(HashDigest));

    if (Frozen::ScannerType::kGeneric == type)
    {
        uint32_t flags = 0;

        if (GetBoolean(data, "RequireWhitespace"))
            flags |= Frozen::GenericScannerData::kFlagRequireWhitespace;
        if (GetBoolean(data, "UseSeparators"))
            flags |= Frozen::GenericScannerData::kFlagUseSeparators;
        if (GetBoolean(data, "BareMeansSystem"))
            flags |= Frozen::GenericScannerData::kFlagBareMeansSystem;

        BinarySegmentWriteUint32(seg, flags);

        const JsonArrayValue *follow_kws = FindArrayValue(data, "Keywords");
        const JsonArrayValue *nofollow_kws = FindArrayValue(data, "KeywordsNoFollow");

        size_t kw_count =
            (follow_kws ? follow_kws->m_Count : 0) +
            (nofollow_kws ? nofollow_kws->m_Count : 0);

        BinarySegmentWriteInt32(seg, (int)kw_count);
        if (kw_count > 0)
        {
            BinarySegmentAlign(array_seg, 4);
            BinarySegmentWritePointer(seg, BinarySegmentPosition(array_seg));
            auto write_kws = [array_seg, str_seg](const JsonArrayValue *array, bool follow) -> bool {
                if (array)
                {
                    for (size_t i = 0, count = array->m_Count; i < count; ++i)
                    {
                        const JsonStringValue *value = array->m_Values[i]->AsString();
                        if (!value)
                            return false;
                        WriteStringPtr(array_seg, str_seg, value->m_String);
                        BinarySegmentWriteInt16(array_seg, (int16_t)strlen(value->m_String));
                        BinarySegmentWriteUint8(array_seg, follow ? 1 : 0);
                        BinarySegmentWriteUint8(array_seg, 0);
                    }
                }
                return true;
            };
            if (!write_kws(follow_kws, true))
                return false;
            if (!write_kws(nofollow_kws, false))
                return false;
        }
        else
        {
            BinarySegmentWriteNullPointer(seg);
        }
    }

    HashFinalize(&h, static_cast<HashDigest *>(digest_space));

    return true;
}

bool ComputeNodeGuids(const JsonArrayValue *nodes, int32_t *remap_table, TempNodeGuid *guid_table)
{
    size_t node_count = nodes->m_Count;
    for (size_t i = 0; i < node_count; ++i)
    {
        const JsonObjectValue *nobj = nodes->m_Values[i]->AsObject();

        if (!nobj)
            return false;

        guid_table[i].m_Node = (int)i;

        HashState h;
        HashInit(&h);

        const JsonArrayValue *outputs = FindArrayValue(nobj, "Outputs");
        bool didHashAnyOutputs = false;
        if (outputs)
        {
            for (size_t fi = 0, fi_count = outputs->m_Count; fi < fi_count; ++fi)
            {
                if (const JsonStringValue *str = outputs->m_Values[fi]->AsString())
                {
                    HashAddString(&h, str->m_String);
                    didHashAnyOutputs = true;
                }
            }
        }

        if (didHashAnyOutputs)
        {
            HashAddString(&h, "salt for outputs");
        }
        else
        {
            // For nodes with no outputs, preserve the legacy behaviour

            const char *action = FindStringValue(nobj, "Action");
            const JsonArrayValue *inputs = FindArrayValue(nobj, "Inputs");

            if (action && action[0])
                HashAddString(&h, action);

            if (inputs)
            {
                for (size_t fi = 0, fi_count = inputs->m_Count; fi < fi_count; ++fi)
                {
                    if (const JsonStringValue *str = inputs->m_Values[fi]->AsString())
                    {
                        HashAddString(&h, str->m_String);
                    }
                }
            }

            const char *annotation = FindStringValue(nobj, "Annotation");

            if (annotation)
                HashAddString(&h, annotation);

            if ((!action || action[0] == '\0') && !inputs && !annotation)
            {
                return false;
            }

            HashAddString(&h, "salt for legacy");
        }

        HashFinalize(&h, &guid_table[i].m_Digest);
    }

    std::sort(guid_table, guid_table + node_count);

    for (size_t i = 1; i < node_count; ++i)
    {
        if (guid_table[i - 1].m_Digest == guid_table[i].m_Digest)
        {
            int i0 = guid_table[i - 1].m_Node;
            int i1 = guid_table[i].m_Node;
            const JsonObjectValue *o0 = nodes->m_Values[i0]->AsObject();
            const JsonObjectValue *o1 = nodes->m_Values[i1]->AsObject();
            const char *anno0 = FindStringValue(o0, "Annotation");
            const char *anno1 = FindStringValue(o1, "Annotation");
            char digest[kDigestStringSize];
            DigestToString(digest, guid_table[i].m_Digest);
            Log(kError, "duplicate node guids: %s and %s share common GUID (%s)", anno0, anno1, digest);
            return false;
        }
    }

    for (size_t i = 0; i < node_count; ++i)
    {
        remap_table[guid_table[i].m_Node] = (int32_t)i;
    }

    return true;
}

bool WriteSharedResources(const JsonArrayValue *resources, BinarySegment *main_seg, BinarySegment *aux_seg, BinarySegment *aux2_seg, BinarySegment *str_seg)
{
    if (resources == nullptr || EmptyArray(resources))
    {
        BinarySegmentWriteInt32(main_seg, 0);
        BinarySegmentWriteNullPointer(main_seg);
        return true;
    }

    BinarySegmentWriteInt32(main_seg, (int)resources->m_Count);
    BinarySegmentWritePointer(main_seg, BinarySegmentPosition(aux_seg));

    for (size_t i = 0, count = resources->m_Count; i < count; ++i)
    {
        const JsonObjectValue *resource = resources->m_Values[i]->AsObject();
        if (resource == nullptr)
            return false;

        const char *annotation = FindStringValue(resource, "Annotation");
        const char *create_action = FindStringValue(resource, "CreateAction");
        const char *destroy_action = FindStringValue(resource, "DestroyAction");
        const JsonObjectValue *env = FindObjectValue(resource, "Env");

        if (annotation == nullptr)
            return false;

        BinarySegmentWritePointer(aux_seg, BinarySegmentPosition(str_seg));
        BinarySegmentWriteStringData(str_seg, annotation);

        if (create_action != nullptr)
        {
            BinarySegmentWritePointer(aux_seg, BinarySegmentPosition(str_seg));
            BinarySegmentWriteStringData(str_seg, create_action);
        }
        else
        {
            BinarySegmentWriteNullPointer(aux_seg);
        }

        if (destroy_action != nullptr)
        {
            BinarySegmentWritePointer(aux_seg, BinarySegmentPosition(str_seg));
            BinarySegmentWriteStringData(str_seg, destroy_action);
        }
        else
        {
            BinarySegmentWriteNullPointer(aux_seg);
        }

        if (env != nullptr)
        {
            BinarySegmentWriteInt32(aux_seg, env->m_Count);
            BinarySegmentWritePointer(aux_seg, BinarySegmentPosition(aux2_seg));

            for (size_t j = 0; j < env->m_Count; ++j)
            {
                if (env->m_Values[j]->AsString() == nullptr)
                    return false;

                BinarySegmentWritePointer(aux2_seg, BinarySegmentPosition(str_seg));
                BinarySegmentWriteStringData(str_seg, env->m_Names[j]);

                BinarySegmentWritePointer(aux2_seg, BinarySegmentPosition(str_seg));
                BinarySegmentWriteStringData(str_seg, env->m_Values[j]->AsString()->m_String);
            }
        }
        else
        {
            BinarySegmentWriteInt32(aux_seg, 0);
            BinarySegmentWriteNullPointer(aux_seg);
        }
    }

    return true;
}

static bool CompileDag(const JsonObjectValue *root, BinaryWriter *writer, MemAllocHeap *heap, MemAllocLinear *scratch)
{
    HashTable<CommonStringRecord, kFlagCaseSensitive> shared_strings;
    HashTableInit(&shared_strings, heap);

    BinarySegment *main_seg = BinaryWriterAddSegment(writer);
    BinarySegment *node_guid_seg = BinaryWriterAddSegment(writer);
    BinarySegment *node_data_seg = BinaryWriterAddSegment(writer);
    BinarySegment *aux_seg = BinaryWriterAddSegment(writer);
    BinarySegment *aux2_seg = BinaryWriterAddSegment(writer);
    BinarySegment *str_seg = BinaryWriterAddSegment(writer);
    BinarySegment *writetextfile_payloads_seg = BinaryWriterAddSegment(writer);

    const JsonArrayValue *nodes = FindArrayValue(root, "Nodes");
    const JsonArrayValue *directoriesCausingImplicitDependencies = FindArrayValue(root, "DirectoriesCausingImplicitDependencies");
    const JsonArrayValue *scanners = FindArrayValue(root, "Scanners");
    const JsonArrayValue *shared_resources = FindArrayValue(root, "SharedResources");
    const char *identifier = FindStringValue(root, "Identifier", "default");

    // Write scanners, store pointers
    BinaryLocator *scanner_ptrs = nullptr;

    if (!EmptyArray(scanners))
    {
        scanner_ptrs = (BinaryLocator *)alloca(sizeof(BinaryLocator) * scanners->m_Count);
        for (size_t i = 0, count = scanners->m_Count; i < count; ++i)
        {
            if (!WriteScanner(&scanner_ptrs[i], aux_seg, aux2_seg, str_seg, scanners->m_Values[i]->AsObject(), &shared_strings, scratch))
            {
                fprintf(stderr, "invalid scanner data\n");
                return false;
            }
        }
    }

    // Write magic number
    BinarySegmentWriteUint32(main_seg, Frozen::Dag::MagicNumber);

    BinarySegmentWriteUint32(main_seg, Djb2Hash(identifier));

    // Compute node guids and index remapping table.
    int32_t *remap_table = HeapAllocateArray<int32_t>(heap, nodes->m_Count);
    TempNodeGuid *guid_table = HeapAllocateArray<TempNodeGuid>(heap, nodes->m_Count);

    if (!ComputeNodeGuids(nodes, remap_table, guid_table))
        return false;

    // m_NodeCount
    size_t node_count = nodes->m_Count;
    BinarySegmentWriteInt32(main_seg, int(node_count));

    // Write node guids
    BinarySegmentWritePointer(main_seg, BinarySegmentPosition(node_guid_seg)); // m_NodeGuids
    for (size_t i = 0; i < node_count; ++i)
    {
        BinarySegmentWrite(node_guid_seg, (char *)&guid_table[i].m_Digest, sizeof guid_table[i].m_Digest);
    }

    // Write nodes.
    if (!WriteNodes(nodes, main_seg, node_data_seg, aux_seg, str_seg, writetextfile_payloads_seg, scanner_ptrs, heap, &shared_strings, scratch, guid_table, remap_table))
        return false;

    const JsonObjectValue *named_nodes = FindObjectValue(root, "NamedNodes");
    if (named_nodes)
    {
        size_t ncount = named_nodes->m_Count;
        BinarySegmentWriteInt32(main_seg, (int)ncount);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(aux2_seg));
        for (size_t i = 0; i < ncount; ++i)
        {
            WriteStringPtr(aux2_seg, str_seg, named_nodes->m_Names[i]);
            const JsonNumberValue *node_index = named_nodes->m_Values[i]->AsNumber();
            if (!node_index)
            {
                fprintf(stderr, "named node index must be number\n");
                return false;
            }
            int remapped_index = remap_table[(int)node_index->m_Number];
            BinarySegmentWriteInt32(aux2_seg, remapped_index);
        }
    }
    else
    {
        BinarySegmentWriteInt32(main_seg, 0);
        BinarySegmentWriteNullPointer(main_seg);
    }

    const JsonArrayValue *default_nodes = FindArrayValue(root, "DefaultNodes");
    if (!WriteNodeArray(main_seg, aux2_seg, default_nodes, remap_table))
    {
        fprintf(stderr, "bad DefaultNodes data\n");
        return false;
    }

    // Write shared resources
    if (!WriteSharedResources(shared_resources, main_seg, aux_seg, aux2_seg, str_seg))
        return false;

    EmitFileSignatures(root, main_seg, aux_seg, str_seg);
    EmitGlobSignatures(root, main_seg, aux_seg, str_seg, heap, scratch);

    WriteFileArray(main_seg, aux_seg, str_seg, directoriesCausingImplicitDependencies);

    if (scanner_ptrs == nullptr)
    {
        BinarySegmentWriteInt32(main_seg, 0);
        BinarySegmentWriteNullPointer(main_seg);
    } else {
        BinarySegmentWriteInt32(main_seg, scanners->m_Count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(aux_seg));
        for (int i=0; i<scanners->m_Count; i++)
        {
            BinarySegmentWritePointer(aux_seg, scanner_ptrs[i]);
        }
    }

    // Emit hashes of file extensions to sign using SHA-1 content digest instead of the normal timestamp signing.
    if (const JsonArrayValue *sha_exts = FindArrayValue(root, "ContentDigestExtensions"))
    {
        BinarySegmentWriteInt32(main_seg, (int)sha_exts->m_Count);
        BinarySegmentWritePointer(main_seg, BinarySegmentPosition(aux_seg));

        for (size_t i = 0, count = sha_exts->m_Count; i < count; ++i)
        {
            const JsonValue *v = sha_exts->m_Values[i];
            if (const JsonStringValue *sv = v->AsString())
            {
                const char *str = sv->m_String;
                if (str[0] != '.')
                {
                    fprintf(stderr, "ContentDigestExtensions: Expected extension to start with dot: %s\b", str);
                    return false;
                }

                BinarySegmentWriteUint32(aux_seg, Djb2Hash(str));
            }
            else
                return false;
        }
    }
    else
    {
        BinarySegmentWriteInt32(main_seg, 0);
        BinarySegmentWriteNullPointer(main_seg);
    }

    BinarySegmentWriteInt32(main_seg, (int)FindIntValue(root, "DaysToKeepUnreferencedNodesAround", -1));
    BinarySegmentWriteInt32(main_seg, (int)FindIntValue(root, "EmitDataForBeeWhy", 1));

    WriteStringPtr(main_seg, str_seg, FindStringValue(root, "StateFileName", ".tundra2.state"));
    WriteStringPtr(main_seg, str_seg, FindStringValue(root, "StateFileNameTmp", ".tundra2.state.tmp"));
    WriteStringPtr(main_seg, str_seg, FindStringValue(root, "StateFileNameMapped", ".tundra2.state.mapped"));

    WriteStringPtr(main_seg, str_seg, FindStringValue(root, "ScanCacheFileName", ".tundra2.scancache"));
    WriteStringPtr(main_seg, str_seg, FindStringValue(root, "ScanCacheFileNameTmp", ".tundra2.scancache.tmp"));

    WriteStringPtr(main_seg, str_seg, FindStringValue(root, "DigestCacheFileName", ".tundra2.digestcache"));
    WriteStringPtr(main_seg, str_seg, FindStringValue(root, "DigestCacheFileNameTmp", ".tundra2.digestcache.tmp"));

    WriteStringPtr(main_seg, str_seg, FindStringValue(root, "BuildTitle", "Tundra"));
    WriteStringPtr(main_seg, str_seg, FindStringValue(root, "StructuredLogFileName"));

    HashTableDestroy(&shared_strings);

    HeapFree(heap, remap_table);
    HeapFree(heap, guid_table);

    //write magic number again at the end to pretect against writing too much / too little data and not noticing.
    BinarySegmentWriteUint32(main_seg, Frozen::Dag::MagicNumber);
    return true;
}

static bool CreateDagFromJsonData(char *json_memory, const char *dag_fn)
{
    MemAllocHeap heap;
    HeapInit(&heap);

    MemAllocLinear alloc;
    MemAllocLinear scratch;

    LinearAllocInit(&alloc, &heap, MB(256), "json alloc");
    LinearAllocInit(&scratch, &heap, MB(64), "json scratch");

    char error_msg[1024];

    bool result = false;

    const JsonValue *value = JsonParse(json_memory, &alloc, &scratch, error_msg);

    if (value)
    {
        if (const JsonObjectValue *obj = value->AsObject())
        {
            if (obj->m_Count == 0)
            {
                Log(kInfo, "Nothing to do");
                exit(BuildResult::kOk);
            }

            BinaryWriter writer;
            BinaryWriterInit(&writer, &heap);

            result = CompileDag(obj, &writer, &heap, &scratch);

            result = result && BinaryWriterFlush(&writer, dag_fn);

            BinaryWriterDestroy(&writer);
        }
        else
        {
            Log(kError, "bad JSON structure");
        }
    }
    else
    {
        Log(kError, "failed to parse JSON: %s", error_msg);
    }

    LinearAllocDestroy(&scratch);
    LinearAllocDestroy(&alloc);

    HeapDestroy(&heap);
    return result;
}

bool FreezeDagJson(const char* json_filename, const char* dag_fn)
{
    FileInfo json_info = GetFileInfo(json_filename);
    if (!json_info.Exists())
    {
        Log(kError, "build script didn't generate %s", json_filename);
        return false;
    }

    size_t json_size = size_t(json_info.m_Size + 1);
    char *json_memory = (char *)malloc(json_size);
    if (!json_memory)
        Croak("couldn't allocate memory for JSON buffer");

    FILE *f = fopen(json_filename, "rb");
    if (!f)
    {
        Log(kError, "couldn't open %s for reading", json_filename);
        return false;
    }

    size_t read_count = fread(json_memory, 1, json_size - 1, f);
    if (json_size - 1 != read_count)
    {
        fclose(f);
        free(json_memory);
        Log(kError, "couldn't read JSON data (%d bytes read out of %d)",
            json_filename, (int)read_count, (int)json_size);
        return false;
    }

    fclose(f);

    json_memory[json_size - 1] = 0;

    bool success = CreateDagFromJsonData(json_memory, dag_fn);

    free(json_memory);

    return success;
}
