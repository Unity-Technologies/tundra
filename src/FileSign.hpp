#pragma once

#include "Common.hpp"
#include "Hash.hpp"

struct HashState;
struct StatCache;
struct DigestCache;
struct MemAllocHeap;
struct MemAllocLinear;

HashDigest ComputeFileSignatureSha1(StatCache* stat_cache, DigestCache* digest_cache, const char* filename, uint32_t fn_hash);
void ComputeFileSignatureSha1(HashState *state, StatCache *stat_cache, DigestCache *digest_cache, const char *filename, uint32_t fn_hash);
HashDigest CalculateGlobSignatureFor(const char *path, const char *filter, bool recurse, MemAllocHeap *heap, MemAllocLinear *scratch);

bool ShouldUseSHA1SignatureFor(const char *filename, const uint32_t sha_extension_hashes[], int sha_extension_hash_count);
