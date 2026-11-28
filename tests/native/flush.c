#include "disk_fixture.h"
#include "flush.h"
#include "recovery.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static const FDBManifestLimits manifest_limits = {16, 4096, 16, 3};
static const FDBWalLimits wal_limits = {16, 64, 8, 200, 256};
static const FDBSSTLimits sst_limits = {16, 64, 80, 128, 512, 128, 10, false};

typedef struct Fault {
    FDBFsOp op;
    size_t countdown;
} Fault;

static FDBFsFault reject(void *context, FDBFsOp op, size_t requested)
{
    (void)requested;
    Fault *fault = context;
    if (fault->op == op && fault->countdown != 0 && --fault->countdown == 0)
        return (FDBFsFault){.error = EIO};
    return (FDBFsFault){0};
}

static void apply(FDBTree *tree, const FDBRecord *record)
{
    size_t bytes;
    CHECK(fdb_node_size(record->key, record->value, &bytes));
    FDBNode *node = fdb_node_create(record, bytes);
    CHECK(node != NULL);
    fdb_tree_put(tree, node);
}

static FastDBStatus replay(void *context, const unsigned char *payload, size_t length,
                            size_t records, uint64_t first)
{
    FDBTree *tree = context;
    for (size_t i = 0; i < records; ++i) {
        FDBRecord record;
        size_t consumed;
        OK(fdb_record_decode(payload, length, wal_limits.max_key_bytes, wal_limits.max_value_bytes,
                             &record, &consumed));
        CHECK(record.sequence == first + i);
        apply(tree, &record);
        payload += consumed;
        length -= consumed;
    }
    CHECK(length == 0);
    return FASTDB_OK;
}

static void check_table(const FDBDirectory *dir, const char *name, uint64_t id, uint64_t sequence)
{
    FDBSSTReader *reader;
    OK(fdb_sst_reader_open(dir, name, id, &sst_limits, &reader));
    OK(fdb_sst_reader_verify(reader));
    const FDBSSTProperties *props = fdb_sst_reader_properties(reader);
    CHECK(props->max_sequence == sequence);
    if (sequence == 3) {
        CHECK(props->records == 2 && props->tombstones == 1);
        FDBSSTIterator *it;
        OK(fdb_sst_iter_open(reader, &it));
        fdb_sst_iter_seek_version(it, (FastDBSlice){"k", 1}, 3);
        OK(fdb_sst_iter_status(it));
        const FDBRecord *record = fdb_sst_iter_current(it);
        CHECK(record != NULL && record->kind == FDB_DELETE && record->sequence == 3);
        fdb_sst_iter_close(it);
    }
    OK(fdb_sst_reader_close(reader));
}

