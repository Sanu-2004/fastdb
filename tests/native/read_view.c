#include "disk_fixture.h"
#include "engine.h"
#include "manifest.h"

#include <errno.h>
#include <inttypes.h>

typedef struct ReadGate {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t foreground;
    unsigned writes_until_pause;
    bool entered;
    bool release;
} ReadGate;

typedef struct ReadRequest {
    FastDB *db;
    FastDBStatus expected;
    FastDBSlice value;
    uint64_t revision;
} ReadRequest;

static FDBFsFault hold_open(void *context, FDBFsOp operation, size_t size)
{
    (void)size;
    ReadGate *gate = context;
    if (pthread_equal(pthread_self(), gate->foreground))
        return (FDBFsFault){0};
    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    bool pause = gate->writes_until_pause == 0 ? operation == FDB_FS_OPEN :
                 operation == FDB_FS_WRITE && --gate->writes_until_pause == 0;
    if (pause && !gate->entered) {
        gate->entered = true;
        CHECK(pthread_cond_signal(&gate->condition) == 0);
        while (!gate->release)
            CHECK(pthread_cond_wait(&gate->condition, &gate->mutex) == 0);
    }
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
    return (FDBFsFault){0};
}

static void *read_k(void *arg)
{
    ReadRequest *request = arg;
    void *value;
    size_t size;
    uint64_t revision;
    STATUS(fastdb_get_with_revision(request->db, "k", 1, &value, &size, &revision), request->expected);
    if (request->expected == FASTDB_OK) {
        CHECK(revision == request->revision && size == request->value.size);
        CHECK(memcmp(value, request->value.data, size) == 0);
    } else {
        CHECK(value == NULL && size == 0 && revision == 0);
    }
    fastdb_free(value);
    return NULL;
}

static void wait_read(ReadGate *gate)
{
    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    while (!gate->entered)
        CHECK(pthread_cond_wait(&gate->condition, &gate->mutex) == 0);
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void finish_read(ReadGate *gate, pthread_t reader)
{
    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    gate->release = true;
    CHECK(pthread_cond_signal(&gate->condition) == 0);
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
    CHECK(pthread_join(reader, NULL) == 0);
}

static void destroy_gate(ReadGate *gate)
{
    CHECK(pthread_cond_destroy(&gate->condition) == 0);
    CHECK(pthread_mutex_destroy(&gate->mutex) == 0);
}

static void collect_tables(const char *path, char names[2][25])
{
    DIR *dir = opendir(path);
    CHECK(dir != NULL);
    size_t count = 0;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "sst-", 4) == 0) {
            CHECK(count < 2 && strlen(entry->d_name) == 24);
            memcpy(names[count++], entry->d_name, sizeof(*names));
        }
    }
    CHECK(count == 2 && closedir(dir) == 0);
}

static void check_old_files(const char *path, char names[2][25], bool present)
{
    DIR *dir = opendir(path);
    CHECK(dir != NULL);
    for (size_t i = 0; i < 2; ++i) {
        int exists = faccessat(dirfd(dir), names[i], F_OK, 0);
        CHECK(present ? exists == 0 : exists < 0 && errno == ENOENT);
    }
    CHECK(closedir(dir) == 0);
}

static void across_compaction(bool start_with_tombstone)
{
    char path[] = "/tmp/fastdb-read-view.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDBCompactionOptions policy;
    fastdb_compaction_options_default(&policy);
    policy.max_levels = 2;
    OK(fastdb_options_set_compaction(options, &policy));

    ReadGate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                     .foreground = pthread_self()};
    const FDBFs fs = {hold_open, &gate};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    OK(fastdb_put(db, "k", 1, "old", 3));
    OK(fastdb_flush(db));
    OK(fastdb_compact(db));
    if (start_with_tombstone)
        OK(fastdb_delete(db, "k", 1));
    else
        OK(fastdb_put(db, "k", 1, "middle", 6));
    OK(fastdb_flush(db));

    char old_names[2][25];
    collect_tables(path, old_names);
    FastDBStats before, during;
    OK(fastdb_stats(db, &before));
    ReadRequest request = {db, start_with_tombstone ? FASTDB_NOT_FOUND : FASTDB_OK, {"middle", 6}, 2};
    pthread_t reader;
    CHECK(pthread_create(&reader, NULL, read_k, &request) == 0);
    wait_read(&gate);
    OK(fastdb_stats(db, &during));
    CHECK(during.memory_bytes > before.memory_bytes && during.memory_bytes <= 128 * 1024);

    OK(fastdb_compact(db));
    check_old_files(path, old_names, true);
    if (start_with_tombstone)
        expect_missing(db, (FastDBSlice){"k", 1});
    else
        expect_value(db, (FastDBSlice){"k", 1}, (FastDBSlice){"middle", 6}, 2);
    OK(fastdb_put(db, "k", 1, "new", 3));
    expect_value(db, (FastDBSlice){"k", 1}, (FastDBSlice){"new", 3}, 3);
    STATUS(fastdb_flush(db), FASTDB_BUSY);
    STATUS(fastdb_close(db), FASTDB_BUSY);

    finish_read(&gate, reader);
    OK(fastdb_flush(db));
    check_old_files(path, old_names, false);
    OK(fastdb_close(db));
    destroy_gate(&gate);
    OK(fastdb_open(options, &db));
    expect_value(db, (FastDBSlice){"k", 1}, (FastDBSlice){"new", 3}, 3);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void wait_worker_flush(FastDB *db)
{
    CHECK(pthread_mutex_lock(&db->mutex) == 0);
    while ((db->tree.count != 0 || db->flush->frozen.count != 0) && db->flush->error == FASTDB_OK)
        CHECK(pthread_cond_wait(&db->flush->condition, &db->mutex) == 0);
    CHECK(db->flush->error == FASTDB_OK);
    CHECK(pthread_mutex_unlock(&db->mutex) == 0);
}

