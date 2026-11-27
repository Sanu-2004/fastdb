#include "disk_fixture.h"

static const FastDBSlice a = {"a", 1}, b = {"b", 1}, c = {"c", 1};
static const FastDBSlice value = {"v", 1};

static FastDBOptions *cache_options(FastDBEvictionPolicy policy, size_t capacity)
{
    FastDBOptions *options;
    OK(fastdb_options_create(&options));
    OK(fastdb_options_set_eviction(options, policy, capacity));
    return options;
}

static size_t entry_charge(void)
{
    FastDBOptions *options = cache_options(FASTDB_EVICTION_FIFO, 4096);
    FastDBStats empty, one;
    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_stats(db, &empty));
    OK(fastdb_put(db, "a", 1, "v", 1));
    OK(fastdb_stats(db, &one));
    CHECK(one.memory_bytes > empty.memory_bytes);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    return one.memory_bytes - empty.memory_bytes;
}

static void policy_order(FastDBEvictionPolicy policy, size_t capacity)
{
    FastDBOptions *options = cache_options(policy, capacity);
    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "a", 1, "v", 1));
    OK(fastdb_put(db, "b", 1, "v", 1));
    expect_value(db, a, value, 1);
    expect_value(db, a, value, 1);
    expect_value(db, b, value, 2);
    OK(fastdb_put(db, "c", 1, "v", 1));
    if (policy == FASTDB_EVICTION_LFU) {
        expect_value(db, a, value, 1);
        expect_missing(db, b);
    } else {
        expect_missing(db, a);
        expect_value(db, b, value, 2);
    }
    expect_value(db, c, value, 3);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
}

static void recency_and_fifo_replacement(size_t capacity)
{
    const FastDBEvictionPolicy policies[] = {FASTDB_EVICTION_LRU, FASTDB_EVICTION_FIFO};
    for (size_t i = 0; i < sizeof(policies) / sizeof(policies[0]); ++i) {
        FastDBOptions *options = cache_options(policies[i], capacity);
        FastDB *db;
        OK(fastdb_open(options, &db));
        OK(fastdb_put(db, "a", 1, "v", 1));
        OK(fastdb_put(db, "b", 1, "v", 1));
        OK(fastdb_put(db, "a", 1, "v", 1));
        bool exists;
        OK(fastdb_exists(db, "a", 1, &exists));
        CHECK(exists);
        OK(fastdb_put(db, "c", 1, "v", 1));
        if (policies[i] == FASTDB_EVICTION_LRU) {
            expect_missing(db, b);
            expect_value(db, a, value, 3);
        } else {
            expect_missing(db, a);
            expect_value(db, b, value, 2);
        }
        OK(fastdb_close(db));
        fastdb_options_destroy(options);
    }
}

static void batch_and_snapshot(size_t capacity)
{
    FastDBOptions *options = cache_options(FASTDB_EVICTION_FIFO, capacity);
    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "a", 1, "v", 1));
    OK(fastdb_put(db, "b", 1, "v", 1));
    FastDBSnapshot *snapshot;
    OK(fastdb_snapshot_create(db, &snapshot));
    FastDBStats before, after;
    OK(fastdb_stats(db, &before));
    FastDBBatch *batch;
    OK(fastdb_batch_create(options, &batch));
    OK(fastdb_batch_put(batch, "c", 1, "v", 1));
    OK(fastdb_batch_put(batch, "d", 1, "v", 1));
    OK(fastdb_batch_put(batch, "e", 1, "v", 1));
    STATUS(fastdb_batch_commit(db, batch), FASTDB_LIMIT_EXCEEDED);
    fastdb_batch_destroy(batch);
    OK(fastdb_stats(db, &after));
    CHECK(after.memory_bytes == before.memory_bytes && after.revision == 2 &&
          after.record_count == 2);
    expect_value(db, a, value, 1);
    expect_value(db, b, value, 2);
    expect_missing(db, c);

    OK(fastdb_batch_create(options, &batch));
    OK(fastdb_batch_put(batch, "c", 1, "v", 1));
    OK(fastdb_batch_put(batch, "d", 1, "v", 1));
    OK(fastdb_batch_commit(db, batch));
    fastdb_batch_destroy(batch);
    expect_missing(db, a);
    expect_missing(db, b);
    expect_value(db, c, value, 3);
    expect_value(db, (FastDBSlice){"d", 1}, value, 4);
    void *old;
    size_t size;
    uint64_t revision;
    OK(fastdb_snapshot_get(snapshot, "a", 1, &old, &size, &revision));
    CHECK(size == 1 && memcmp(old, "v", 1) == 0 && revision == 1);
    fastdb_free(old);
    fastdb_snapshot_release(snapshot);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
}