static void verify_flush(const FDBDirectory *dir, Fault *fault)
{
    FDBManifest manifest;
    OK(fdb_manifest_new(FDB_MODE_LSM, &manifest));
    OK(fdb_manifest_publish(dir, &manifest_limits, 0, &manifest));
    FDBWalSet set;
    FDBTree tree = {0};
    OK(fdb_lsm_recover(dir, &manifest_limits, &wal_limits, &sst_limits, &(FDBWalReplay){.visit = replay, .context = &tree}, &set));
    OK(fdb_flush_l0(&set, &tree, 0, &sst_limits));
    const FDBRecord first[] = {
        {{"k", 1}, {"value", 5}, 1, 0, FDB_PUT},
        {{"abc", 3}, {"second", 6}, 2, 0, FDB_PUT},
        {{"k", 1}, {0}, 3, 0, FDB_DELETE},
    };
    OK(fdb_wal_set_append(&set, first, 3, true));
    for (size_t i = 0; i < 3; ++i)
        apply(&tree, &first[i]);

    *fault = (Fault){FDB_FS_RENAME, 2};
    STATUS(fdb_flush_l0(&set, &tree, 3, &sst_limits), FASTDB_IO_ERROR);
    CHECK(tree.count == 2 && set.catalog.view.count == 1);
    CHECK(set.catalog.view.entries[0].kind == FDB_FILE_WAL);
    OK(fdb_wal_set_close(&set));
    fdb_tree_destroy(&tree);
    OK(fdb_wal_set_open(dir, &manifest_limits, &wal_limits, 0, &(FDBWalReplay){.visit = replay, .context = &tree}, &set));
    CHECK(tree.count == 2 && fdb_tree_get(&tree, (FastDBSlice){"k", 1})->record.kind == FDB_DELETE);

    *fault = (Fault){FDB_FS_SYNC, 6};
    STATUS(fdb_flush_l0(&set, &tree, 3, &sst_limits), FASTDB_COMMIT_UNCERTAIN);
    CHECK(tree.count == 2 && set.poisoned);
    STATUS(fdb_wal_set_close(&set), FASTDB_COMMIT_UNCERTAIN);
    fdb_tree_destroy(&tree);

    int fd;
    OK(fdb_dir_open_file(dir, "wal-00000000000000000001", FDB_FILE_READ, &fd));
    OK(fdb_fs_close(fd));
    check_table(dir, "sst-00000000000000000003", 3, 3);
    *fault = (Fault){FDB_FS_SYNC, 1};
    STATUS(fdb_lsm_recover(dir, &manifest_limits, &wal_limits, &sst_limits, &(FDBWalReplay){.visit = replay, .context = &tree}, &set),
           FASTDB_IO_ERROR);
    OK(fdb_dir_open_file(dir, "wal-00000000000000000001", FDB_FILE_READ, &fd));
    OK(fdb_fs_close(fd));

    const char *unreserved = "sst-00000000000000000999";
    OK(fdb_dir_open_file(dir, unreserved, FDB_FILE_CREATE_NEW, &fd));
    OK(fdb_fs_close(fd));
    OK(fdb_lsm_recover(dir, &manifest_limits, &wal_limits, &sst_limits, &(FDBWalReplay){.visit = replay, .context = &tree}, &set));
    STATUS(fdb_dir_open_file(dir, "wal-00000000000000000001", FDB_FILE_READ, &fd), FASTDB_NOT_FOUND);
    STATUS(fdb_dir_open_file(dir, ".sst-00000000000000000002.tmp", FDB_FILE_READ, &fd),
           FASTDB_NOT_FOUND);
    OK(fdb_dir_open_file(dir, unreserved, FDB_FILE_READ, &fd));
    OK(fdb_fs_close(fd));
    CHECK(tree.count == 0 && set.catalog.view.count == 1);
    CHECK(set.catalog.view.entries[0].id == 3);

    const FDBRecord next = {{"b", 1}, {"next", 4}, 4, 0, FDB_PUT};
    OK(fdb_wal_set_append(&set, &next, 1, true));
    apply(&tree, &next);
    OK(fdb_flush_l0(&set, &tree, 4, &sst_limits));
    CHECK(tree.count == 0 && set.catalog.view.count == 2);
    const FDBManifestEntry *first_table = &set.catalog.view.entries[0];
    CHECK(first_table->id == 3 && first_table->min_key.size == 3);
    CHECK(memcmp(first_table->min_key.data, "abc", 3) == 0);
    CHECK(set.catalog.view.entries[1].id == 5 && set.catalog.view.sequence == 4);
    check_table(dir, "sst-00000000000000000005", 5, 4);
    OK(fdb_wal_set_close(&set));
    OK(fdb_dir_unlink(dir, "sst-00000000000000000005"));
    STATUS(fdb_lsm_recover(dir, &manifest_limits, &wal_limits, &sst_limits, &(FDBWalReplay){.visit = replay, .context = &tree}, &set),
           FASTDB_CORRUPTION);
    OK(fdb_dir_open_file(dir, "wal-00000000000000000004", FDB_FILE_READ, &fd));
    OK(fdb_fs_close(fd));
}