static void across_background_flush(void)
{
    char path[] = "/tmp/fastdb-read-worker.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "k", 1, "old", 3));
    OK(fastdb_flush(db));
    OK(fastdb_close(db));

    size_t threshold;
    CHECK(fdb_node_size((FastDBSlice){"w", 1}, (FastDBSlice){0}, &threshold));
    OK(fastdb_options_set_lsm_flush_threshold(options, threshold));
    ReadGate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                     .foreground = pthread_self()};
    const FDBFs fs = {hold_open, &gate};
    OK(fdb_open_with_fs(options, &fs, &db));
    ReadRequest request = {db, FASTDB_OK, {"old", 3}, 1};
    pthread_t reader;
    CHECK(pthread_create(&reader, NULL, read_k, &request) == 0);
    wait_read(&gate);
    OK(fastdb_put(db, "w", 1, NULL, 0));
    wait_worker_flush(db);
    expect_value(db, (FastDBSlice){"w", 1}, (FastDBSlice){0}, 2);
    OK(fastdb_put(db, "x", 1, NULL, 0));
    STATUS(fastdb_put(db, "y", 1, NULL, 0), FASTDB_BUSY);
    STATUS(fastdb_flush(db), FASTDB_BUSY);

    finish_read(&gate, reader);
    wait_worker_flush(db);
    expect_value(db, (FastDBSlice){"x", 1}, (FastDBSlice){0}, 3);
    expect_missing(db, (FastDBSlice){"y", 1});
    OK(fastdb_close(db));
    destroy_gate(&gate);
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void *compact_db(void *arg)
{
    OK(fastdb_compact(arg));
    return NULL;
}

