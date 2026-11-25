#ifndef FASTDB_DISK_FIXTURE_H
#define FASTDB_DISK_FIXTURE_H

#include "check.h"

#include <dirent.h>
#include <string.h>
#include <unistd.h>

static inline FastDBOptions *disk_options_with_mode(const char *path, FastDBMode mode,
                                                      FastDBDurability durability)
{
    FastDBOptions *options;
    OK(fastdb_options_create(&options));
    FastDBLimits limits;
    fastdb_limits_default(&limits);
    limits.max_key_bytes = 32;
    limits.max_value_bytes = 128;
    limits.max_memory_bytes = 128 * 1024;
    limits.max_batch_operations = 6;
    OK(fastdb_options_set_limits(options, &limits));
    FastDBPersistentOptions lsm;
    fastdb_persistent_options_default(&lsm);
    lsm.durability = durability;
    lsm.max_wal_segment_bytes = 410;
    lsm.max_wal_frame_bytes = 360;
    lsm.target_block_bytes = 80;
    lsm.max_index_bytes = 1024;
    lsm.max_filter_bytes = 320;
    lsm.max_catalog_files = 16;
    lsm.max_catalog_bytes = 2048;
    if (mode == FASTDB_MODE_MEMORY_PERSISTENT)
        lsm.checkpoint_wal_segments = 2;
    OK(fastdb_options_set_storage(options, mode, path, &lsm));
    return options;
}

static inline FastDBOptions *disk_options(const char *path, FastDBDurability durability)
{
    return disk_options_with_mode(path, FASTDB_MODE_LSM, durability);
}

static inline void remove_disk_fixture(const char *path)
{
    DIR *dir = opendir(path);
    CHECK(dir != NULL);
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
            CHECK(unlinkat(dirfd(dir), entry->d_name, 0) == 0);
    }
    CHECK(closedir(dir) == 0);
    CHECK(rmdir(path) == 0);
}

static inline void expect_value(FastDB *db, FastDBSlice key, FastDBSlice expected, uint64_t revision)
{
    void *value;
    size_t size;
    uint64_t actual;
    OK(fastdb_get_with_revision(db, key.data, key.size, &value, &size, &actual));
    CHECK(value != NULL && size == expected.size && actual == revision);
    CHECK(size == 0 || memcmp(value, expected.data, size) == 0);
    fastdb_free(value);
}

static inline void expect_missing(FastDB *db, FastDBSlice key)
{
    bool exists = true;
    void *value = db;
    size_t size = 7;
    uint64_t revision = 100;
    OK(fastdb_exists(db, key.data, key.size, &exists));
    STATUS(fastdb_get_with_revision(db, key.data, key.size, &value, &size, &revision),
           FASTDB_NOT_FOUND);
    CHECK(!exists && value == NULL && size == 0 && revision == 0);
}

#endif
