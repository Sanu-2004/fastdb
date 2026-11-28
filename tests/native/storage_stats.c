#include "disk_fixture.h"
#include "engine.h"
#include "wal.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

typedef struct Failure {
    FDBFsOp operation;
    size_t remaining;
} Failure;

static FDBFsFault inject(void *context, FDBFsOp operation, size_t requested)
{
    (void)requested;
    Failure *failure = context;
    if (operation == failure->operation && failure->remaining != 0 && --failure->remaining == 0)
        return (FDBFsFault){.error = EIO};
    return (FDBFsFault){0};
}

static uint64_t stored_file_bytes(const char *path, const char *prefix)
{
    DIR *dir = opendir(path);
    CHECK(dir != NULL);
    uint64_t total = 0;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, prefix, strlen(prefix)) != 0)
            continue;
        struct stat info;
        CHECK(fstatat(dirfd(dir), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) == 0);
        CHECK(S_ISREG(info.st_mode) && info.st_size >= 0);
        total += (uint64_t)info.st_size;
    }
    CHECK(closedir(dir) == 0);
    return total;
}

static void memory_gauges(void)
{
    FastDBStorageStats stats;
    STATUS(fastdb_storage_stats(NULL, &stats), FASTDB_INVALID_ARGUMENT);
    FastDB *db;
    OK(fastdb_open(NULL, &db));
    STATUS(fastdb_storage_stats(db, NULL), FASTDB_INVALID_ARGUMENT);
    OK(fastdb_storage_stats(db, &stats));
    CHECK(stats.mode == FASTDB_MODE_MEMORY && stats.active_memtable_bytes == 0);
    CHECK(stats.catalog_generation == 0 && stats.wal_files == 0 && stats.wal_bytes == 0);
    CHECK(stats.level_count == 0 && stats.sstable_files == 0 && stats.checkpoint_files == 0);
    OK(fastdb_put(db, "a", 1, "v", 1));
    FastDBSnapshot *snapshot;
    OK(fastdb_snapshot_create(db, &snapshot));
    OK(fastdb_storage_stats(db, &stats));
    CHECK(stats.active_memtable_bytes > 0 && stats.snapshot_bytes > 0);
    CHECK(stats.frozen_memtable_bytes == 0 && stats.reserved_engine_bytes == 0);
    fastdb_snapshot_release(snapshot);
    OK(fastdb_storage_stats(db, &stats));
    CHECK(stats.snapshot_bytes == 0 && stats.activity.wal_frame_bytes_appended == 0);
    OK(fastdb_close(db));
}

