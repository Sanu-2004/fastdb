#include "check.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

enum { THREADS = 4, INCREMENTS = 1500, BATCHES = 2000 };

static uint64_t fetch(FastDB *db, uint64_t *revision)
{
    void *value;
    size_t size;
    uint64_t n;
    OK(fastdb_get_with_revision(db, "counter", 7, &value, &size, revision));
    CHECK(size == sizeof(n));
    memcpy(&n, value, size);
    fastdb_free(value);
    return n;
}

static void *increment(void *context)
{
    FastDB *db = context;
    for (size_t i = 0; i < INCREMENTS; ++i) {
        FastDBStatus status;
        do {
            uint64_t revision;
            uint64_t value = fetch(db, &revision) + 1;
            status = fastdb_compare_and_swap(db, "counter", 7, revision, &value, sizeof(value));
            CHECK(status == FASTDB_OK || status == FASTDB_CONFLICT);
        } while (status == FASTDB_CONFLICT);
    }
    return NULL;
}

typedef struct ScanTest {
    FastDB *db;
    pthread_barrier_t barrier;
    atomic_bool finished;
} ScanTest;

static void barrier_wait(pthread_barrier_t *barrier)
{
    int status = pthread_barrier_wait(barrier);
    CHECK(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *scan(void *context)
{
    ScanTest *test = context;
    barrier_wait(&test->barrier);
    do {
        FastDBIterator *page;
        OK(fastdb_scan_prefix(test->db, (FastDBSlice){"p", 1}, 2, &page));
        FastDBSlice key, left, right;
        uint64_t revision;
        OK(fastdb_iterator_next(page, &key, &left, &revision));
        CHECK(key.size == 2 && memcmp(key.data, "pa", 2) == 0);
        OK(fastdb_iterator_next(page, &key, &right, &revision));
        CHECK(key.size == 2 && memcmp(key.data, "pb", 2) == 0);
        CHECK(left.size == sizeof(uint64_t) && right.size == left.size);
        CHECK(memcmp(left.data, right.data, left.size) == 0);
        fastdb_iterator_close(page);
    } while (!atomic_load(&test->finished));
    return NULL;
}

static void write_pair(FastDB *db, uint64_t value)
{
    FastDBBatch *batch;
    OK(fastdb_batch_create(NULL, &batch));
    OK(fastdb_batch_put(batch, "pa", 2, &value, sizeof(value)));
    OK(fastdb_batch_put(batch, "pb", 2, &value, sizeof(value)));
    OK(fastdb_batch_commit(db, batch));
    fastdb_batch_destroy(batch);
}

int main(void)
{
    FastDB *db;
    OK(fastdb_open(NULL, &db));
    uint64_t start = 0, revision;
    OK(fastdb_put(db, "counter", 7, &start, sizeof(start)));
    pthread_t threads[THREADS];
    for (size_t i = 0; i < THREADS; ++i)
        CHECK(pthread_create(&threads[i], NULL, increment, db) == 0);
    for (size_t i = 0; i < THREADS; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(fetch(db, &revision) == THREADS * INCREMENTS);
    CHECK(revision == THREADS * INCREMENTS + 1);

    ScanTest test = {.db = db};
    atomic_init(&test.finished, false);
    CHECK(pthread_barrier_init(&test.barrier, NULL, 2) == 0);
    write_pair(db, 0);
    CHECK(pthread_create(&threads[0], NULL, scan, &test) == 0);
    barrier_wait(&test.barrier);
    for (uint64_t i = 1; i <= BATCHES; ++i)
        write_pair(db, i);
    atomic_store(&test.finished, true);
    CHECK(pthread_join(threads[0], NULL) == 0);
    CHECK(pthread_barrier_destroy(&test.barrier) == 0);
    OK(fastdb_close(db));
}
