#include "check.h"
#include "manifest.h"
#include "wal.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static const FDBManifestLimits limits = {8, 2048, 16, 3};

typedef struct Fault {
    FDBFsOp operation;
    size_t countdown;
} Fault;

static FDBFsFault inject(void *context, FDBFsOp operation, size_t requested)
{
    (void)requested;
    Fault *fault = context;
    if (operation == fault->operation && fault->countdown != 0 && --fault->countdown == 0)
        return (FDBFsFault){.error = EIO};
    return (FDBFsFault){0};
}

static FDBManifest snapshot(const FDBDirectory *dir)
{
    FDBManifestLoad loaded;
    OK(fdb_manifest_load(dir, &limits, &loaded));
    CHECK(loaded.view.count == 0);
    FDBManifest content = loaded.view;
    content.entries = NULL;
    fdb_manifest_release(&loaded);
    return content;
}

static void check_catalog(const FDBDirectory *dir, uint64_t generation, uint64_t id,
                           FDBFileKind kind)
{
    FDBManifestLoad loaded;
    OK(fdb_manifest_load(dir, &limits, &loaded));
    const FDBManifest *view = &loaded.view;
    CHECK(view->generation == generation && view->count == 1);
    CHECK(view->entries[0].id == id && view->entries[0].kind == kind);
    if (kind == FDB_FILE_SSTABLE) {
        CHECK(view->entries[0].min_key.size == 2 && view->entries[0].max_key.size == 2);
        CHECK(memcmp(view->entries[0].min_key.data, "\0a", 2) == 0);
        CHECK(memcmp(view->entries[0].max_key.data, "\xff\0", 2) == 0);
        CHECK(view->entries[0].sequence_begin == 1 && view->entries[0].sequence_end == 4);
    }
    fdb_manifest_release(&loaded);
}

static void test_catalog(FDBDirectory *dir, Fault *fault, const char *path)
{
    FDBManifestLoad loaded;
    STATUS(fdb_manifest_load(dir, &limits, &loaded), FASTDB_NOT_FOUND);
    FDBManifest current;
    OK(fdb_manifest_new(FDB_MODE_LSM, &current));
    OK(fdb_manifest_publish(dir, &limits, 0, &current));
    STATUS(fdb_manifest_publish(dir, &limits, 0, &current), FASTDB_CONFLICT);
    FDBManifest observed = snapshot(dir);
    CHECK(observed.generation == 1 && observed.sequence == 0 && observed.count == 0);
    CHECK(memcmp(current.database_id, observed.database_id, sizeof(current.database_id)) == 0);

    uint64_t id;
    *fault = (Fault){FDB_FS_RENAME, 1};
    STATUS(fdb_manifest_reserve_file(dir, &limits, &current, &id), FASTDB_IO_ERROR);
    CHECK(id == 0 && snapshot(dir).generation == 1);
    OK(fdb_manifest_reserve_file(dir, &limits, &current, &id));
    CHECK(id == 1 && current.generation == 2 && current.next_file_id == 2);
    OK(fdb_dir_close(dir));

    const FDBFs fs = {inject, fault};
    OK(fdb_dir_lock(path, &fs, dir));
    current = snapshot(dir);
    OK(fdb_manifest_reserve_file(dir, &limits, &current, &id));
    CHECK(id == 2 && current.generation == 3 && current.next_file_id == 3);

    const FDBWalLimits wal_limits = {16, 32, 8, 256, 65536};
    FDBWal wal;
    OK(fdb_wal_create(dir, "wal.2", id, current.sequence, &wal_limits, &wal));
    OK(fdb_wal_close(&wal));
    FDBManifestEntry input = {.id = id, .kind = FDB_FILE_WAL, .bytes = FDB_WAL_HEADER_BYTES};
    FDBManifest next = current;
    ++next.generation;
    next.entries = &input;
    next.count = 1;
    OK(fdb_manifest_publish(dir, &limits, current.generation, &next));
    current = next;
    check_catalog(dir, 4, input.id, FDB_FILE_WAL);

    OK(fdb_manifest_reserve_file(dir, &limits, &current, &id));
    CHECK(id == 3 && current.generation == 5);
    FDBManifestEntry output = {
        .id = id, .kind = FDB_FILE_SSTABLE, .level = 1, .bytes = 32, .sequence_begin = 1,
        .sequence_end = 4, .min_key = {"\0a", 2}, .max_key = {"\xff\0", 2},
    };
    next = current;
    ++next.generation;
    next.sequence = 4;
    next.entries = &output;
    *fault = (Fault){FDB_FS_SYNC, 1};
    STATUS(fdb_manifest_publish(dir, &limits, current.generation, &next), FASTDB_IO_ERROR);
    check_catalog(dir, 5, input.id, FDB_FILE_WAL);
    *fault = (Fault){FDB_FS_SYNC, 2};
    STATUS(fdb_manifest_publish(dir, &limits, current.generation, &next), FASTDB_COMMIT_UNCERTAIN);
    check_catalog(dir, 6, output.id, FDB_FILE_SSTABLE);
    STATUS(fdb_manifest_publish(dir, &limits, current.generation, &next), FASTDB_CONFLICT);
    int original;
    OK(fdb_dir_open_file(dir, "wal.2", FDB_FILE_READ, &original));
    OK(fdb_fs_close(original));

    FDBManifestLimits too_small = limits;
    too_small.max_bytes = 72;
    STATUS(fdb_manifest_load(dir, &too_small, &loaded), FASTDB_LIMIT_EXCEEDED);
    CHECK(loaded.entries == NULL && loaded.image == NULL);

    next.generation = 7;
    next.sequence = 3;
    STATUS(fdb_manifest_publish(dir, &limits, 6, &next), FASTDB_INVALID_ARGUMENT);
    next.sequence = 4;
    next.mode = FDB_MODE_MEMORY_PERSISTENT;
    next.count = 0;
    next.entries = NULL;
    STATUS(fdb_manifest_publish(dir, &limits, 6, &next), FASTDB_CONFLICT);
    OK(fdb_dir_unlink(dir, "wal.2"));
}

