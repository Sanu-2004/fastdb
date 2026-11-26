#include "disk_fixture.h"

#include <pthread.h>

static void page_row(FastDBIterator *page, FastDBSlice key, FastDBSlice value, uint64_t revision)
{
    FastDBSlice actual_key, actual_value;
    uint64_t sequence;
    OK(fastdb_iterator_next(page, &actual_key, &actual_value, &sequence));
    CHECK(actual_key.size == key.size && memcmp(actual_key.data, key.data, key.size) == 0);
    CHECK(actual_value.size == value.size && sequence == revision);
    CHECK(value.size == 0 || memcmp(actual_value.data, value.data, value.size) == 0);
}

static void page_end(FastDBIterator *page, bool more)
{
    FastDBSlice key, value;
    uint64_t revision;
    STATUS(fastdb_iterator_next(page, &key, &value, &revision), FASTDB_NOT_FOUND);
    CHECK(fastdb_iterator_has_more(page) == more);
    fastdb_iterator_close(page);
}

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
    FastDBIterator *page;
    OK(fastdb_snapshot_scan_prefix(snapshot, (FastDBSlice){binary, 2}, 1, &page));
    page_row(page, (FastDBSlice){binary, sizeof(binary)}, (FastDBSlice){"old", 3}, 1);
    page_end(page, false);
    const FastDBRange rest = {.start = {binary, sizeof(binary)}, .start_exclusive = true};
    OK(fastdb_snapshot_scan_range(snapshot, &rest, 1, &page));
    fastdb_snapshot_release(snapshot);
    STATUS(fastdb_close(db), FASTDB_BUSY);
    page_row(page, (FastDBSlice){"empty", 5}, (FastDBSlice){0}, 2);
    page_end(page, false);
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
            FastDBIterator *page;
            STATUS(fastdb_snapshot_scan_prefix(snapshot, (FastDBSlice){0}, 1, &page),
                   FASTDB_LIMIT_EXCEEDED);
            CHECK(page == NULL);
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

static size_t table_count(const char *path)
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

static void lsm_snapshot_and_merged_pages(void)
{
    char path[] = "/tmp/fastdb-lsm-snapshot.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDBCompactionOptions policy;
    fastdb_compaction_options_default(&policy);
    policy.max_levels = 2;
    OK(fastdb_options_set_compaction(options, &policy));
    FastDB *db;
    OK(fastdb_open(options, &db));
    const FastDBSlice a = {"a", 1}, binary = {"a\0\xff", 3}, b = {"b", 1}, c = {"c", 1};
    const FastDBSlice old = {"old", 3}, new = {"new", 3};
    OK(fastdb_put(db, a.data, a.size, old.data, old.size));
    OK(fastdb_put(db, binary.data, binary.size, NULL, 0));
    OK(fastdb_flush(db));
    OK(fastdb_compact(db));
    OK(fastdb_put(db, b.data, b.size, old.data, old.size));
    OK(fastdb_put(db, a.data, a.size, new.data, new.size));
    OK(fastdb_flush(db));
    OK(fastdb_delete(db, b.data, b.size));
    OK(fastdb_put(db, c.data, c.size, NULL, 0));
    FastDBSnapshot *snapshot;
    OK(fastdb_snapshot_create(db, &snapshot));
    CHECK(fastdb_snapshot_revision(snapshot) == 6);

    FastDBBatch *batch;
    OK(fastdb_batch_create(options, &batch));
    OK(fastdb_batch_delete(batch, a.data, a.size));
    OK(fastdb_batch_put(batch, b.data, b.size, new.data, new.size));
    OK(fastdb_batch_commit(db, batch));
    fastdb_batch_destroy(batch);
    OK(fastdb_flush(db));
    OK(fastdb_compact(db));
    CHECK(table_count(path) > 1);
    snapshot_value(snapshot, a, new, 4);
    snapshot_value(snapshot, binary, (FastDBSlice){0}, 2);
    void *value;
    size_t size;
    uint64_t revision;
    STATUS(fastdb_snapshot_get(snapshot, b.data, b.size, &value, &size, &revision), FASTDB_NOT_FOUND);
    CHECK(value == NULL && size == 0 && revision == 0);

    FastDBIterator *page;
    OK(fastdb_snapshot_scan_prefix(snapshot, a, 1, &page));
    page_row(page, a, new, 4);
    page_end(page, true);
    const FastDBRange rest = {.start = a, .end = {"d", 1}, .start_exclusive = true};
    OK(fastdb_snapshot_scan_range(snapshot, &rest, 2, &page));
    page_row(page, binary, (FastDBSlice){0}, 2);
    page_row(page, c, (FastDBSlice){0}, 6);
    page_end(page, false);
    OK(fastdb_scan_range(db, &rest, 4, &page));
    page_row(page, binary, (FastDBSlice){0}, 2);
    page_row(page, b, new, 8);
    page_row(page, c, (FastDBSlice){0}, 6);
    page_end(page, false);
    STATUS(fastdb_close(db), FASTDB_BUSY);
    fastdb_snapshot_release(snapshot);
    OK(fastdb_flush(db));
    CHECK(table_count(path) == 1);
    OK(fastdb_close(db));
    OK(fastdb_open(options, &db));
    expect_missing(db, a);
    expect_value(db, b, new, 8);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    checkpoint_snapshot();
    snapshot_admission();
    snapshot_atomicity();
    lsm_snapshot_and_merged_pages();
}
