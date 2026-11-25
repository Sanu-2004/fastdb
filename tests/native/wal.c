#include "check.h"
#include "crc32c.h"
#include "wal.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static const FDBWalLimits limits = {
    .max_key_bytes = 16, .max_value_bytes = 32, .max_batch_operations = 4,
    .max_frame_bytes = 144, .max_segment_bytes = 200,
};

typedef struct Visit {
    size_t batches;
    size_t operations;
    uint64_t last_sequence;
    uint64_t tombstones;
} Visit;

typedef struct Fault {
    FDBFsOp operation;
    size_t remaining;
    size_t chunk;
    int error;
} Fault;

static FDBFsFault inject(void *context, FDBFsOp op, size_t requested)
{
    (void)requested;
    Fault *fault = context;
    if (op != fault->operation || fault->remaining == 0)
        return (FDBFsFault){0};
    if (--fault->remaining == 0)
        return (FDBFsFault){.error = fault->error};
    return (FDBFsFault){.max_transfer = fault->chunk};
}

static FastDBStatus visit(void *context, const unsigned char *payload, size_t size,
                           size_t count, uint64_t sequence)
{
    Visit *result = context;
    CHECK(sequence == result->last_sequence + 1);
    for (size_t i = 0; i < count; ++i) {
        FDBRecord record;
        size_t used;
        OK(fdb_record_decode(payload, size, limits.max_key_bytes, limits.max_value_bytes,
                             &record, &used));
        CHECK(record.sequence == sequence + i);
        CHECK(record.key.size == 2 && memcmp(record.key.data, "x\0", 2) == 0);
        if (record.kind == FDB_PUT) {
            CHECK(record.value.size == 2 && memcmp(record.value.data, "\0\xff", 2) == 0);
            CHECK(record.expires_at_ms == 1000);
        } else {
            CHECK(record.kind == FDB_DELETE);
            ++result->tombstones;
        }
        payload += used;
        size -= used;
    }
    CHECK(size == 0);
    ++result->batches;
    result->operations += count;
    result->last_sequence += count;
    return FASTDB_OK;
}

static FDBRecord put(uint64_t revision)
{
    return (FDBRecord){{"x\0", 2}, {"\0\xff", 2}, revision, 1000, FDB_PUT};
}

static FDBRecord tombstone(uint64_t revision)
{
    return (FDBRecord){{"x\0", 2}, {0}, revision, 0, FDB_DELETE};
}

static void rotation(const FDBDirectory *dir)
{
    FDBWal wal;
    OK(fdb_wal_create(dir, "wal.1", 1, 17, &limits, &wal));
    FDBRecord records[] = {put(18), tombstone(19)};
    size_t frame_size;
    OK(fdb_wal_frame_size(&wal, records, 2, &frame_size));
    CHECK(frame_size == 94);
    OK(fdb_wal_append(&wal, records, 2, true));
    CHECK(wal.sequence == 19 && wal.offset == 134);
    records[0] = put(20);
    records[1] = tombstone(21);
    STATUS(fdb_wal_append(&wal, records, 2, true), FASTDB_LIMIT_EXCEEDED);
    OK(fdb_wal_close(&wal));
    STATUS(fdb_wal_create(dir, "wal.1", 1, 17, &limits, &wal), FASTDB_IO_ERROR);
    CHECK(wal.fd == -1);
    OK(fdb_wal_create(dir, "wal.2", 2, 19, &limits, &wal));
    STATUS(fdb_wal_append(&wal, records + 1, 1, false), FASTDB_INVALID_ARGUMENT);
    OK(fdb_wal_append(&wal, records, 1, false));
    OK(fdb_wal_sync(&wal));
    OK(fdb_wal_close(&wal));

    Visit result = {.last_sequence = 17};
    OK(fdb_wal_open(dir, "wal.1", 1, 17, &limits, FDB_WAL_REPAIR_TAIL, &(FDBWalReplay){.visit = visit, .context = &result}, &wal));
    OK(fdb_wal_close(&wal));
    OK(fdb_wal_open(dir, "wal.2", 2, result.last_sequence, &limits, FDB_WAL_REPAIR_TAIL, &(FDBWalReplay){.visit = visit, .context = &result}, &wal));
    OK(fdb_wal_close(&wal));
    CHECK(result.batches == 2 && result.operations == 3 && result.last_sequence == 20);
    CHECK(result.tombstones == 1);
    STATUS(fdb_wal_open(dir, "wal.2", 2, 0, &limits, FDB_WAL_REPAIR_TAIL, NULL, &wal), FASTDB_CORRUPTION);
    STATUS(fdb_wal_open(dir, "missing", 3, 20, &limits, FDB_WAL_REPAIR_TAIL, NULL, &wal), FASTDB_CORRUPTION);
    OK(fdb_dir_unlink(dir, "wal.1"));
    OK(fdb_dir_unlink(dir, "wal.2"));
}