static void damage_catalog(const FDBDirectory *dir)
{
    int fd;
    OK(fdb_dir_open_file(dir, "MANIFEST", FDB_FILE_READ_WRITE, &fd));
    uint64_t size;
    OK(fdb_fs_size(fd, &size));
    unsigned char byte;
    size_t transferred;
    OK(fdb_fs_read(NULL, fd, size - 1, &byte, 1, &transferred));
    CHECK(transferred == 1);
    byte ^= 1;
    OK(fdb_fs_write(NULL, fd, size - 1, &byte, 1, &transferred));
    OK(fdb_fs_close(fd));
    FDBManifestLoad loaded;
    STATUS(fdb_manifest_load(dir, &limits, &loaded), FASTDB_CORRUPTION);
    CHECK(loaded.image == NULL && loaded.entries == NULL);
}

static void reserve_range(FDBDirectory *dir, Fault *fault)
{
    FDBManifest current;
    OK(fdb_manifest_new(FDB_MODE_LSM, &current));
    OK(fdb_manifest_publish(dir, &limits, 0, &current));
    uint64_t first = 123;
    STATUS(fdb_manifest_reserve_files(dir, &limits, &current, 0, &first), FASTDB_INVALID_ARGUMENT);
    CHECK(first == 0 && current.next_file_id == 1);
    *fault = (Fault){FDB_FS_RENAME, 1};
    STATUS(fdb_manifest_reserve_files(dir, &limits, &current, 5, &first), FASTDB_IO_ERROR);
    CHECK(first == 0 && snapshot(dir).next_file_id == 1);
    OK(fdb_manifest_reserve_files(dir, &limits, &current, 5, &first));
    CHECK(first == 1 && current.next_file_id == 6 && current.generation == 2);

    *fault = (Fault){FDB_FS_SYNC, 2};
    STATUS(fdb_manifest_reserve_files(dir, &limits, &current, 5, &first), FASTDB_COMMIT_UNCERTAIN);
    CHECK(first == 0 && current.next_file_id == 6);
    current = snapshot(dir);
    CHECK(current.next_file_id == 11 && current.generation == 3);
    OK(fdb_manifest_reserve_file(dir, &limits, &current, &first));
    CHECK(first == 11 && current.next_file_id == 12);

    FDBManifest near_limit = current;
    ++near_limit.generation;
    near_limit.next_file_id = UINT64_MAX - 2;
    OK(fdb_manifest_publish(dir, &limits, current.generation, &near_limit));
    STATUS(fdb_manifest_reserve_files(dir, &limits, &near_limit, 3, &first), FASTDB_LIMIT_EXCEEDED);
    OK(fdb_manifest_reserve_files(dir, &limits, &near_limit, 2, &first));
    CHECK(first == UINT64_MAX - 2 && near_limit.next_file_id == UINT64_MAX);
    STATUS(fdb_manifest_reserve_file(dir, &limits, &near_limit, &first), FASTDB_LIMIT_EXCEEDED);
    CHECK(first == 0 && snapshot(dir).next_file_id == UINT64_MAX);
}

int main(void)
{
    char path[] = "/tmp/fastdb-manifest.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    Fault fault = {0};
    const FDBFs fs = {inject, &fault};
    FDBDirectory dir;
    OK(fdb_dir_lock(path, &fs, &dir));
    test_catalog(&dir, &fault, path);
    damage_catalog(&dir);
    OK(fdb_dir_unlink(&dir, "MANIFEST"));
    reserve_range(&dir, &fault);
    OK(fdb_dir_unlink(&dir, "MANIFEST"));
    OK(fdb_dir_close(&dir));
    char lock_path[sizeof(path) + sizeof("/.fastdb.lock")];
    CHECK(snprintf(lock_path, sizeof(lock_path), "%s/.fastdb.lock", path) > 0);
    CHECK(unlink(lock_path) == 0 && rmdir(path) == 0);
}
