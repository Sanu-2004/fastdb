#include "disk_fixture.h"

#include <pthread.h>

static void snapshot_value(const FastDBSnapshot *snapshot, FastDBSlice key,
                           FastDBSlice expected, uint64_t revision)
{
    void *value;
    size_t size;
    uint64_t found;
    OK(fastdb_snapshot_get(snapshot, key.data, key.size, &value, &size, &found));
    CHECK(value != NULL && size == expected.size && found == revision);
    CHECK(size == 0 || memcmp(value, expected.data, size) == 0);
    fastdb_free(value);
}

static void checkpoint_snapshot(void)
{
    char path[] = "/tmp/fastdb-snapshot.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options_with_mode(path, FASTDB_MODE_MEMORY_PERSISTENT,
                                                      FASTDB_DURABILITY_SYNC);
    FastDB *db;
    OK(fastdb_open(options, &db));
    unsigned char binary[] = {0, 255, 1};
    OK(fastdb_put(db, binary, sizeof(binary), "old", 3));
    OK(fastdb_put(db, "empty", 5, NULL, 0));
    FastDBSnapshot *snapshot;
    OK(fastdb_snapshot_create(db, &snapshot));
    CHECK(fastdb_snapshot_revision(snapshot) == 2);
    STATUS(fastdb_close(db), FASTDB_BUSY);

    FastDBBatch *batch;
    OK(fastdb_batch_create(options, &batch));
    OK(fastdb_batch_put(batch, binary, sizeof(binary), "new", 3));
    OK(fastdb_batch_delete(batch, "empty", 5));
    OK(fastdb_batch_put(batch, "added", 5, NULL, 0));
    OK(fastdb_batch_commit(db, batch));
    fastdb_batch_destroy(batch);
    OK(fastdb_checkpoint(db));
    snapshot_value(snapshot, (FastDBSlice){binary, sizeof(binary)}, (FastDBSlice){"old", 3}, 1);
    snapshot_value(snapshot, (FastDBSlice){"empty", 5}, (FastDBSlice){0}, 2);
    void *out = db;
    size_t size = 1;
    uint64_t revision = 1;
    STATUS(fastdb_snapshot_get(snapshot, "added", 5, &out, &size, &revision), FASTDB_NOT_FOUND);
    CHECK(out == NULL && size == 0 && revision == 0);
    fastdb_snapshot_release(snapshot);
    OK(fastdb_close(db));
    OK(fastdb_open(options, &db));
    expect_value(db, (FastDBSlice){binary, sizeof(binary)}, (FastDBSlice){"new", 3}, 3);
    expect_missing(db, (FastDBSlice){"empty", 5});
    expect_value(db, (FastDBSlice){"added", 5}, (FastDBSlice){0}, 5);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void snapshot_admission(void)
{
    FastDB *db;
    OK(fastdb_open(NULL, &db));
    unsigned char value[120] = {42};
    OK(fastdb_put(db, "a", 1, value, sizeof(value)));
    FastDBStats live, pinned, released;
    FastDBSnapshot *snapshot, *extra;
    OK(fastdb_stats(db, &live));
    OK(fastdb_snapshot_create(db, &snapshot));
    STATUS(fastdb_snapshot_create(db, &extra), FASTDB_LIMIT_EXCEEDED);
    CHECK(extra == NULL);
    OK(fastdb_stats(db, &pinned));
    CHECK(pinned.memory_bytes > live.memory_bytes && pinned.record_count == live.record_count);
    fastdb_snapshot_release(snapshot);
    OK(fastdb_stats(db, &released));
    CHECK(released.memory_bytes == live.memory_bytes);
    OK(fastdb_close(db));

    FastDBOptions *options;
    FastDBLimits limits;
    OK(fastdb_options_create(&options));
    fastdb_limits_default(&limits);
    for (size_t shortfall = 1;; --shortfall) {
        limits.max_memory_bytes = pinned.memory_bytes - shortfall;
        OK(fastdb_options_set_limits(options, &limits));
        OK(fastdb_open(options, &db));
        OK(fastdb_put(db, "a", 1, value, sizeof(value)));
        if (shortfall != 0) {
            STATUS(fastdb_snapshot_create(db, &snapshot), FASTDB_LIMIT_EXCEEDED);
            CHECK(snapshot == NULL);
        } else {
            OK(fastdb_snapshot_create(db, &snapshot));
            STATUS(fastdb_put(db, "b", 1, NULL, 0), FASTDB_LIMIT_EXCEEDED);
            fastdb_snapshot_release(snapshot);
            OK(fastdb_put(db, "b", 1, NULL, 0));
        }
        OK(fastdb_close(db));
        if (shortfall == 0)
            break;
    }
    fastdb_options_destroy(options);
}

static void put_pair(FastDB *db, uint32_t value)
{
    FastDBBatch *batch;
    OK(fastdb_batch_create(NULL, &batch));
    OK(fastdb_batch_put(batch, "a", 1, &value, sizeof(value)));
    OK(fastdb_batch_put(batch, "b", 1, &value, sizeof(value)));
    OK(fastdb_batch_commit(db, batch));
    fastdb_batch_destroy(batch);
}

typedef struct Writer {
    FastDB *db;
    pthread_barrier_t start;
} Writer;

static void start_pair(Writer *writer)
{
    int result = pthread_barrier_wait(&writer->start);
    CHECK(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *replace_pairs(void *context)
{
    Writer *writer = context;
    start_pair(writer);
    for (uint32_t i = 1; i <= 512; ++i)
        put_pair(writer->db, i);
    return NULL;
}

static void snapshot_atomicity(void)
{
    Writer writer;
    OK(fastdb_open(NULL, &writer.db));
    put_pair(writer.db, 0);
    CHECK(pthread_barrier_init(&writer.start, NULL, 2) == 0);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, replace_pairs, &writer) == 0);
    start_pair(&writer);
    for (size_t i = 0; i < 512; ++i) {
        FastDBSnapshot *snapshot;
        OK(fastdb_snapshot_create(writer.db, &snapshot));
        uint64_t revision = fastdb_snapshot_revision(snapshot);
        CHECK(revision >= 2 && revision <= 1026 && revision % 2 == 0);
        uint32_t expected = (uint32_t)(revision / 2 - 1);
        FastDBSlice value = {&expected, sizeof(expected)};
        snapshot_value(snapshot, (FastDBSlice){"a", 1}, value, revision - 1);
        snapshot_value(snapshot, (FastDBSlice){"b", 1}, value, revision);
        fastdb_snapshot_release(snapshot);
    }
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(pthread_barrier_destroy(&writer.start) == 0);
    OK(fastdb_close(writer.db));
}

static void lsm_snapshot_unsupported(void)
{
    char path[] = "/tmp/fastdb-lsm-snapshot.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDB *db;
    OK(fastdb_open(options, &db));
    FastDBSnapshot *snapshot;
    STATUS(fastdb_snapshot_create(db, &snapshot), FASTDB_UNSUPPORTED);
    CHECK(snapshot == NULL);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    checkpoint_snapshot();
    snapshot_admission();
    snapshot_atomicity();
    lsm_snapshot_unsupported();
}
