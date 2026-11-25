#include "disk_fixture.h"

#include <fcntl.h>

static size_t count_files(const char *path, const char *prefix)
{
    DIR *dir = opendir(path);
    CHECK(dir != NULL);
    size_t count = 0;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0)
            ++count;
    }
    CHECK(closedir(dir) == 0);
    return count;
}

static void checkpoints_and_replay(void)
{
    char path[] = "/tmp/fastdb-ram-checkpoint.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options_with_mode(path, FASTDB_MODE_MEMORY_PERSISTENT,
                                                      FASTDB_DURABILITY_SYNC);
    FastDB *db;
    OK(fastdb_open(options, &db));
    const FastDBSlice key = {"m\0", 2};
    unsigned char large[120] = {0};
    for (unsigned char version = 1; version <= 4; ++version) {
        large[0] = version;
        OK(fastdb_put(db, key.data, key.size, large, sizeof(large)));
    }
    CHECK(count_files(path, "wal-") == 2 && count_files(path, "chk-") == 0);
    OK(fastdb_put(db, key.data, key.size, large, sizeof(large)));
    CHECK(count_files(path, "wal-") == 1 && count_files(path, "chk-") == 1);
    OK(fastdb_put(db, "x", 1, NULL, 0));
    OK(fastdb_close(db));

    OK(fastdb_open(options, &db));
    expect_value(db, key, (FastDBSlice){large, sizeof(large)}, 5);
    expect_value(db, (FastDBSlice){"x", 1}, (FastDBSlice){0}, 6);
    FastDBIterator *it;
    OK(fastdb_scan_prefix(db, (FastDBSlice){0}, 2, &it));
    OK(fastdb_delete(db, key.data, key.size));
    OK(fastdb_delete(db, "x", 1));
    FastDBSlice got_key, got_value;
    uint64_t revision;
    OK(fastdb_iterator_next(it, &got_key, &got_value, &revision));
    CHECK(got_key.size == key.size && revision == 5);
    CHECK(got_value.size == sizeof(large) && memcmp(got_value.data, large, sizeof(large)) == 0);
    STATUS(fastdb_close(db), FASTDB_BUSY);
    fastdb_iterator_close(it);
    FastDBStats stats;
    OK(fastdb_stats(db, &stats));
    CHECK(stats.record_count == 0 && stats.revision == 8);
    OK(fastdb_checkpoint(db));
    CHECK(count_files(path, "wal-") == 0 && count_files(path, "chk-") == 1);
    STATUS(fastdb_flush(db), FASTDB_UNSUPPORTED);
    OK(fastdb_close(db));

    FastDBOptions *lsm = disk_options(path, FASTDB_DURABILITY_SYNC);
    STATUS(fastdb_open(lsm, &db), FASTDB_UNSUPPORTED);
    CHECK(db == NULL);
    fastdb_options_destroy(lsm);
    OK(fastdb_open(options, &db));
    expect_missing(db, key);
    expect_missing(db, (FastDBSlice){"x", 1});
    OK(fastdb_stats(db, &stats));
    CHECK(stats.record_count == 0 && stats.revision == 8);
    OK(fastdb_compare_and_swap(db, key.data, key.size, FASTDB_EXPECT_ABSENT, "again", 5));
    expect_value(db, key, (FastDBSlice){"again", 5}, 9);
    OK(fastdb_close(db));
    OK(fastdb_open(options, &db));
    expect_value(db, key, (FastDBSlice){"again", 5}, 9);
    OK(fastdb_checkpoint(db));
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void no_eviction_and_missing_checkpoint(void)
{
    char path[] = "/tmp/fastdb-ram-limit.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options_with_mode(path, FASTDB_MODE_MEMORY_PERSISTENT,
                                                      FASTDB_DURABILITY_SYNC);
    FastDB *db;
    OK(fastdb_open(options, &db));
    for (unsigned char key = 1; key <= 16; ++key)
        OK(fastdb_put(db, &key, 1, NULL, 0));
    unsigned char extra = 17;
    STATUS(fastdb_put(db, &extra, 1, NULL, 0), FASTDB_LIMIT_EXCEEDED);
    OK(fastdb_checkpoint(db));
    STATUS(fastdb_put(db, &extra, 1, NULL, 0), FASTDB_LIMIT_EXCEEDED);
    const unsigned char first = 1;
    expect_value(db, (FastDBSlice){&first, 1}, (FastDBSlice){0}, 1);
    OK(fastdb_delete(db, &first, 1));
    OK(fastdb_put(db, &extra, 1, NULL, 0));
    OK(fastdb_checkpoint(db));
    OK(fastdb_close(db));

    DIR *dir = opendir(path);
    CHECK(dir != NULL);
    const struct dirent *entry;
    bool removed = false;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "chk-", 4) == 0) {
            CHECK(!removed && unlinkat(dirfd(dir), entry->d_name, 0) == 0);
            removed = true;
        }
    }
    CHECK(closedir(dir) == 0 && removed);
    STATUS(fastdb_open(options, &db), FASTDB_CORRUPTION);
    CHECK(db == NULL);
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    checkpoints_and_replay();
    no_eviction_and_missing_checkpoint();
}
