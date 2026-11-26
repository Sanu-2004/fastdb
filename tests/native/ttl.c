#include "disk_fixture.h"
#include "engine.h"

#include <errno.h>

typedef struct Clock {
    uint64_t now;
    FastDBStatus status;
} Clock;

static Clock test_clock;

FastDBStatus __wrap_fdb_read_time(uint64_t *milliseconds);

FastDBStatus __wrap_fdb_read_time(uint64_t *milliseconds)
{
    *milliseconds = test_clock.now;
    return test_clock.status;
}

static void expect_snapshot(FastDBSnapshot *snapshot, const char *expected, uint64_t revision)
{
    void *value;
    size_t size;
    uint64_t actual;
    OK(fastdb_snapshot_get(snapshot, "a", 1, &value, &size, &actual));
    CHECK(actual == revision && size == strlen(expected) && memcmp(value, expected, size) == 0);
    fastdb_free(value);
}

static void boundary_purge_and_clock_errors(void)
{
    FastDB *db;
    OK(fastdb_open(NULL, &db));
    test_clock = (Clock){.now = 1000};
    OK(fastdb_put_with_ttl(db, "a", 1, "timed", 5, 10));
    OK(fastdb_put_with_ttl(db, "b", 1, NULL, 0, 0));
    test_clock.now = 1009;
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"timed", 5}, 1);
    FastDBSnapshot *snapshot;
    OK(fastdb_snapshot_create(db, &snapshot));

    test_clock.status = FASTDB_IO_ERROR;
    void *value = db;
    size_t size = 1;
    STATUS(fastdb_get(db, "a", 1, &value, &size), FASTDB_IO_ERROR);
    CHECK(value == NULL && size == 0);
    STATUS(fastdb_put_with_ttl(db, "c", 1, NULL, 0, 1), FASTDB_IO_ERROR);
    size_t removed = 1;
    STATUS(fastdb_purge_expired(db, 1, &removed), FASTDB_IO_ERROR);
    CHECK(removed == 0);
    FastDBIterator *page;
    STATUS(fastdb_scan_prefix(db, (FastDBSlice){0}, 2, &page), FASTDB_IO_ERROR);
    CHECK(page == NULL);
    expect_snapshot(snapshot, "timed", 1);

    test_clock = (Clock){.now = UINT64_MAX - 9};
    STATUS(fastdb_put_with_ttl(db, "c", 1, NULL, 0, 10), FASTDB_LIMIT_EXCEEDED);
    FastDBBatch *batch;
    OK(fastdb_batch_create(NULL, &batch));
    OK(fastdb_batch_put(batch, "c", 1, NULL, 0));
    OK(fastdb_batch_put_with_ttl(batch, "d", 1, NULL, 0, 10));
    STATUS(fastdb_batch_commit(db, batch), FASTDB_LIMIT_EXCEEDED);
    fastdb_batch_destroy(batch);
    FastDBStats before, after;
    OK(fastdb_stats(db, &before));
    CHECK(before.revision == 2);
    test_clock.now = 1010;
    expect_missing(db, (FastDBSlice){"a", 1});
    expect_missing(db, (FastDBSlice){"c", 1});
    expect_missing(db, (FastDBSlice){"d", 1});
    expect_value(db, (FastDBSlice){"b", 1}, (FastDBSlice){0}, 2);
    STATUS(fastdb_compare_and_swap(db, "a", 1, 1, "wrong", 5), FASTDB_CONFLICT);
    OK(fastdb_purge_expired(db, 1, &removed));
    CHECK(removed == 1);
    OK(fastdb_stats(db, &after));
    CHECK(after.revision == 3 && after.record_count == 1 && after.memory_bytes < before.memory_bytes);
    test_clock.now = 1000;
    expect_missing(db, (FastDBSlice){"a", 1});
    expect_snapshot(snapshot, "timed", 1);
    fastdb_snapshot_release(snapshot);
    OK(fastdb_compare_and_swap(db, "a", 1, FASTDB_EXPECT_ABSENT, "back", 4));
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"back", 4}, 4);
    OK(fastdb_close(db));
}

