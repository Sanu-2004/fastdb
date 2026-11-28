#include "block_cache.h"
#include "check.h"

#include <string.h>

static bool fail_malloc;

void *__real_malloc(size_t size);
void *__wrap_malloc(size_t size);

void *__wrap_malloc(size_t size)
{
    if (fail_malloc) {
        fail_malloc = false;
        return NULL;
    }
    return __real_malloc(size);
}

int main(void)
{
    FDBBlockCache *cache;
    unsigned char first[80] = {1}, second[80] = {2}, third[80] = {3}, fourth[80] = {4};
    CHECK(!fdb_block_cache_valid_limit(1));
    OK(fdb_block_cache_create(512, &cache));
    FastDBBlockCacheStats stats;
    fdb_block_cache_stats(cache, &stats);
    size_t base = stats.memory_bytes;
    fdb_block_cache_store(cache, 1, 32, first, sizeof(first), sizeof(first));
    fdb_block_cache_stats(cache, &stats);
    CHECK(stats.entries == 1 && stats.memory_bytes > base);
    size_t cap = base + 2 * (stats.memory_bytes - base);
    fdb_block_cache_destroy(cache);
    OK(fdb_block_cache_create(cap, &cache));

    fail_malloc = true;
    fdb_block_cache_store(cache, 1, 32, first, sizeof(first), sizeof(first));
    CHECK(!fail_malloc);
    fdb_block_cache_stats(cache, &stats);
    CHECK(stats.entries == 0 && stats.memory_bytes == base);
    fdb_block_cache_store(cache, 1, 32, first, sizeof(first), sizeof(first));
    fdb_block_cache_store(cache, 1, 96, second, sizeof(second), sizeof(second));
    FDBCachedBlock *block = fdb_block_cache_acquire(cache, 1, 32, sizeof(first));
    CHECK(block != NULL);
    fdb_block_cache_release(cache, block);
    CHECK(fdb_block_cache_acquire(cache, 1, 32, sizeof(first) - 1) == NULL);
    fdb_block_cache_store(cache, 2, 32, third, sizeof(third), sizeof(third));
    CHECK(fdb_block_cache_acquire(cache, 1, 96, sizeof(second)) == NULL);
    fdb_block_cache_stats(cache, &stats);
    CHECK(stats.hits == 1 && stats.misses == 2 && stats.evictions == 1 &&
          stats.entries == 2 && stats.memory_bytes == cap && stats.max_bytes == cap);

    block = fdb_block_cache_acquire(cache, 1, 32, sizeof(first));
    CHECK(block != NULL && fdb_block_cache_acquire(cache, 1, 32, sizeof(first)) == block);
    FDBCachedBlock *other = fdb_block_cache_acquire(cache, 2, 32, sizeof(third));
    CHECK(other != NULL && memcmp(fdb_cached_block_data(other), third, sizeof(third)) == 0);
    fdb_block_cache_store(cache, 1, 160, fourth, sizeof(fourth), sizeof(fourth));
    CHECK(fdb_block_cache_acquire(cache, 1, 160, sizeof(fourth)) == NULL);
    fdb_block_cache_release(cache, block);
    fdb_block_cache_store(cache, 1, 160, fourth, sizeof(fourth), sizeof(fourth));
    CHECK(fdb_block_cache_acquire(cache, 1, 160, sizeof(fourth)) == NULL);
    CHECK(memcmp(fdb_cached_block_data(block), first, sizeof(first)) == 0);
    fdb_block_cache_release(cache, block);
    fdb_block_cache_store(cache, 1, 160, fourth, sizeof(fourth), sizeof(fourth));
    block = fdb_block_cache_acquire(cache, 1, 160, sizeof(fourth));
    CHECK(block != NULL && memcmp(fdb_cached_block_data(block), fourth, sizeof(fourth)) == 0);
    CHECK(memcmp(fdb_cached_block_data(other), third, sizeof(third)) == 0);
    CHECK(fdb_block_cache_acquire(cache, 1, 32, sizeof(first)) == NULL);
    fdb_block_cache_release(cache, block);
    fdb_block_cache_release(cache, other);
    fdb_block_cache_stats(cache, &stats);
    CHECK(stats.evictions == 2 && stats.entries == 2 && stats.memory_bytes == cap);
    fdb_block_cache_destroy(cache);
}
