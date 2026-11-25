#include "check.h"
#include "wal_set.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static const FDBManifestLimits catalog_limits = {4, 4096, 16, 3};
static const FDBWalLimits wal_limits = {16, 16, 2, 150, 200};

typedef struct Replay {
    unsigned char values[8];
    size_t count;
    size_t frames;
} Replay;

static FastDBStatus visit(void *context, const unsigned char *payload, size_t size,
                           size_t count, uint64_t first)
{
    Replay *replay = context;
    CHECK(first == replay->count + 1 && count <= sizeof(replay->values) - replay->count);
    for (size_t i = 0; i < count; ++i) {
        FDBRecord record;
        size_t used;
        OK(fdb_record_decode(payload, size, wal_limits.max_key_bytes, wal_limits.max_value_bytes,
                             &record, &used));
        CHECK(record.key.size == 1 && *(const char *)record.key.data == 'k');
        CHECK(record.kind == FDB_PUT && record.value.size == 1);
        replay->values[replay->count++] = *(const unsigned char *)record.value.data;
        payload += used;
        size -= used;
    }
    CHECK(size == 0);
    ++replay->frames;
    return FASTDB_OK;
}

static FDBFsFault inject(void *context, FDBFsOp operation, size_t requested)
{
    (void)requested;
    size_t *rename_countdown = context;
    if (operation == FDB_FS_RENAME && *rename_countdown != 0 && --*rename_countdown == 0)
        return (FDBFsFault){.error = EIO};
    return (FDBFsFault){0};
}

static FDBRecord record(uint64_t sequence, const char *value)
{
    return (FDBRecord){{"k", 1}, {value, 1}, sequence, 0, FDB_PUT};
}

static void test_rotation(const FDBDirectory *dir, size_t *rename_countdown)
{
    FDBManifest manifest;
    OK(fdb_manifest_new(FDB_MODE_MEMORY_PERSISTENT, &manifest));
    OK(fdb_manifest_publish(dir, &catalog_limits, 0, &manifest));
    FDBWalSet set;
    Replay replay = {0};
    OK(fdb_wal_set_open(dir, &catalog_limits, &wal_limits, 0, &(FDBWalReplay){.visit = visit, .context = &replay}, &set));
    CHECK(set.catalog.view.count == 0 && replay.frames == 0);
    const FDBRecord first[] = {record(1, "a"), record(2, "b")};
    const FDBRecord second[] = {record(3, "c"), record(4, "d")};
    OK(fdb_wal_set_append(&set, first, 2, false));
    CHECK(set.catalog.view.count == 1 && set.wal.sequence == 2);

    *rename_countdown = 2;
    STATUS(fdb_wal_set_append(&set, second, 2, true), FASTDB_IO_ERROR);
    CHECK(set.catalog.view.next_file_id == 3 && set.wal.sequence == 2);
    OK(fdb_wal_set_close(&set));

    OK(fdb_wal_set_open(dir, &catalog_limits, &wal_limits, 0, &(FDBWalReplay){.visit = visit, .context = &replay}, &set));
    CHECK(replay.count == 2 && replay.frames == 1 && memcmp(replay.values, "ab", 2) == 0);
    CHECK(set.catalog.view.count == 1);
    OK(fdb_wal_set_append(&set, second, 2, true));
    CHECK(set.catalog.view.count == 2 && set.wal.sequence == 4);
    CHECK(set.catalog.view.entries[0].id == 1 && set.catalog.view.entries[1].id == 3);
    CHECK(set.catalog.view.entries[0].sequence_end == 2);
    CHECK(set.catalog.view.entries[1].sequence_begin == 2);
    OK(fdb_wal_set_close(&set));

    replay = (Replay){0};
    OK(fdb_wal_set_open(dir, &catalog_limits, &wal_limits, 0, &(FDBWalReplay){.visit = visit, .context = &replay}, &set));
    CHECK(replay.count == 4 && replay.frames == 2 && memcmp(replay.values, "abcd", 4) == 0);
    OK(fdb_wal_set_close(&set));
}

static void test_sealed_tail(const FDBDirectory *dir)
{
    const char *first = "wal-00000000000000000001";
    int fd;
    OK(fdb_dir_open_file(dir, first, FDB_FILE_READ_WRITE, &fd));
    uint64_t size;
    OK(fdb_fs_size(fd, &size));
    CHECK(size > FDB_WAL_HEADER_BYTES);
    OK(fdb_fs_truncate(NULL, fd, size - 1));
    OK(fdb_fs_close(fd));
    FDBWalSet set;
    Replay replay = {0};
    STATUS(fdb_wal_set_open(dir, &catalog_limits, &wal_limits, 0, &(FDBWalReplay){.visit = visit, .context = &replay}, &set),
           FASTDB_CORRUPTION);
    CHECK(replay.frames == 0);
    OK(fdb_dir_open_file(dir, first, FDB_FILE_READ, &fd));
    uint64_t remaining;
    OK(fdb_fs_size(fd, &remaining));
    CHECK(remaining == size - 1);
    OK(fdb_fs_close(fd));
    OK(fdb_dir_unlink(dir, first));
    STATUS(fdb_wal_set_open(dir, &catalog_limits, &wal_limits, 0, &(FDBWalReplay){.visit = visit, .context = &replay}, &set),
           FASTDB_CORRUPTION);
}

int main(void)
{
    char path[] = "/tmp/fastdb-wal-set.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    size_t rename_countdown = 0;
    const FDBFs fs = {inject, &rename_countdown};
    FDBDirectory dir;
    OK(fdb_dir_lock(path, &fs, &dir));
    test_rotation(&dir, &rename_countdown);
    test_sealed_tail(&dir);
    OK(fdb_dir_unlink(&dir, "wal-00000000000000000002"));
    OK(fdb_dir_unlink(&dir, "wal-00000000000000000003"));
    OK(fdb_dir_unlink(&dir, "MANIFEST"));
    OK(fdb_dir_close(&dir));
    char lock_path[sizeof(path) + sizeof("/.fastdb.lock")];
    CHECK(snprintf(lock_path, sizeof(lock_path), "%s/.fastdb.lock", path) > 0);
    CHECK(unlink(lock_path) == 0 && rmdir(path) == 0);
}