static void transient_headroom(size_t capacity)
{
    FastDBOptions *options = cache_options(FASTDB_EVICTION_FIFO, capacity);
    FastDB *db;
    FastDBSnapshot *snapshot;
    FastDBStats held;
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "a", 1, "v", 1));
    OK(fastdb_put(db, "b", 1, "v", 1));
    OK(fastdb_snapshot_create(db, &snapshot));
    OK(fastdb_stats(db, &held));
    fastdb_snapshot_release(snapshot);
    OK(fastdb_close(db));
    FastDBLimits limits;
    fastdb_limits_default(&limits);
    limits.max_memory_bytes = held.memory_bytes;
    OK(fastdb_options_set_limits(options, &limits));
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "a", 1, "v", 1));
    OK(fastdb_put(db, "b", 1, "v", 1));
    OK(fastdb_snapshot_create(db, &snapshot));
    STATUS(fastdb_put(db, "c", 1, "v", 1), FASTDB_LIMIT_EXCEEDED);
    expect_value(db, a, value, 1);
    expect_missing(db, c);
    fastdb_snapshot_release(snapshot);
    OK(fastdb_put(db, "c", 1, "v", 1));
    expect_missing(db, a);
    expect_value(db, c, value, 3);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
}

static void validate_configuration(size_t capacity)
{
    char path[] = "/tmp/fastdb-eviction.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = cache_options(FASTDB_EVICTION_FIFO, capacity);
    STATUS(fastdb_options_set_eviction(options, FASTDB_EVICTION_LRU, 0), FASTDB_INVALID_ARGUMENT);
    STATUS(fastdb_options_set_eviction(options, FASTDB_EVICTION_NONE, 1), FASTDB_INVALID_ARGUMENT);
    STATUS(fastdb_options_set_storage(options, FASTDB_MODE_MEMORY_PERSISTENT, path, NULL),
           FASTDB_INVALID_ARGUMENT);
    STATUS(fastdb_options_set_storage(options, FASTDB_MODE_LSM, path, NULL),
           FASTDB_INVALID_ARGUMENT);
    OK(fastdb_options_set_eviction(options, FASTDB_EVICTION_NONE, 0));
    OK(fastdb_options_set_storage(options, FASTDB_MODE_MEMORY_PERSISTENT, path, NULL));
    STATUS(fastdb_options_set_eviction(options, FASTDB_EVICTION_FIFO, capacity),
           FASTDB_INVALID_ARGUMENT);
    OK(fastdb_options_set_storage(options, FASTDB_MODE_LSM, path, NULL));
    STATUS(fastdb_options_set_eviction(options, FASTDB_EVICTION_LFU, capacity),
           FASTDB_INVALID_ARGUMENT);
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    size_t capacity = 2 * entry_charge();
    validate_configuration(capacity);
    policy_order(FASTDB_EVICTION_FIFO, capacity);
    policy_order(FASTDB_EVICTION_LRU, capacity);
    policy_order(FASTDB_EVICTION_LFU, capacity);
    recency_and_fifo_replacement(capacity);
    batch_and_snapshot(capacity);
    transient_headroom(capacity);
}