static void verify_sealed_prefix(bool uncertain)
{
    char path[] = "/tmp/fastdb-flush-prefix.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    Fault fault = {0};
    const FDBFs fs = {reject, &fault};
    FDBDirectory dir;
    OK(fdb_dir_lock(path, &fs, &dir));
    FDBManifest manifest;
    OK(fdb_manifest_new(FDB_MODE_LSM, &manifest));
    OK(fdb_manifest_publish(&dir, &manifest_limits, 0, &manifest));
    FDBTree frozen = {0}, active = {0};
    const FDBWalReplay restore = {.visit = replay, .context = &active};
    FDBWalSet set;
    OK(fdb_lsm_recover(&dir, &manifest_limits, &wal_limits, &sst_limits, &restore, &set));
    const FDBRecord first[] = {
        {{"k", 1}, {"old", 3}, 1, 0, FDB_PUT},
        {{"frozen", 6}, {"safe", 4}, 2, 0, FDB_PUT},
    };
    OK(fdb_wal_set_append(&set, first, 2, true));
    apply(&frozen, &first[0]);
    apply(&frozen, &first[1]);
    fault = (Fault){FDB_FS_RENAME, 2};
    STATUS(fdb_wal_set_rotate(&set), FASTDB_IO_ERROR);
    CHECK(fault.countdown == 0 && set.wal.sequence == 2 && set.active_index == 0);
    OK(fdb_wal_set_rotate(&set));
    const FDBRecord newer[] = {
        {{"k", 1}, {"new", 3}, 3, 0, FDB_PUT},
        {{"other", 5}, {"new", 3}, 4, 0, FDB_PUT},
    };
    OK(fdb_wal_set_append(&set, newer, 2, true));
    OK(fdb_wal_set_rotate(&set));
    const FDBRecord deletion = {{"other", 5}, {0}, 5, 0, FDB_DELETE};
    OK(fdb_wal_set_append(&set, &deletion, 1, true));
    CHECK(set.catalog.view.sequence == 4 && set.wal.sequence == 5 && set.active_index == 2);
    uint64_t generation = set.catalog.view.generation;
    uint64_t active_id = set.catalog.view.entries[set.active_index].id;
    STATUS(fdb_flush_l0(&set, &frozen, 1, &sst_limits), FASTDB_INVALID_ARGUMENT);
    CHECK(set.catalog.view.generation == generation);
    if (uncertain) {
        fault = (Fault){FDB_FS_SYNC, 6};
        STATUS(fdb_flush_l0(&set, &frozen, 2, &sst_limits), FASTDB_COMMIT_UNCERTAIN);
        CHECK(fault.countdown == 0 && frozen.count == 2 && set.active_index == 2);
        STATUS(fdb_wal_set_close(&set), FASTDB_COMMIT_UNCERTAIN);
    } else {
        int fd = set.wal.fd;
        OK(fdb_flush_l0(&set, &frozen, 2, &sst_limits));
        CHECK(frozen.count == 0 && set.active_index == 1 && set.wal.fd == fd);
        CHECK(set.catalog.view.sequence == 4 && set.wal.sequence == 5);
        OK(fdb_wal_set_close(&set));
    }
    fdb_tree_destroy(&frozen);

    OK(fdb_lsm_recover(&dir, &manifest_limits, &wal_limits, &sst_limits, &restore, &set));
    CHECK(set.active_index == 1 && set.catalog.view.entries[set.active_index].id == active_id);
    CHECK(set.catalog.view.count == 3 && set.catalog.view.sequence == 4 && set.wal.sequence == 5);
    CHECK(active.count == 2 && fdb_tree_get(&active, first[1].key) == NULL);
    CHECK(fdb_tree_get(&active, newer[0].key)->record.sequence == 3);
    CHECK(fdb_tree_get(&active, deletion.key)->record.kind == FDB_DELETE);
    const FDBManifestEntry *table = &set.catalog.view.entries[2];
    CHECK(table->kind == FDB_FILE_SSTABLE && table->sequence_end == 2);
    FDBSSTReader *reader;
    OK(fdb_catalog_open_table(&dir, table, &sst_limits, &reader));
    CHECK(fdb_sst_reader_properties(reader)->records == 2);
    OK(fdb_sst_reader_close(reader));
    const FDBRecord later = {{"later", 5}, {0}, 6, 0, FDB_PUT};
    OK(fdb_wal_set_append(&set, &later, 1, true));
    apply(&active, &later);
    OK(fdb_flush_l0(&set, &active, 6, &sst_limits));
    CHECK(set.active_index == SIZE_MAX && set.catalog.view.count == 2 && active.count == 0);
    OK(fdb_wal_set_close(&set));
    OK(fdb_dir_close(&dir));
    remove_disk_fixture(path);
}

int main(void)
{
    char path[] = "/tmp/fastdb-flush.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    Fault fault = {0};
    const FDBFs fs = {reject, &fault};
    FDBDirectory dir;
    OK(fdb_dir_lock(path, &fs, &dir));
    verify_flush(&dir, &fault);
    static const char *const files[] = {
        "sst-00000000000000000003", "wal-00000000000000000004",
        "sst-00000000000000000999", "MANIFEST",
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
        OK(fdb_dir_unlink(&dir, files[i]));
    OK(fdb_dir_close(&dir));
    char lock_path[sizeof(path) + sizeof("/.fastdb.lock")];
    CHECK(snprintf(lock_path, sizeof(lock_path), "%s/.fastdb.lock", path) > 0);
    CHECK(unlink(lock_path) == 0 && rmdir(path) == 0);
    verify_sealed_prefix(false);
    verify_sealed_prefix(true);
}
