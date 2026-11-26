#include "check.h"

static size_t fail_after = SIZE_MAX;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);

static bool fail(void)
{
    if (fail_after == SIZE_MAX)
        return false;
    if (fail_after == 0) {
        fail_after = SIZE_MAX;
        return true;
    }
    --fail_after;
    return false;
}

void *__wrap_malloc(size_t size)
{
    return fail() ? NULL : __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    return fail() ? NULL : __real_calloc(count, size);
}

int main(void)
{
    FastDB *db;
    fail_after = 0;
    STATUS(fastdb_open(NULL, &db), FASTDB_OUT_OF_MEMORY);
    CHECK(db == NULL);
    OK(fastdb_open(NULL, &db));
    OK(fastdb_put(db, "a", 1, "x", 1));
    FastDBBatch *batch;
    OK(fastdb_batch_create(NULL, &batch));
    fail_after = 0;
    STATUS(fastdb_batch_put(batch, "z", 1, NULL, 0), FASTDB_OUT_OF_MEMORY);
    OK(fastdb_batch_put(batch, "a", 1, "y", 1));
    OK(fastdb_batch_put(batch, "b", 1, "y", 1));
    FastDBStats before, after;
    OK(fastdb_stats(db, &before));
    for (size_t index = 0; index < 3; ++index) {
        fail_after = index;
        STATUS(fastdb_batch_commit(db, batch), FASTDB_OUT_OF_MEMORY);
        CHECK(fail_after == SIZE_MAX);
        OK(fastdb_stats(db, &after));
        CHECK(before.revision == after.revision && before.record_count == after.record_count);
        CHECK(before.memory_bytes == after.memory_bytes);
        void *value;
        size_t size;
        OK(fastdb_get(db, "a", 1, &value, &size));
        CHECK(size == 1 && *(const char *)value == 'x');
        fastdb_free(value);
    }

    void *out = db;
    size_t size = 1;
    fail_after = 0;
    STATUS(fastdb_get(db, "a", 1, &out, &size), FASTDB_OUT_OF_MEMORY);
    CHECK(out == NULL && size == 0);
    FastDBIterator *page;
    fail_after = 0;
    STATUS(fastdb_scan_prefix(db, (FastDBSlice){0}, 2, &page), FASTDB_OUT_OF_MEMORY);
    CHECK(page == NULL);
    OK(fastdb_stats(db, &after));
    CHECK(after.open_iterators == 0 && after.iterator_bytes == 0);

    OK(fastdb_batch_commit(db, batch));
    fastdb_batch_destroy(batch);
    OK(fastdb_stats(db, &after));
    CHECK(after.revision == 3 && after.record_count == 2);
    before = after;
    FastDBSnapshot *snapshot;
    for (size_t index = 0; index < 3; ++index) {
        fail_after = index;
        STATUS(fastdb_snapshot_create(db, &snapshot), FASTDB_OUT_OF_MEMORY);
        CHECK(snapshot == NULL && fail_after == SIZE_MAX);
        OK(fastdb_stats(db, &after));
        CHECK(after.revision == before.revision && after.record_count == before.record_count);
        CHECK(after.memory_bytes == before.memory_bytes);
    }
    OK(fastdb_snapshot_create(db, &snapshot));
    out = db;
    size = 1;
    uint64_t revision = 1;
    fail_after = 0;
    STATUS(fastdb_snapshot_get(snapshot, "a", 1, &out, &size, &revision), FASTDB_OUT_OF_MEMORY);
    CHECK(out == NULL && size == 0 && revision == 0);
    fastdb_snapshot_release(snapshot);
    OK(fastdb_close(db));
}
