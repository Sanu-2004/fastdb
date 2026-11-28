#ifndef FASTDB_BLOCK_CACHE_H
#define FASTDB_BLOCK_CACHE_H

#include "fastdb/fastdb.h"

typedef struct FDBBlockCache FDBBlockCache;
typedef struct FDBCachedBlock FDBCachedBlock;

bool fdb_block_cache_valid_limit(size_t max_bytes);
FastDBStatus fdb_block_cache_create(size_t max_bytes, FDBBlockCache **out);
/* Destroy only after all pins and cache users have finished. */
void fdb_block_cache_destroy(FDBBlockCache *cache);
FDBCachedBlock *fdb_block_cache_acquire(FDBBlockCache *cache, uint64_t file, uint64_t offset,
                                       size_t size);
const unsigned char *fdb_cached_block_data(const FDBCachedBlock *block);
size_t fdb_cached_block_size(const FDBCachedBlock *block);
void fdb_block_cache_release(FDBBlockCache *cache, FDBCachedBlock *block);
/* Copy verified bytes. Insufficient capacity and allocation failure leave reads usable. */
void fdb_block_cache_store(FDBBlockCache *cache, uint64_t file, uint64_t offset,
                           const unsigned char *data, size_t size, size_t stored_size);
void fdb_block_cache_stats(FDBBlockCache *cache, FastDBBlockCacheStats *out);

#endif
