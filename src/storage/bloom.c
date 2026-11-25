#include "bloom.h"
#include "crc32c.h"
#include "record.h"

#include <string.h>

enum { PREFIX_BYTES = 8, CRC_BYTES = 4, OVERHEAD_BYTES = PREFIX_BYTES + CRC_BYTES };

bool fdb_bloom_size(size_t count, unsigned int bits_per_key, size_t *size)
{
    if (bits_per_key == 0 || bits_per_key > FDB_BLOOM_MAX_BITS_PER_KEY || count > UINT32_MAX)
        return false;
    uint64_t bytes = ((uint64_t)count * bits_per_key + 7) / 8;
    if (bytes < 8)
        bytes = 8;
    if (bytes > UINT32_MAX - OVERHEAD_BYTES || bytes > SIZE_MAX - OVERHEAD_BYTES)
        return false;
    *size = (size_t)bytes + OVERHEAD_BYTES;
    return true;
}

void fdb_bloom_init(unsigned char *image, size_t size, unsigned int bits_per_key)
{
    memset(image, 0, size);
    unsigned int probes = (bits_per_key * 69U) / 100U;
    if (probes < 1)
        probes = 1;
    if (probes > 16)
        probes = 16;
    fdb_encode_u32(image, probes);
    fdb_encode_u32(image + 4, (uint32_t)(size - OVERHEAD_BYTES));
}

static uint64_t hash_key(FastDBSlice key)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    const unsigned char *bytes = key.data;
    for (size_t i = 0; i < key.size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    return hash;
}

static uint64_t probe_step(uint64_t hash)
{
    return ((hash >> 32) | (hash << 32)) | UINT64_C(1);
}

void fdb_bloom_add(unsigned char *image, FastDBSlice key)
{
    uint32_t probes = fdb_decode_u32(image);
    uint64_t bits = (uint64_t)fdb_decode_u32(image + 4) * 8;
    uint64_t hash = hash_key(key);
    uint64_t step = probe_step(hash);
    unsigned char *body = image + PREFIX_BYTES;
    for (uint32_t i = 0; i < probes; ++i) {
        uint64_t bit = hash % bits;
        body[bit / 8] |= (unsigned char)(1U << (bit % 8));
        hash += step;
    }
}

void fdb_bloom_finish(unsigned char *image, size_t size)
{
    size_t checked = size - CRC_BYTES;
    fdb_encode_u32(image + checked, fdb_crc32c(image, checked));
}

bool fdb_bloom_decode(const unsigned char *image, size_t size, FDBBloom *out)
{
    *out = (FDBBloom){0};
    if (size < FDB_BLOOM_MIN_IMAGE_BYTES)
        return false;
    uint32_t probes = fdb_decode_u32(image);
    size_t bytes = fdb_decode_u32(image + 4);
    if (probes == 0 || probes > 16 || bytes != size - OVERHEAD_BYTES ||
        fdb_decode_u32(image + size - CRC_BYTES) != fdb_crc32c(image, size - CRC_BYTES))
        return false;
    *out = (FDBBloom){image + PREFIX_BYTES, bytes, probes};
    return true;
}

bool fdb_bloom_may_contain(const FDBBloom *filter, FastDBSlice key)
{
    uint64_t bits = (uint64_t)filter->bytes * 8;
    uint64_t hash = hash_key(key);
    uint64_t step = probe_step(hash);
    for (uint32_t i = 0; i < filter->probes; ++i) {
        uint64_t bit = hash % bits;
        if ((filter->bits[bit / 8] & (1U << (bit % 8))) == 0)
            return false;
        hash += step;
    }
    return true;
}
