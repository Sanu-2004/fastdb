#ifndef FASTDB_BLOOM_H
#define FASTDB_BLOOM_H

#include "fastdb/fastdb.h"

enum { FDB_BLOOM_MIN_IMAGE_BYTES = 20, FDB_BLOOM_MAX_BITS_PER_KEY = 30 };

typedef struct FDBBloom {
    const unsigned char *bits;
    size_t bytes;
    uint32_t probes;
} FDBBloom;

bool fdb_bloom_size(size_t count, unsigned int bits_per_key, size_t *size);
void fdb_bloom_init(unsigned char *image, size_t size, unsigned int bits_per_key);
void fdb_bloom_add(unsigned char *image, FastDBSlice key);
void fdb_bloom_finish(unsigned char *image, size_t size);
bool fdb_bloom_decode(const unsigned char *image, size_t size, FDBBloom *out);
bool fdb_bloom_may_contain(const FDBBloom *filter, FastDBSlice key);

#endif
