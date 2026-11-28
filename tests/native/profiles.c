#include "disk_fixture.h"
#include "engine.h"

enum { KIB = 1024, MIB = KIB * KIB };

static void cache_profile(void)
{
    char path[] = "/tmp/fastdb-cache-profile.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    OK(fastdb_options_set_profile(options, FASTDB_PROFILE_CACHE, FASTDB_MODE_MEMORY, NULL));
    CHECK(options->path == NULL && options->eviction_policy == FASTDB_EVICTION_NONE);
    STATUS(fastdb_options_set_profile(options, FASTDB_PROFILE_CACHE, FASTDB_MODE_LSM, path),
           FASTDB_INVALID_ARGUMENT);
    STATUS(fastdb_options_set_profile(options, FASTDB_PROFILE_CACHE, FASTDB_MODE_MEMORY, path),
           FASTDB_INVALID_ARGUMENT);
    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "key", 3, "ephemeral", 9));
    expect_value(db, (FastDBSlice){"key", 3}, (FastDBSlice){"ephemeral", 9}, 1);
    OK(fastdb_close(db));
    OK(fastdb_open(options, &db));
    expect_missing(db, (FastDBSlice){"key", 3});
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    CHECK(rmdir(path) == 0);
}

static void persistent_profile(FastDBStorageProfile profile, FastDBMode mode)
{
    char path[] = "/tmp/fastdb-profile.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    char input[sizeof(path)];
    memcpy(input, path, sizeof(input));
    FastDBOptions *options;
    OK(fastdb_options_create(&options));
    OK(fastdb_options_set_eviction(options, FASTDB_EVICTION_LRU, 2 * KIB));
    OK(fastdb_options_set_profile(options, profile, mode, input));
    input[0] = '!';
    CHECK(strcmp(options->path, path) == 0 && options->mode == mode &&
          options->eviction_policy == FASTDB_EVICTION_NONE);
    STATUS(fastdb_options_set_profile(options, profile, FASTDB_MODE_MEMORY, NULL),
           FASTDB_INVALID_ARGUMENT);
    STATUS(fastdb_options_set_profile(options, profile, mode, ""), FASTDB_INVALID_ARGUMENT);
    STATUS(fastdb_options_set_profile(options, (FastDBStorageProfile)99, mode, path),
           FASTDB_INVALID_ARGUMENT);
    CHECK(strcmp(options->path, path) == 0 && options->mode == mode);

    FastDBDurability durability = profile == FASTDB_PROFILE_WRITE_OPTIMIZED ?
                                  FASTDB_DURABILITY_GROUP_COMMIT : FASTDB_DURABILITY_SYNC;
    CHECK(options->storage.durability == durability);
    CHECK((options->storage.min_free_bytes != 0) == (profile == FASTDB_PROFILE_DURABLE));
    FastDB *db;
    OK(fastdb_open(options, &db));
    CHECK((db->flush != NULL) == (mode == FASTDB_MODE_LSM && profile == FASTDB_PROFILE_WRITE_OPTIMIZED));
    OK(fastdb_put(db, "key", 3, "value", 5));
    if (profile == FASTDB_PROFILE_LOW_MEMORY) {
        CHECK(options->limits.max_memory_bytes == 16 * MIB);
        size_t size = 256 * KIB + 1;
        unsigned char *oversized = calloc(1, size);
        CHECK(oversized != NULL);
        STATUS(fastdb_put(db, "large", 5, oversized, size), FASTDB_LIMIT_EXCEEDED);
        free(oversized);
        FastDBStats stats;
        OK(fastdb_stats(db, &stats));
        CHECK(stats.memory_bytes <= 16 * MIB && stats.revision == 1);
    }
    if (mode == FASTDB_MODE_LSM)
        OK(fastdb_flush(db));
    else
        OK(fastdb_checkpoint(db));
    expect_value(db, (FastDBSlice){"key", 3}, (FastDBSlice){"value", 5}, 1);
    expect_value(db, (FastDBSlice){"key", 3}, (FastDBSlice){"value", 5}, 1);
    FastDBBlockCacheStats cache;
    OK(fastdb_block_cache_stats(db, &cache));
    if (mode == FASTDB_MODE_LSM && profile == FASTDB_PROFILE_READ_OPTIMIZED)
        CHECK(cache.max_bytes == 16 * MIB && cache.hits > 0 && cache.entries > 0);
    else
        CHECK(cache.max_bytes == 0);
    OK(fastdb_close(db));
    OK(fastdb_open(options, &db));
    expect_value(db, (FastDBSlice){"key", 3}, (FastDBSlice){"value", 5}, 1);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    cache_profile();
    const FastDBStorageProfile profiles[] = {FASTDB_PROFILE_BALANCED, FASTDB_PROFILE_READ_OPTIMIZED,
        FASTDB_PROFILE_WRITE_OPTIMIZED, FASTDB_PROFILE_DURABLE, FASTDB_PROFILE_LOW_MEMORY};
    for (size_t i = 0; i < sizeof(profiles) / sizeof(*profiles); ++i) {
        persistent_profile(profiles[i], FASTDB_MODE_LSM);
        persistent_profile(profiles[i], FASTDB_MODE_MEMORY_PERSISTENT);
    }
}
