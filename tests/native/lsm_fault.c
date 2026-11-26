#include "disk_fixture.h"
#include "engine.h"

#include <errno.h>
#include <dirent.h>
#include <string.h>

static size_t table_file_count(const char *path)
{
    DIR *directory = opendir(path);
    CHECK(directory != NULL);
    size_t count = 0;
    const struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(entry->d_name, "sst-", 4) == 0)
            ++count;
    }
    CHECK(closedir(directory) == 0);
    return count;
}

typedef struct Injection {
    FDBFsOp op;
    size_t countdown;
    uint64_t available_bytes;
    bool override_space;
} Injection;

static FDBFsFault reject(void *context, FDBFsOp operation, size_t requested)
{
    (void)requested;
    Injection *state = context;
    if (operation == FDB_FS_SPACE && state->override_space)
        return (FDBFsFault){.space_override = true, .available_bytes = state->available_bytes};
    if (state->op == operation && state->countdown != 0 && --state->countdown == 0)
        return (FDBFsFault){.error = EIO};
    return (FDBFsFault){0};
}

static void low_space_admission(void)
{
    char path[] = "/tmp/fastdb-lsm-space.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDBPersistentOptions settings;
    fastdb_persistent_options_default(&settings);
    settings.max_wal_segment_bytes = 410;
    settings.max_wal_frame_bytes = 360;
    settings.target_block_bytes = 80;
    settings.max_index_bytes = 1024;
    settings.max_filter_bytes = 320;
    settings.max_catalog_files = 16;
    settings.max_catalog_bytes = 2048;
    settings.min_free_bytes = 1024;
    OK(fastdb_options_set_storage(options, FASTDB_MODE_LSM, path, &settings));

    Injection injection = {.op = FDB_FS_SPACE, .available_bytes = 1024,
                           .override_space = true};
    const FDBFs fs = {reject, &injection};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    injection = (Injection){.op = FDB_FS_SPACE, .countdown = 1};
    STATUS(fastdb_put(db, "a", 1, "value", 5), FASTDB_IO_ERROR);
    FastDBStats stats;
    OK(fastdb_stats(db, &stats));
    CHECK(stats.revision == 0);
    expect_missing(db, (FastDBSlice){"a", 1});

    injection = (Injection){.op = FDB_FS_SPACE, .available_bytes = 1024,
                            .override_space = true};
    STATUS(fastdb_put(db, "a", 1, "value", 5), FASTDB_LIMIT_EXCEEDED);
    OK(fastdb_stats(db, &stats));
    CHECK(stats.revision == 0);
    expect_missing(db, (FastDBSlice){"a", 1});

    injection.available_bytes = UINT64_MAX;
    OK(fastdb_put(db, "a", 1, "value", 5));
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"value", 5}, 1);
    injection.available_bytes = 1024;
    STATUS(fastdb_flush(db), FASTDB_LIMIT_EXCEEDED);
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"value", 5}, 1);
    injection.available_bytes = UINT64_MAX;
    OK(fastdb_flush(db));
    CHECK(table_file_count(path) == 1);
    injection.available_bytes = 1024;
    STATUS(fastdb_compact(db), FASTDB_LIMIT_EXCEEDED);
    CHECK(table_file_count(path) == 1);
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"value", 5}, 1);
    injection.available_bytes = UINT64_MAX;
    OK(fastdb_compact(db));
    CHECK(table_file_count(path) == 1);
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"value", 5}, 1);
    OK(fastdb_close(db));

    settings.min_free_bytes = UINT64_MAX;
    OK(fastdb_options_set_storage(options, FASTDB_MODE_LSM, path, &settings));
    injection.available_bytes = UINT64_MAX;
    OK(fdb_open_with_fs(options, &fs, &db));
    STATUS(fastdb_put(db, "b", 1, "overflow", 8), FASTDB_LIMIT_EXCEEDED);
    OK(fastdb_stats(db, &stats));
    CHECK(stats.revision == 1);
    expect_missing(db, (FastDBSlice){"b", 1});
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void sync_failure(void)
{
    char path[] = "/tmp/fastdb-lsm-uncertain.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    Injection injection = {0};
    const FDBFs fs = {reject, &injection};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    OK(fastdb_put(db, "a", 1, "old", 3));
    FastDBBatch *batch;
    OK(fastdb_batch_create(options, &batch));
    OK(fastdb_batch_delete(batch, "a", 1));
    OK(fastdb_batch_put(batch, "b", 1, "new", 3));
    injection = (Injection){.op = FDB_FS_SYNC, .countdown = 1};
    STATUS(fastdb_batch_commit(db, batch), FASTDB_COMMIT_UNCERTAIN);
    fastdb_batch_destroy(batch);
    bool exists;
    STATUS(fastdb_exists(db, "a", 1, &exists), FASTDB_COMMIT_UNCERTAIN);
    STATUS(fastdb_put(db, "c", 1, NULL, 0), FASTDB_COMMIT_UNCERTAIN);
    STATUS(fastdb_flush(db), FASTDB_COMMIT_UNCERTAIN);
    STATUS(fastdb_close(db), FASTDB_COMMIT_UNCERTAIN);
    /* With a completed pwrite and failed fsync, this process can see the full frame. */
    OK(fastdb_open(options, &db));
    expect_missing(db, (FastDBSlice){"a", 1});
    expect_value(db, (FastDBSlice){"b", 1}, (FastDBSlice){"new", 3}, 3);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void flush_and_read_failure(void)
{
    char path[] = "/tmp/fastdb-lsm-io.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    Injection injection = {0};
    const FDBFs fs = {reject, &injection};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    OK(fastdb_put(db, "a", 1, "safe", 4));
    injection = (Injection){.op = FDB_FS_RENAME, .countdown = 2};
    STATUS(fastdb_flush(db), FASTDB_IO_ERROR);
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"safe", 4}, 1);
    OK(fastdb_close(db));
    OK(fdb_open_with_fs(options, &fs, &db));
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"safe", 4}, 1);
    OK(fastdb_flush(db));
    injection = (Injection){.op = FDB_FS_READ, .countdown = 1};
    bool exists = true;
    STATUS(fastdb_exists(db, "a", 1, &exists), FASTDB_IO_ERROR);
    CHECK(!exists);
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"safe", 4}, 1);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void asynchronous_close(void)
{
    char path[] = "/tmp/fastdb-lsm-async.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_ASYNC);
    Injection injection = {0};
    const FDBFs fs = {reject, &injection};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    OK(fastdb_put(db, "a", 1, NULL, 0));
    injection = (Injection){.op = FDB_FS_SYNC, .countdown = 1};
    OK(fastdb_put(db, "b", 1, NULL, 0));
    CHECK(injection.countdown == 1);
    STATUS(fastdb_close(db), FASTDB_COMMIT_UNCERTAIN);
    CHECK(injection.countdown == 0);
    OK(fastdb_open(options, &db));
    expect_value(db, (FastDBSlice){"b", 1}, (FastDBSlice){0}, 2);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void checkpoint_failure(void)
{
    char path[] = "/tmp/fastdb-checkpoint-fault.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options_with_mode(path, FASTDB_MODE_MEMORY_PERSISTENT,
                                                      FASTDB_DURABILITY_SYNC);
    Injection injection = {0};
    const FDBFs fs = {reject, &injection};
    FastDB *db;
    const FastDBSlice a = {"a", 1}, value = {"preserved", 9};
    OK(fdb_open_with_fs(options, &fs, &db));
    OK(fastdb_put(db, a.data, a.size, value.data, value.size));
    injection = (Injection){.op = FDB_FS_RENAME, .countdown = 2};
    STATUS(fastdb_checkpoint(db), FASTDB_IO_ERROR);
    expect_value(db, a, value, 1);
    OK(fastdb_close(db));
    OK(fdb_open_with_fs(options, &fs, &db));
    expect_value(db, a, value, 1);
    OK(fastdb_delete(db, a.data, a.size));
    injection = (Injection){.op = FDB_FS_SYNC, .countdown = 6};
    STATUS(fastdb_checkpoint(db), FASTDB_COMMIT_UNCERTAIN);
    STATUS(fastdb_close(db), FASTDB_COMMIT_UNCERTAIN);
    OK(fastdb_open(options, &db));
    expect_missing(db, a);
    FastDBStats stats;
    OK(fastdb_stats(db, &stats));
    CHECK(stats.revision == 2);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void compaction_failure(FDBFsOp op, size_t countdown, FastDBStatus failure)
{
    char path[] = "/tmp/fastdb-compact-fault.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDBCompactionOptions compaction;
    fastdb_compaction_options_default(&compaction);
    compaction.max_levels = 2;
    OK(fastdb_options_set_compaction(options, &compaction));
    Injection injection = {0};
    const FDBFs fs = {reject, &injection};
    FastDB *db;
    const FastDBSlice masked = {"a", 1}, retained = {"b", 1}, kept = {"kept", 4};
    OK(fdb_open_with_fs(options, &fs, &db));
    OK(fastdb_put(db, masked.data, masked.size, "old", 3));
    OK(fastdb_put(db, retained.data, retained.size, kept.data, kept.size));
    OK(fastdb_flush(db));
    OK(fastdb_compact(db));
    OK(fastdb_delete(db, masked.data, masked.size));
    OK(fastdb_flush(db));
    OK(fastdb_put(db, "wal", 3, "pending", 7));
    injection = (Injection){.op = op, .countdown = countdown};
    STATUS(fastdb_compact(db), failure);
    CHECK(injection.countdown == 0);
    if (failure == FASTDB_COMMIT_UNCERTAIN) {
        FastDBStats stats;
        STATUS(fastdb_stats(db, &stats), FASTDB_COMMIT_UNCERTAIN);
        STATUS(fastdb_close(db), FASTDB_COMMIT_UNCERTAIN);
    } else {
        expect_missing(db, masked);
        expect_value(db, retained, kept, 2);
        OK(fastdb_close(db));
    }
    /* Both original inputs and the renamed output exist until safe reclamation. */
    DIR *dir = opendir(path);
    CHECK(dir != NULL);
    size_t tables = 0;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "sst-", 4) == 0)
            ++tables;
    }
    CHECK(tables == 3 && closedir(dir) == 0);
    OK(fastdb_open(options, &db));
    expect_missing(db, masked);
    expect_value(db, retained, kept, 2);
    expect_value(db, (FastDBSlice){"wal", 3}, (FastDBSlice){"pending", 7}, 4);
    OK(fastdb_compact(db));
    OK(fastdb_put(db, masked.data, masked.size, NULL, 0));
    expect_value(db, masked, (FastDBSlice){0}, 5);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    low_space_admission();
    sync_failure();
    flush_and_read_failure();
    asynchronous_close();
    checkpoint_failure();
    compaction_failure(FDB_FS_RENAME, 3, FASTDB_IO_ERROR);
    compaction_failure(FDB_FS_SYNC, 6, FASTDB_COMMIT_UNCERTAIN);
}