static void persistent_inventory(void)
{
    char path[] = "/tmp/fastdb-storage-stats.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDBCompactionOptions policy;
    fastdb_compaction_options_default(&policy);
    policy.max_levels = 2;
    OK(fastdb_options_set_compaction(options, &policy));
    OK(fastdb_options_set_lsm_compaction_strategy(options, FASTDB_COMPACTION_SIZE_TIERED));
    Failure failure = {0};
    const FDBFs fs = {inject, &failure};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    FastDBStorageStats stats;
    OK(fastdb_storage_stats(db, &stats));
    CHECK(stats.mode == FASTDB_MODE_LSM && stats.level_count == 2 && stats.wal_files == 0);
    CHECK(stats.catalog_generation > 0 && stats.reserved_engine_bytes > 0);
    unsigned char payload[120] = {3};
    for (char key = 'a'; key <= 'c'; ++key)
        OK(fastdb_put(db, &key, 1, payload, sizeof(payload)));
    uint64_t frame_bytes = FDB_WAL_FRAME_BYTES + FDB_RECORD_HEADER_BYTES + 1 + sizeof(payload);
    OK(fastdb_storage_stats(db, &stats));
    CHECK(stats.wal_files == 2 && stats.active_wal_bytes == FDB_WAL_HEADER_BYTES + frame_bytes);
    CHECK(stats.wal_bytes == 2 * FDB_WAL_HEADER_BYTES + 3 * frame_bytes);
    CHECK(stats.wal_bytes == stored_file_bytes(path, "wal-"));
    CHECK(stats.activity.wal_frame_bytes_appended == 3 * frame_bytes && stats.active_memtable_bytes > 0);
    OK(fastdb_flush(db));
    OK(fastdb_storage_stats(db, &stats));
    CHECK(stats.wal_files == 0 && stats.active_wal_bytes == 0 && stats.wal_bytes == 0);
    CHECK(stats.active_memtable_bytes == 0 && stats.sstable_files == 1 && stats.sstable_bytes != 0);
    CHECK(stats.levels[0].files == 1 && stats.levels[0].bytes == stats.sstable_bytes);
    CHECK(stats.activity.flush_files_published == 1 && stats.activity.flush_output_bytes == stats.sstable_bytes);
    CHECK(stats.sstable_bytes == stored_file_bytes(path, "sst-"));

    OK(fastdb_put(db, "d", 1, payload, sizeof(payload)));
    OK(fastdb_flush(db));
    OK(fastdb_storage_stats(db, &stats));
    CHECK(stats.sstable_files == 2 && stats.activity.flush_files_published == 2);
    uint64_t inputs = stats.sstable_bytes;
    failure = (Failure){FDB_FS_RENAME, 2};
    STATUS(fastdb_compact(db), FASTDB_IO_ERROR);
    OK(fastdb_storage_stats(db, &stats));
    CHECK(stats.sstable_files == 2 && stats.sstable_bytes == inputs);
    CHECK(stats.activity.compaction_job_errors == 1 && stats.activity.compaction_jobs_published == 0 &&
          stats.activity.compaction_input_bytes == 0 && stats.activity.compaction_output_bytes == 0);
    OK(fastdb_compact(db));
    OK(fastdb_storage_stats(db, &stats));
    CHECK(stats.sstable_files == 1 && stats.activity.compaction_jobs_published == 1);
    CHECK(stats.activity.compaction_job_errors == 1 && stats.activity.compaction_input_bytes == inputs &&
          stats.activity.compaction_output_bytes == stats.sstable_bytes);
    uint64_t first_output = stats.sstable_bytes;

    OK(fastdb_put(db, "e", 1, payload, sizeof(payload)));
    OK(fastdb_flush(db));
    OK(fastdb_storage_stats(db, &stats));
    inputs += stats.sstable_bytes;
    failure = (Failure){FDB_FS_UNLINK, 1};
    STATUS(fastdb_compact(db), FASTDB_IO_ERROR);
    OK(fastdb_storage_stats(db, &stats));
    CHECK(stats.sstable_files == 1 && stats.activity.compaction_jobs_published == 2);
    CHECK(stats.activity.compaction_job_errors == 2 && stats.activity.compaction_input_bytes == inputs &&
          stats.activity.compaction_output_bytes == first_output + stats.sstable_bytes);
    CHECK(stats.sstable_bytes < stored_file_bytes(path, "sst-"));
    uint64_t bytes = stats.sstable_bytes;
    OK(fastdb_close(db));

    OK(fastdb_open(options, &db));
    OK(fastdb_storage_stats(db, &stats));
    CHECK(stats.sstable_files == 1 && stats.sstable_bytes == bytes && stats.wal_files == 0);
    CHECK(stats.sstable_bytes == stored_file_bytes(path, "sst-"));
    CHECK(stats.activity.flush_files_published == 0 && stats.activity.compaction_job_errors == 0 &&
          stats.activity.compaction_jobs_published == 0);
    expect_value(db, (FastDBSlice){"e", 1}, (FastDBSlice){payload, sizeof(payload)}, 5);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void checkpoint_and_uncertainty(void)
{
    char path[] = "/tmp/fastdb-checkpoint-stats.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options_with_mode(path, FASTDB_MODE_MEMORY_PERSISTENT,
                                                     FASTDB_DURABILITY_SYNC);
    Failure failure = {0};
    const FDBFs fs = {inject, &failure};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    OK(fastdb_put(db, "a", 1, "v", 1));
    OK(fastdb_checkpoint(db));
    FastDBStorageStats stats;
    OK(fastdb_storage_stats(db, &stats));
    CHECK(stats.mode == FASTDB_MODE_MEMORY_PERSISTENT && stats.level_count == 0);
    CHECK(stats.active_memtable_bytes > 0 && stats.checkpoint_files == 1 && stats.wal_files == 0);
    CHECK(stats.checkpoint_bytes != 0 && stats.sstable_bytes == 0 &&
          stats.activity.checkpoints_published == 1 &&
          stats.activity.checkpoint_output_bytes == stats.checkpoint_bytes);
    CHECK(stats.checkpoint_bytes == stored_file_bytes(path, "chk-"));
    uint64_t original = stats.checkpoint_bytes;
    OK(fastdb_put(db, "b", 1, "", 0));
    failure = (Failure){FDB_FS_SYNC, 1};
    STATUS(fastdb_put(db, "c", 1, "", 0), FASTDB_COMMIT_UNCERTAIN);
    STATUS(fastdb_storage_stats(db, &stats), FASTDB_COMMIT_UNCERTAIN);
    CHECK(stats.catalog_generation == 0 && stats.checkpoint_files == 0 && stats.level_count == 0);
    STATUS(fastdb_close(db), FASTDB_COMMIT_UNCERTAIN);
    OK(fastdb_open(options, &db));
    OK(fastdb_storage_stats(db, &stats));
    CHECK(stats.checkpoint_files == 1 && stats.checkpoint_bytes == original && stats.wal_files == 1);
    CHECK(stats.wal_bytes == stats.active_wal_bytes && stats.wal_bytes > FDB_WAL_HEADER_BYTES);
    CHECK(stats.activity.checkpoints_published == 0 && stats.activity.wal_frame_bytes_appended == 0);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    memory_gauges();
    persistent_inventory();
    checkpoint_and_uncertainty();
}
