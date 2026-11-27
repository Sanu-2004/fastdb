#include "block_cache.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct FDBCachedBlock {
    FDBCachedBlock *older;
    FDBCachedBlock *newer;
    FDBCachedBlock *hash_next;
    uint64_t file;
    uint64_t offset;
    size_t size;
    size_t pins;
    unsigned char data[];
};

struct FDBBlockCache {
    pthread_mutex_t mutex;
    FastDBBlockCacheStats stats;
    size_t fixed_bytes;
    size_t pinned_bytes;
    size_t bucket_count;
    FDBCachedBlock *oldest;
    FDBCachedBlock *newest;
    FDBCachedBlock *buckets[];
};

static size_t bucket_count(size_t capacity)
{
    size_t buckets = capacity / 1024;
    if (buckets == 0)
        return 1;
    return buckets > 65536 ? 65536 : buckets;
}

static size_t fixed_bytes(size_t capacity)
{
    return sizeof(FDBBlockCache) + bucket_count(capacity) * sizeof(FDBCachedBlock *);
}

bool fdb_block_cache_valid_limit(size_t max_bytes)
{
    return max_bytes > fixed_bytes(max_bytes) + sizeof(FDBCachedBlock);
}

FastDBStatus fdb_block_cache_create(size_t max_bytes, FDBBlockCache **out)
{
    *out = NULL;
    if (max_bytes == 0)
        return FASTDB_OK;
    if (!fdb_block_cache_valid_limit(max_bytes))
        return FASTDB_INVALID_ARGUMENT;
    size_t bytes = fixed_bytes(max_bytes);
    FDBBlockCache *cache = calloc(1, bytes);
    if (cache == NULL)
        return FASTDB_OUT_OF_MEMORY;
    int error = pthread_mutex_init(&cache->mutex, NULL);
    if (error != 0) {
        free(cache);
        return error == ENOMEM ? FASTDB_OUT_OF_MEMORY : FASTDB_IO_ERROR;
    }
    cache->stats.max_bytes = max_bytes;
    cache->stats.memory_bytes = bytes;
    cache->fixed_bytes = bytes;
    cache->bucket_count = bucket_count(max_bytes);
    *out = cache;
    return FASTDB_OK;
}

void fdb_block_cache_destroy(FDBBlockCache *cache)
{
    if (cache == NULL)
        return;
    FDBCachedBlock *block = cache->oldest;
    while (block != NULL) {
        FDBCachedBlock *next = block->newer;
        free(block);
        block = next;
    }
    pthread_mutex_destroy(&cache->mutex);
    free(cache);
}

static FDBCachedBlock **bucket(FDBBlockCache *cache, uint64_t file, uint64_t offset)
{
    uint64_t hash = offset ^ (file * UINT64_C(0x9e3779b97f4a7c15));
    hash = (hash ^ (hash >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    hash = (hash ^ (hash >> 27)) * UINT64_C(0x94d049bb133111eb);
    hash ^= hash >> 31;
    return &cache->buckets[hash % cache->bucket_count];
}

static FDBCachedBlock **find_link(FDBBlockCache *cache, uint64_t file, uint64_t offset)
{
    FDBCachedBlock **link = bucket(cache, file, offset);
    while (*link != NULL && ((*link)->file != file || (*link)->offset != offset))
        link = &(*link)->hash_next;
    return link;
}

static void unlink_recency(FDBBlockCache *cache, FDBCachedBlock *block)
{
    if (block->older == NULL)
        cache->oldest = block->newer;
    else
        block->older->newer = block->newer;
    if (block->newer == NULL)
        cache->newest = block->older;
    else
        block->newer->older = block->older;
}

static void append_newest(FDBBlockCache *cache, FDBCachedBlock *block)
{
    block->older = cache->newest;
    block->newer = NULL;
    if (cache->newest == NULL)
        cache->oldest = block;
    else
        cache->newest->newer = block;
    cache->newest = block;
}

static void increment(uint64_t *counter)
{
    if (*counter < UINT64_MAX)
        ++*counter;
}

FDBCachedBlock *fdb_block_cache_acquire(FDBBlockCache *cache, uint64_t file, uint64_t offset,
                                       size_t size)
{
    pthread_mutex_lock(&cache->mutex);
    FDBCachedBlock *block = *find_link(cache, file, offset);
    if (block == NULL || block->size != size || block->pins == SIZE_MAX) {
        increment(&cache->stats.misses);
        block = NULL;
    } else {
        increment(&cache->stats.hits);
        if (block->pins++ == 0)
            cache->pinned_bytes += sizeof(*block) + block->size;
        unlink_recency(cache, block);
        append_newest(cache, block);
    }
    pthread_mutex_unlock(&cache->mutex);
    return block;
}

const unsigned char *fdb_cached_block_data(const FDBCachedBlock *block)
{
    return block->data;
}

void fdb_block_cache_release(FDBBlockCache *cache, FDBCachedBlock *block)
{
    if (block == NULL)
        return;
    pthread_mutex_lock(&cache->mutex);
    if (--block->pins == 0)
        cache->pinned_bytes -= sizeof(*block) + block->size;
    pthread_mutex_unlock(&cache->mutex);
}

static void evict(FDBBlockCache *cache, FDBCachedBlock *block)
{
    *find_link(cache, block->file, block->offset) = block->hash_next;
    unlink_recency(cache, block);
    cache->stats.memory_bytes -= sizeof(*block) + block->size;
    --cache->stats.entries;
    increment(&cache->stats.evictions);
    free(block);
}

static void insert(FDBBlockCache *cache, uint64_t file, uint64_t offset,
                   const unsigned char *data, size_t size)
{
    size_t available = cache->stats.max_bytes - cache->fixed_bytes - cache->pinned_bytes;
    if (available < sizeof(FDBCachedBlock) || size > available - sizeof(FDBCachedBlock) ||
        *find_link(cache, file, offset) != NULL)
        return;
    size_t charge = sizeof(FDBCachedBlock) + size;
    FDBCachedBlock *block = cache->oldest;
    while (charge > cache->stats.max_bytes - cache->stats.memory_bytes) {
        FDBCachedBlock *next = block->newer;
        if (block->pins == 0)
            evict(cache, block);
        block = next;
    }

    block = malloc(charge);
    if (block == NULL)
        return;
    FDBCachedBlock **chain = bucket(cache, file, offset);
    *block = (FDBCachedBlock){.hash_next = *chain, .file = file, .offset = offset, .size = size};
    memcpy(block->data, data, size);
    *chain = block;
    append_newest(cache, block);
    cache->stats.memory_bytes += charge;
    ++cache->stats.entries;
}

void fdb_block_cache_store(FDBBlockCache *cache, uint64_t file, uint64_t offset,
                           const unsigned char *data, size_t size)
{
    pthread_mutex_lock(&cache->mutex);
    insert(cache, file, offset, data, size);
    pthread_mutex_unlock(&cache->mutex);
}

void fdb_block_cache_stats(FDBBlockCache *cache, FastDBBlockCacheStats *out)
{
    if (cache == NULL) {
        *out = (FastDBBlockCacheStats){0};
        return;
    }
    pthread_mutex_lock(&cache->mutex);
    *out = cache->stats;
    pthread_mutex_unlock(&cache->mutex);
}
