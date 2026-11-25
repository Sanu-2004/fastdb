#include "check.h"

#include <string.h>

static uint64_t verify_value(FastDB *db, FastDBSlice key, FastDBSlice expected)
{
    void *value;
    size_t size;
    uint64_t revision;
    OK(fastdb_get_with_revision(db, key.data, key.size, &value, &size, &revision));
    CHECK(value != NULL && size == expected.size && revision != 0);
    CHECK(size == 0 || memcmp(value, expected.data, size) == 0);
    fastdb_free(value);
    return revision;
}

static void verify_absent(FastDB *db, FastDBSlice key)
{
    void *value = db;
    size_t size = 1;
    uint64_t revision = 1;
    STATUS(fastdb_get_with_revision(db, key.data, key.size, &value, &size, &revision),
           FASTDB_NOT_FOUND);
    CHECK(value == NULL && size == 0 && revision == 0);
    bool exists = true;
    OK(fastdb_exists(db, key.data, key.size, &exists));
    CHECK(!exists);
}

static void crud(void)
{
    CHECK(FASTDB_ABI_VERSION == 1 && fastdb_version()[0] != '\0');
    FastDB *db, *second;
    OK(fastdb_open(NULL, &db));
    OK(fastdb_open(NULL, &second));
    const unsigned char key[] = {0, 0xff};
    const unsigned char value[] = {0x80, 0, 0x40};
    const FastDBSlice k = {key, sizeof(key)};
    const FastDBSlice v = {value, sizeof(value)};
    STATUS(fastdb_put(db, NULL, 0, value, sizeof(value)), FASTDB_INVALID_ARGUMENT);
    STATUS(fastdb_put(db, key, sizeof(key), NULL, 1), FASTDB_INVALID_ARGUMENT);
    FastDBStats initial, stored, updated;
    OK(fastdb_stats(db, &initial));
    OK(fastdb_put(db, key, sizeof(key), value, sizeof(value)));
    OK(fastdb_stats(db, &stored));
    CHECK(stored.record_count == 1 && stored.memory_bytes > initial.memory_bytes);
    verify_absent(second, k);
    void *copy;
    size_t size;
    OK(fastdb_get(db, key, sizeof(key), &copy, &size));
    CHECK(size == sizeof(value));
    memset(copy, 0, size);
    uint64_t first = verify_value(db, k, v);
    CHECK(first == 1);

    for (size_t i = 0; i < 1000; ++i)
        OK(fastdb_put(db, key, sizeof(key), value, sizeof(value)));
    OK(fastdb_stats(db, &updated));
    CHECK(updated.memory_bytes == stored.memory_bytes && updated.revision == 1001);
    STATUS(fastdb_compare_and_swap(db, key, sizeof(key), first, NULL, 0), FASTDB_CONFLICT);
    OK(fastdb_delete(db, key, sizeof(key)));
    OK(fastdb_delete(db, key, sizeof(key)));
    CHECK(memcmp(copy, (unsigned char[3]){0}, size) == 0);
    fastdb_free(copy);
    verify_absent(db, k);
    OK(fastdb_stats(db, &updated));
    CHECK(updated.memory_bytes == initial.memory_bytes && updated.record_count == 0);
    OK(fastdb_compare_and_swap(db, key, sizeof(key), FASTDB_EXPECT_ABSENT, NULL, 0));
    CHECK(verify_value(db, k, (FastDBSlice){0}) == 1004);
    OK(fastdb_close(second));
    OK(fastdb_close(db));
}

static void batches(void)
{
    FastDB *db;
    FastDBBatch *batch;
    const FastDBSlice a = {"a", 1}, b = {"b", 1};
    OK(fastdb_open(NULL, &db));
    OK(fastdb_put(db, "a", 1, "old", 3));
    OK(fastdb_batch_create(NULL, &batch));
    OK(fastdb_batch_put(batch, "b", 1, "temp", 4));
    OK(fastdb_batch_delete(batch, "a", 1));
    OK(fastdb_batch_expect(batch, "a", 1, 42));
    STATUS(fastdb_batch_commit(db, batch), FASTDB_CONFLICT);
    verify_absent(db, b);
    CHECK(verify_value(db, a, (FastDBSlice){"old", 3}) == 1);
    fastdb_batch_destroy(batch);

    OK(fastdb_batch_create(NULL, &batch));
    unsigned char buffer[] = {'a'};
    OK(fastdb_batch_put(batch, "b", 1, "one", 3));
    OK(fastdb_batch_delete(batch, buffer, sizeof(buffer)));
    OK(fastdb_batch_expect(batch, "b", 1, FASTDB_EXPECT_ABSENT));
    OK(fastdb_batch_expect(batch, buffer, sizeof(buffer), 1));
    buffer[0] = 'c';
    OK(fastdb_batch_delete(batch, "b", 1));
    memcpy(buffer, "d", 1);
    OK(fastdb_batch_put(batch, "b", 1, buffer, sizeof(buffer)));
    buffer[0] = 'e';
    OK(fastdb_batch_commit(db, batch));
    STATUS(fastdb_batch_commit(db, batch), FASTDB_CONFLICT);
    fastdb_batch_destroy(batch);
    verify_absent(db, a);
    CHECK(verify_value(db, b, (FastDBSlice){"d", 1}) == 5);
    OK(fastdb_close(db));
}