static void interrupted(const FDBDirectory *dir, Fault *fault, size_t bytes_written)
{
    FDBWal wal;
    OK(fdb_wal_create(dir, "wal.interrupted", 3, 0, &limits, &wal));
    FDBRecord first = put(1);
    OK(fdb_wal_append(&wal, &first, 1, true));
    uint64_t committed = wal.offset;
    const FDBRecord next[] = {tombstone(2), put(3)};
    *fault = (Fault){FDB_FS_WRITE, 2, bytes_written, ENOSPC};
    STATUS(fdb_wal_append(&wal, next, 2, true), FASTDB_COMMIT_UNCERTAIN);
    STATUS(fdb_wal_append(&wal, next, 2, true), FASTDB_COMMIT_UNCERTAIN);
    uint64_t size;
    OK(fdb_fs_size(wal.fd, &size));
    CHECK(size == committed + bytes_written);
    STATUS(fdb_wal_close(&wal), FASTDB_COMMIT_UNCERTAIN);

    Visit result = {0};
    OK(fdb_wal_open(dir, "wal.interrupted", 3, 0, &limits, FDB_WAL_REPAIR_TAIL, &(FDBWalReplay){.visit = visit, .context = &result}, &wal));
    CHECK(wal.offset == committed && result.batches == 1 && result.last_sequence == 1);
    OK(fdb_fs_size(wal.fd, &size));
    CHECK(size == committed);
    OK(fdb_wal_append(&wal, next, 2, true));
    OK(fdb_wal_close(&wal));
    OK(fdb_dir_unlink(dir, "wal.interrupted"));
}

static void uncertain_sync_and_corruption(const FDBDirectory *dir, Fault *fault)
{
    FDBWal wal;
    OK(fdb_wal_create(dir, "wal.uncertain", 4, 0, &limits, &wal));
    FDBRecord records[] = {put(1), tombstone(2)};
    *fault = (Fault){FDB_FS_SYNC, 1, 0, EIO};
    STATUS(fdb_wal_append(&wal, records, 2, true), FASTDB_COMMIT_UNCERTAIN);
    STATUS(fdb_wal_close(&wal), FASTDB_COMMIT_UNCERTAIN);

    Visit result = {0};
    OK(fdb_wal_open(dir, "wal.uncertain", 4, 0, &limits, FDB_WAL_REPAIR_TAIL, &(FDBWalReplay){.visit = visit, .context = &result}, &wal));
    CHECK(result.batches == 1 && result.operations == 2 && result.tombstones == 1);
    uint64_t original = wal.offset;
    size_t transferred;
    unsigned char byte = 0;
    OK(fdb_fs_write(NULL, wal.fd, original - 2, &byte, 1, &transferred));
    OK(fdb_wal_close(&wal));
    result = (Visit){0};
    STATUS(fdb_wal_open(dir, "wal.uncertain", 4, 0, &limits, FDB_WAL_REPAIR_TAIL, &(FDBWalReplay){.visit = visit, .context = &result}, &wal),
           FASTDB_CORRUPTION);
    CHECK(wal.fd == -1 && result.batches == 0);
    int fd;
    OK(fdb_dir_open_file(dir, "wal.uncertain", FDB_FILE_READ, &fd));
    uint64_t preserved;
    OK(fdb_fs_size(fd, &preserved));
    CHECK(preserved == original);
    OK(fdb_fs_close(fd));
    OK(fdb_dir_unlink(dir, "wal.uncertain"));
}

int main(void)
{
    CHECK(fdb_crc32c("123456789", 9) == UINT32_C(0xe3069283));
    char path[] = "/tmp/fastdb-wal.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    Fault fault = {0};
    FDBFs fs = {inject, &fault};
    FDBDirectory dir;
    OK(fdb_dir_lock(path, &fs, &dir));
    rotation(&dir);
    interrupted(&dir, &fault, 3);
    interrupted(&dir, &fault, FDB_WAL_FRAME_BYTES + 2);
    uncertain_sync_and_corruption(&dir, &fault);
    OK(fdb_dir_close(&dir));
    char lock_path[sizeof(path) + sizeof("/.fastdb.lock")];
    CHECK(snprintf(lock_path, sizeof(lock_path), "%s/.fastdb.lock", path) > 0);
    CHECK(unlink(lock_path) == 0 && rmdir(path) == 0);
}