static void writes_during_compaction(void)
{
    char path[] = "/tmp/fastdb-compaction-write.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDBCompactionOptions policy;
    fastdb_compaction_options_default(&policy);
    policy.max_levels = 2;
    OK(fastdb_options_set_compaction(options, &policy));
    ReadGate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                     .foreground = pthread_self(), .writes_until_pause = 2};
    const FDBFs fs = {hold_open, &gate};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    OK(fastdb_put(db, "k", 1, "old", 3));
    OK(fastdb_flush(db));
    pthread_t compactor;
    CHECK(pthread_create(&compactor, NULL, compact_db, db) == 0);
    wait_read(&gate);
    expect_value(db, (FastDBSlice){"k", 1}, (FastDBSlice){"old", 3}, 1);
    char large[120] = {1};
    OK(fastdb_put(db, "a", 1, large, sizeof(large)));
    OK(fastdb_put(db, "b", 1, large, sizeof(large)));
    OK(fastdb_put(db, "c", 1, large, sizeof(large)));
    OK(fastdb_delete(db, "k", 1));
    STATUS(fastdb_compact(db), FASTDB_BUSY);
    STATUS(fastdb_flush(db), FASTDB_BUSY);
    STATUS(fastdb_close(db), FASTDB_BUSY);
    finish_read(&gate, compactor);
    OK(fastdb_put(db, "d", 1, NULL, 0));
    OK(fastdb_close(db));
    destroy_gate(&gate);
    OK(fastdb_open(options, &db));
    expect_missing(db, (FastDBSlice){"k", 1});
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){large, sizeof(large)}, 2);
    expect_value(db, (FastDBSlice){"c", 1}, (FastDBSlice){large, sizeof(large)}, 4);
    expect_value(db, (FastDBSlice){"d", 1}, (FastDBSlice){0}, 6);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void snapshot_during_mask_reclamation(void)
{
    char path[] = "/tmp/fastdb-mask-read.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDBCompactionOptions policy;
    fastdb_compaction_options_default(&policy);
    policy.max_levels = 2;
    OK(fastdb_options_set_compaction(options, &policy));
    ReadGate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                     .foreground = pthread_self(), .writes_until_pause = 2};
    const FDBFs fs = {hold_open, &gate};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    OK(fastdb_put(db, "a", 1, "old", 3));
    OK(fastdb_flush(db));
    OK(fastdb_compact(db));
    OK(fastdb_delete(db, "a", 1));
    OK(fastdb_put(db, "z", 1, "guard", 5));
    OK(fastdb_flush(db));
    char old_names[2][25];
    collect_tables(path, old_names);

    pthread_t compactor;
    CHECK(pthread_create(&compactor, NULL, compact_db, db) == 0);
    wait_read(&gate);
    FastDBSnapshot *snapshot;
    OK(fastdb_snapshot_create(db, &snapshot));
    CHECK(fastdb_snapshot_revision(snapshot) == 3);
    OK(fastdb_put(db, "a", 1, "fresh", 5));
    finish_read(&gate, compactor);

    check_old_files(path, old_names, true);
    void *value;
    size_t size;
    uint64_t revision;
    STATUS(fastdb_snapshot_get(snapshot, "a", 1, &value, &size, &revision), FASTDB_NOT_FOUND);
    CHECK(value == NULL && size == 0 && revision == 0);
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"fresh", 5}, 4);
    fastdb_snapshot_release(snapshot);
    OK(fastdb_close(db));
    check_old_files(path, old_names, false);
    destroy_gate(&gate);
    OK(fastdb_open(options, &db));
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"fresh", 5}, 4);
    expect_value(db, (FastDBSlice){"z", 1}, (FastDBSlice){"guard", 5}, 3);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static uint64_t verify_merge_levels(const char *path, const FastDBOptions *options,
                                    bool after_merge)
{
    FDBDirectory directory;
    OK(fdb_dir_lock(path, NULL, &directory));
    const FDBManifestLimits limits = {options->storage.max_catalog_files,
                                      options->storage.max_catalog_bytes,
                                      options->limits.max_key_bytes, 3};
    FDBManifestLoad catalog;
    OK(fdb_manifest_load(&directory, &limits, &catalog));
    size_t counts[3] = {0};
    uint64_t bytes = 0;
    for (size_t i = 0; i < catalog.view.count; ++i) {
        const FDBManifestEntry *entry = &catalog.view.entries[i];
        if (entry->kind != FDB_FILE_SSTABLE)
            continue;
        CHECK(entry->level < 3);
        ++counts[entry->level];
        bytes += entry->bytes;
    }
    CHECK(counts[0] == 0 && counts[1] == (after_merge ? 0U : 1U) && counts[2] == 1);
    fdb_manifest_release(&catalog);
    OK(fdb_dir_close(&directory));
    return bytes;
}