static FastDB *limited_db(FastDBOptions **options)
{
    FastDB *db;
    FastDBLimits limits, invalid;
    fastdb_limits_default(&limits);
    limits.max_key_bytes = 8;
    limits.max_value_bytes = 256;
    limits.max_memory_bytes = 512;
    limits.max_batch_operations = 2;
    limits.max_batch_bytes = 240;
    limits.max_scan_records = 2;
    limits.max_scan_bytes = 256;
    limits.max_open_iterators = 1;
    OK(fastdb_options_create(options));
    OK(fastdb_options_set_limits(*options, &limits));
    invalid = limits;
    invalid.max_memory_bytes = 1;
    STATUS(fastdb_options_set_limits(*options, &invalid), FASTDB_INVALID_ARGUMENT);
    OK(fastdb_open(*options, &db));
    return db;
}

static void budgets(void)
{
    FastDBOptions *options;
    FastDB *db = limited_db(&options);
    unsigned char bytes[257] = {1};
    STATUS(fastdb_put(db, "x", 1, bytes, sizeof(bytes)), FASTDB_LIMIT_EXCEEDED);
    STATUS(fastdb_delete(db, "longerkey", 9), FASTDB_LIMIT_EXCEEDED);
    OK(fastdb_put(db, "x", 1, bytes, 230));
    STATUS(fastdb_put(db, "y", 1, bytes, 230), FASTDB_LIMIT_EXCEEDED);
    FastDBIterator *iterator = NULL;
    STATUS(fastdb_scan_prefix(db, (FastDBSlice){0}, 1, &iterator), FASTDB_LIMIT_EXCEEDED);
    CHECK(iterator == NULL);
    OK(fastdb_delete(db, "x", 1));
    STATUS(fastdb_scan_prefix(db, (FastDBSlice){0}, 3, &iterator), FASTDB_LIMIT_EXCEEDED);
    OK(fastdb_scan_prefix(db, (FastDBSlice){0}, 2, &iterator));
    STATUS(fastdb_close(db), FASTDB_BUSY);
    FastDBIterator *blocked;
    STATUS(fastdb_scan_prefix(db, (FastDBSlice){0}, 1, &blocked), FASTDB_LIMIT_EXCEEDED);
    fastdb_iterator_close(iterator);

    FastDBBatch *batch;
    OK(fastdb_batch_create(options, &batch));
    STATUS(fastdb_batch_put(batch, "x", 1, bytes, 124), FASTDB_LIMIT_EXCEEDED);
    OK(fastdb_batch_put(batch, "x", 1, NULL, 0));
    OK(fastdb_batch_delete(batch, "y", 1));
    STATUS(fastdb_batch_put(batch, "z", 1, NULL, 0), FASTDB_LIMIT_EXCEEDED);
    fastdb_options_destroy(options);
    OK(fastdb_batch_commit(db, batch));
    fastdb_batch_destroy(batch);

    OK(fastdb_batch_create(NULL, &batch));
    OK(fastdb_batch_put(batch, "x", 1, NULL, 0));
    OK(fastdb_batch_put(batch, "y", 1, NULL, 0));
    OK(fastdb_batch_put(batch, "z", 1, NULL, 0));
    STATUS(fastdb_batch_commit(db, batch), FASTDB_LIMIT_EXCEEDED);
    fastdb_batch_destroy(batch);
    verify_absent(db, (FastDBSlice){"y", 1});
    OK(fastdb_close(db));
}

static void scans(void)
{
    FastDB *db;
    OK(fastdb_open(NULL, &db));
    OK(fastdb_put(db, "a", 1, "A", 1));
    OK(fastdb_put(db, "a\0", 2, "B", 1));
    OK(fastdb_put(db, "a\xff", 2, "C", 1));
    OK(fastdb_put(db, "b", 1, "D", 1));
    FastDBIterator *page;
    OK(fastdb_scan_prefix(db, (FastDBSlice){"a", 1}, 2, &page));
    CHECK(fastdb_iterator_has_more(page));
    OK(fastdb_put(db, "a", 1, "changed", 7));
    OK(fastdb_delete(db, "a\0", 2));
    FastDBSlice key, value;
    uint64_t revision;
    OK(fastdb_iterator_next(page, &key, &value, &revision));
    CHECK(key.size == 1 && revision == 1 && memcmp(value.data, "A", 1) == 0);
    const void *retained = value.data;
    OK(fastdb_iterator_next(page, &key, &value, &revision));
    CHECK(key.size == 2 && memcmp(key.data, "a\0", 2) == 0 && revision == 2);
    CHECK(memcmp(retained, "A", 1) == 0 && memcmp(value.data, "B", 1) == 0);
    STATUS(fastdb_iterator_next(page, &key, &value, &revision), FASTDB_NOT_FOUND);
    fastdb_iterator_close(page);

    FastDBRange range = {.start = {"a", 1}, .end = {"b", 1}, .start_exclusive = true};
    OK(fastdb_scan_range(db, &range, 10, &page));
    OK(fastdb_iterator_next(page, &key, &value, &revision));
    CHECK(key.size == 2 && memcmp(key.data, "a\xff", 2) == 0 && revision == 3);
    STATUS(fastdb_iterator_next(page, &key, &value, &revision), FASTDB_NOT_FOUND);
    CHECK(!fastdb_iterator_has_more(page));
    fastdb_iterator_close(page);
    range.start = (FastDBSlice){"z", 1};
    STATUS(fastdb_scan_range(db, &range, 1, &page), FASTDB_INVALID_ARGUMENT);
    OK(fastdb_close(db));
}

int main(void)
{
    crud();
    batches();
    budgets();
    scans();
}