static void durable_ttl(FastDBMode mode)
{
    char path[] = "/tmp/fastdb-ttl.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options_with_mode(path, mode, FASTDB_DURABILITY_SYNC);
    FastDB *db;
    OK(fastdb_open(options, &db));
    test_clock = (Clock){.now = 1000};
    OK(fastdb_put(db, "a", 1, "older", 5));
    if (mode == FASTDB_MODE_LSM)
        OK(fastdb_flush(db));
    else
        OK(fastdb_checkpoint(db));

    FastDBBatch *batch;
    OK(fastdb_batch_create(options, &batch));
    OK(fastdb_batch_expect(batch, "a", 1, 1));
    OK(fastdb_batch_put_with_ttl(batch, "a", 1, "timed", 5, 10));
    OK(fastdb_batch_put_with_ttl(batch, "b", 1, NULL, 0, 10));
    test_clock.now = 2000;
    OK(fastdb_batch_commit(db, batch));
    fastdb_batch_destroy(batch);
    OK(fastdb_close(db));
    OK(fastdb_open(options, &db));
    test_clock.now = 2009;
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"timed", 5}, 2);
    expect_value(db, (FastDBSlice){"b", 1}, (FastDBSlice){0}, 3);
    if (mode == FASTDB_MODE_LSM)
        OK(fastdb_flush(db));
    else
        OK(fastdb_checkpoint(db));
    FastDBSnapshot *snapshot;
    OK(fastdb_snapshot_create(db, &snapshot));

    test_clock.now = 2010;
    expect_missing(db, (FastDBSlice){"a", 1});
    expect_missing(db, (FastDBSlice){"b", 1});
    FastDBIterator *page;
    FastDBSlice key, value;
    uint64_t revision;
    OK(fastdb_scan_prefix(db, (FastDBSlice){0}, 4, &page));
    STATUS(fastdb_iterator_next(page, &key, &value, &revision), FASTDB_NOT_FOUND);
    CHECK(!fastdb_iterator_has_more(page));
    fastdb_iterator_close(page);
    size_t removed;
    if (mode == FASTDB_MODE_LSM) {
        STATUS(fastdb_purge_expired(db, 1, &removed), FASTDB_UNSUPPORTED);
        CHECK(removed == 0);
        OK(fastdb_compact(db));
        expect_missing(db, (FastDBSlice){"a", 1});
    } else {
        OK(fastdb_purge_expired(db, 1, &removed));
        CHECK(removed == 1);
    }
    expect_snapshot(snapshot, "timed", 2);
    OK(fastdb_snapshot_scan_prefix(snapshot, (FastDBSlice){0}, 4, &page));
    OK(fastdb_iterator_next(page, &key, &value, &revision));
    CHECK(key.size == 1 && memcmp(key.data, "a", 1) == 0 && revision == 2);
    OK(fastdb_iterator_next(page, &key, &value, &revision));
    CHECK(key.size == 1 && memcmp(key.data, "b", 1) == 0 && value.size == 0 && revision == 3);
    STATUS(fastdb_iterator_next(page, &key, &value, &revision), FASTDB_NOT_FOUND);
    fastdb_iterator_close(page);
    fastdb_snapshot_release(snapshot);
    OK(fastdb_close(db));
    OK(fastdb_open(options, &db));
    if (mode == FASTDB_MODE_MEMORY_PERSISTENT)
        test_clock.now = 2009;
    expect_missing(db, (FastDBSlice){"a", 1});
    if (mode == FASTDB_MODE_MEMORY_PERSISTENT)
        expect_value(db, (FastDBSlice){"b", 1}, (FastDBSlice){0}, 3);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static FDBFsFault fail_write(void *context, FDBFsOp operation, size_t size)
{
    (void)size;
    unsigned int *remaining = context;
    if (operation == FDB_FS_WRITE && *remaining != 0 && --*remaining == 0)
        return (FDBFsFault){.error = EIO};
    return (FDBFsFault){0};
}

static void purge_partial_error(void)
{
    char path[] = "/tmp/fastdb-ttl-purge.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options_with_mode(path, FASTDB_MODE_MEMORY_PERSISTENT,
                                                      FASTDB_DURABILITY_SYNC);
    unsigned int remaining = 0;
    const FDBFs fs = {fail_write, &remaining};
    FastDB *db;
    test_clock = (Clock){.now = 100};
    OK(fdb_open_with_fs(options, &fs, &db));
    OK(fastdb_put_with_ttl(db, "a", 1, "A", 1, 10));
    OK(fastdb_put_with_ttl(db, "b", 1, "B", 1, 10));
    test_clock.now = 110;
    remaining = 2;
    size_t removed;
    STATUS(fastdb_purge_expired(db, 2, &removed), FASTDB_COMMIT_UNCERTAIN);
    CHECK(removed == 1 && remaining == 0);
    STATUS(fastdb_close(db), FASTDB_COMMIT_UNCERTAIN);
    test_clock.now = 109;
    OK(fastdb_open(options, &db));
    FastDBStats stats;
    OK(fastdb_stats(db, &stats));
    CHECK(stats.revision == 3 && stats.record_count == 1);
    expect_missing(db, (FastDBSlice){"a", 1});
    expect_value(db, (FastDBSlice){"b", 1}, (FastDBSlice){"B", 1}, 2);
    test_clock.now = 110;
    OK(fastdb_purge_expired(db, 2, &removed));
    CHECK(removed == 1);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    boundary_purge_and_clock_errors();
    durable_ttl(FASTDB_MODE_MEMORY_PERSISTENT);
    durable_ttl(FASTDB_MODE_LSM);
    purge_partial_error();
}