static void merge_larger_than_memory(void)
{
    enum { records_per_flush = 14, flushes = 10, records = records_per_flush * flushes };
    const size_t memory_cap = 256 * 1024;
    const uint64_t initial_revision = records + flushes;
    char path[] = "/tmp/fastdb-large-merge.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options;
    OK(fastdb_options_create(&options));
    FastDBLimits limits;
    fastdb_limits_default(&limits);
    limits.max_memory_bytes = memory_cap;
    limits.max_key_bytes = 32;
    unsigned char payload[2048];
    memset(payload, 0xDB, sizeof(payload));
    limits.max_value_bytes = sizeof(payload);
    OK(fastdb_options_set_limits(options, &limits));
    FastDBPersistentOptions storage;
    fastdb_persistent_options_default(&storage);
    storage.max_wal_segment_bytes = 64 * 1024;
    storage.max_wal_frame_bytes = 8 * 1024;
    storage.target_block_bytes = 4 * 1024;
    storage.max_index_bytes = 12 * 1024;
    storage.max_filter_bytes = 8 * 1024;
    storage.max_catalog_files = 16;
    storage.max_catalog_bytes = 2048;
    OK(fastdb_options_set_storage(options, FASTDB_MODE_LSM, path, &storage));
    FastDBCompactionOptions policy;
    fastdb_compaction_options_default(&policy);
    policy.max_levels = 3;
    policy.level0_file_trigger = 1;
    policy.level1_bytes = 512 * 1024;
    policy.target_file_bytes = 512 * 1024;
    OK(fastdb_options_set_compaction(options, &policy));

    FastDB *db;
    OK(fastdb_open(options, &db));
    for (unsigned int i = 0; i < records; ++i) {
        const unsigned char key[] = {'k', (unsigned char)i};
        payload[0] = (unsigned char)i;
        OK(fastdb_put(db, key, sizeof(key), payload, sizeof(payload)));
        if ((i + 1) % records_per_flush == 0) {
            /* Keep successive runs overlapping as the last-level table grows. */
            OK(fastdb_put(db, "z", 1, NULL, 0));
            OK(fastdb_flush(db));
            OK(fastdb_compact(db));
        }
    }
    const unsigned char update[] = {'k', 0}, deleted[] = {'k', 1}, concurrent[] = {'k', 2};
    OK(fastdb_put(db, update, sizeof(update), "before", 6));
    OK(fastdb_delete(db, deleted, sizeof(deleted)));
    /* Automatic promotion leaves the new run in L1, overlapping the L2 run. */
    OK(fastdb_flush(db));
    OK(fastdb_close(db));
    uint64_t input_bytes = verify_merge_levels(path, options, false);
    CHECK(input_bytes > memory_cap);

    ReadGate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                     .foreground = pthread_self(), .writes_until_pause = 2};
    const FDBFs fs = {hold_open, &gate};
    OK(fdb_open_with_fs(options, &fs, &db));
    FastDBSnapshot *snapshot;
    OK(fastdb_snapshot_create(db, &snapshot));
    CHECK(fastdb_snapshot_revision(snapshot) == initial_revision + 2);
    FastDBStats before, merging, during;
    OK(fastdb_stats(db, &before));
    pthread_t compactor;
    CHECK(pthread_create(&compactor, NULL, compact_db, db) == 0);
    wait_read(&gate);
    OK(fastdb_stats(db, &merging));
    CHECK(merging.memory_bytes > before.memory_bytes && merging.memory_bytes <= memory_cap);
    expect_value(db, (FastDBSlice){update, sizeof(update)}, (FastDBSlice){"before", 6},
                 initial_revision + 1);
    expect_missing(db, (FastDBSlice){deleted, sizeof(deleted)});
    OK(fastdb_put(db, update, sizeof(update), "during", 6));
    OK(fastdb_delete(db, concurrent, sizeof(concurrent)));
    OK(fastdb_stats(db, &during));
    CHECK(during.record_count == 2 && during.memory_bytes > merging.memory_bytes &&
          during.memory_bytes <= memory_cap);
    finish_read(&gate, compactor);

    void *value;
    size_t size;
    uint64_t revision;
    OK(fastdb_snapshot_get(snapshot, update, sizeof(update), &value, &size, &revision));
    CHECK(size == 6 && memcmp(value, "before", 6) == 0 && revision == initial_revision + 1);
    fastdb_free(value);
    OK(fastdb_snapshot_get(snapshot, concurrent, sizeof(concurrent), &value, &size, &revision));
    payload[0] = 2;
    CHECK(size == sizeof(payload) && memcmp(value, payload, size) == 0 && revision == 3);
    fastdb_free(value);
    fastdb_snapshot_release(snapshot);
    expect_value(db, (FastDBSlice){update, sizeof(update)}, (FastDBSlice){"during", 6},
                 initial_revision + 3);
    expect_missing(db, (FastDBSlice){concurrent, sizeof(concurrent)});
    OK(fastdb_close(db));
    destroy_gate(&gate);
    uint64_t output_bytes = verify_merge_levels(path, options, true);
    CHECK(output_bytes > memory_cap);
    printf("streaming merge bytes: inputs=%" PRIu64 " output=%" PRIu64
           " memory before=%zu merge=%zu with foreground writes=%zu limit=%zu\n",
           input_bytes, output_bytes, before.memory_bytes, merging.memory_bytes,
           during.memory_bytes, memory_cap);

    OK(fastdb_open(options, &db));
    expect_value(db, (FastDBSlice){update, sizeof(update)}, (FastDBSlice){"during", 6},
                 initial_revision + 3);
    expect_missing(db, (FastDBSlice){deleted, sizeof(deleted)});
    expect_missing(db, (FastDBSlice){concurrent, sizeof(concurrent)});
    for (unsigned int i = 3; i < records; ++i) {
        const unsigned char key[] = {'k', (unsigned char)i};
        payload[0] = (unsigned char)i;
        expect_value(db, (FastDBSlice){key, sizeof(key)}, (FastDBSlice){payload, sizeof(payload)},
                     i + i / records_per_flush + 1);
    }
    expect_value(db, (FastDBSlice){"z", 1}, (FastDBSlice){0}, initial_revision);
    OK(fastdb_stats(db, &during));
    CHECK(during.revision == initial_revision + 4 && during.memory_bytes <= memory_cap);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    across_compaction(false);
    across_compaction(true);
    across_background_flush();
    writes_during_compaction();
    snapshot_during_mask_reclamation();
    merge_larger_than_memory();
}
