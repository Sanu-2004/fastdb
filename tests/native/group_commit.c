#include "disk_fixture.h"
#include "disk.h"
#include "group_commit.h"

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/wait.h>
#include <time.h>

typedef struct SyncGate {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    size_t syncs;
    bool armed;
    bool release;
    bool fail;
} SyncGate;

typedef struct Request {
    FastDB *db;
    FastDBSlice key;
    FastDBSlice value;
    FastDBBatch *batch;
    bool cas;
    uint64_t expected;
    atomic_bool done;
    FastDBStatus status;
} Request;

static FDBFsFault gate_sync(void *context, FDBFsOp operation, size_t size)
{
    (void)size;
    SyncGate *gate = context;
    if (operation != FDB_FS_SYNC)
        return (FDBFsFault){0};
    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    bool fail = false;
    if (gate->armed && ++gate->syncs == 1) {
        CHECK(pthread_cond_signal(&gate->condition) == 0);
        while (!gate->release)
            CHECK(pthread_cond_wait(&gate->condition, &gate->mutex) == 0);
        fail = gate->fail;
    }
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
    return (FDBFsFault){.error = fail ? EIO : 0};
}

static void arm_sync(SyncGate *gate)
{
    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    gate->armed = true;
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void *submit(void *arg)
{
    Request *request = arg;
    FastDBSlice key = request->key, value = request->value;
    if (request->batch != NULL)
        request->status = fastdb_batch_commit(request->db, request->batch);
    else if (request->cas)
        request->status = fastdb_compare_and_swap(request->db, key.data, key.size,
                                                   request->expected, value.data, value.size);
    else
        request->status = fastdb_put(request->db, key.data, key.size, value.data, value.size);
    atomic_store(&request->done, true);
    return NULL;
}

static void wait_pending(FastDB *db, size_t count)
{
    struct timespec start, now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    FDBGroupCommit *group = fdb_disk_group_commit(db->disk);
    while (fdb_group_commit_pending(group) < count) {
        CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec - start.tv_sec < 3);
        sched_yield();
    }
}

static void wait_sync(SyncGate *gate)
{
    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    while (gate->syncs == 0)
        CHECK(pthread_cond_wait(&gate->condition, &gate->mutex) == 0);
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void release_sync(SyncGate *gate)
{
    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    gate->release = true;
    CHECK(pthread_cond_signal(&gate->condition) == 0);
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void destroy_gate(SyncGate *gate)
{
    CHECK(pthread_cond_destroy(&gate->condition) == 0);
    CHECK(pthread_mutex_destroy(&gate->mutex) == 0);
}

static FastDBOptions *prime_wal(const char *path, FastDBMode mode, size_t requests,
                                size_t bytes, uint32_t delay_ms)
{
    FastDBOptions *options = disk_options_with_mode(path, mode, FASTDB_DURABILITY_SYNC);
    FastDBGroupCommitOptions group;
    fastdb_group_commit_options_default(&group);
    STATUS(fastdb_options_set_group_commit(options, &group), FASTDB_INVALID_ARGUMENT);
    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "k", 1, "seed", 4));
    OK(fastdb_close(db));

    FastDBPersistentOptions storage = options->storage;
    storage.durability = FASTDB_DURABILITY_GROUP_COMMIT;
    OK(fastdb_options_set_storage(options, mode, path, &storage));
    group.max_pending_requests = 0;
    STATUS(fastdb_options_set_group_commit(options, &group), FASTDB_INVALID_ARGUMENT);
    group.max_pending_requests = requests;
    group.max_pending_bytes = bytes;
    group.collection_delay_ms = delay_ms;
    OK(fastdb_options_set_group_commit(options, &group));
    return options;
}

static void group_acknowledgements(FastDBMode mode, bool fail)
{
    char path[] = "/tmp/fastdb-group-ack.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = prime_wal(path, mode, 3, 4096, 4000);
    SyncGate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                     .fail = fail};
    const FDBFs fs = {gate_sync, &gate};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    arm_sync(&gate);
    FastDBBatch *batch;
    OK(fastdb_batch_create(options, &batch));
    OK(fastdb_batch_expect(batch, "k", 1, fail ? 2 : 3));
    OK(fastdb_batch_put(batch, "x", 1, "marker", 6));
    OK(fastdb_batch_delete(batch, "k", 1));
    Request requests[] = {
        {.db = db, .key = {"k", 1}, .value = {"first", 5}},
        {.db = db, .key = {"k", 1}, .value = {"second", 6}, .cas = true,
         .expected = fail ? 999 : 2},
        {.db = db, .batch = batch},
    };
    pthread_t threads[3];
    for (size_t i = 0; i < 3; ++i) {
        CHECK(pthread_create(&threads[i], NULL, submit, &requests[i]) == 0);
        wait_pending(db, i + 1);
        if (i == 0)
            STATUS(fastdb_close(db), FASTDB_BUSY);
    }
    wait_sync(&gate);
    CHECK(pthread_mutex_trylock(&db->mutex) == EBUSY);
    for (size_t i = 0; i < 3; ++i)
        CHECK(!atomic_load(&requests[i].done));
    STATUS(fastdb_put(db, "extra", 5, NULL, 0), FASTDB_BUSY);
    release_sync(&gate);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        STATUS(requests[i].status, fail ? FASTDB_COMMIT_UNCERTAIN : FASTDB_OK);
    }
    fastdb_batch_destroy(batch);
    STATUS(fastdb_close(db), fail ? FASTDB_COMMIT_UNCERTAIN : FASTDB_OK);
    CHECK(gate.syncs == 1);
    destroy_gate(&gate);

    OK(fastdb_open(options, &db));
    expect_missing(db, (FastDBSlice){"k", 1});
    expect_missing(db, (FastDBSlice){"extra", 5});
    expect_value(db, (FastDBSlice){"x", 1}, (FastDBSlice){"marker", 6}, fail ? 3 : 4);
    FastDBStats stats;
    OK(fastdb_stats(db, &stats));
    CHECK(stats.revision == (fail ? 4U : 5U));
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void rotate_during_group(void)
{
    char path[] = "/tmp/fastdb-group-rotate.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = prime_wal(path, FASTDB_MODE_LSM, 3, 4096, 4000);
    SyncGate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    const FDBFs fs = {gate_sync, &gate};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    arm_sync(&gate);
    const unsigned char payload[120] = {42};
    Request requests[] = {
        {.db = db, .key = {"a", 1}, .value = {payload, sizeof(payload)}},
        {.db = db, .key = {"b", 1}, .value = {payload, sizeof(payload)}},
        {.db = db, .key = {"c", 1}, .value = {payload, sizeof(payload)}},
    };
    pthread_t threads[3];
    for (size_t i = 0; i < 3; ++i) {
        CHECK(pthread_create(&threads[i], NULL, submit, &requests[i]) == 0);
        wait_pending(db, i + 1);
    }
    wait_sync(&gate);
    for (size_t i = 0; i < 3; ++i)
        CHECK(!atomic_load(&requests[i].done));
    release_sync(&gate);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        OK(requests[i].status);
    }
    OK(fastdb_close(db));
    CHECK(gate.syncs > 1);
    destroy_gate(&gate);
    OK(fastdb_open(options, &db));
    for (size_t i = 0; i < 3; ++i)
        expect_value(db, requests[i].key, requests[i].value, i + 2);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void bounded_queue_bytes(void)
{
    char path[] = "/tmp/fastdb-group-bytes.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = prime_wal(path, FASTDB_MODE_LSM, 4, 350, 0);
    SyncGate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};
    const FDBFs fs = {gate_sync, &gate};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    arm_sync(&gate);
    const unsigned char payload[120] = {21};
    STATUS(fastdb_compare_and_swap(db, "k", 1, 999, NULL, 0), FASTDB_CONFLICT);
    CHECK(gate.syncs == 0);
    FastDBBatch *batch;
    OK(fastdb_batch_create(options, &batch));
    OK(fastdb_batch_put(batch, "a", 1, payload, sizeof(payload)));
    OK(fastdb_batch_put(batch, "b", 1, payload, sizeof(payload)));
    STATUS(fastdb_batch_commit(db, batch), FASTDB_LIMIT_EXCEEDED);
    fastdb_batch_destroy(batch);

    Request request = {.db = db, .key = {"a", 1}, .value = {payload, sizeof(payload)}};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, submit, &request) == 0);
    wait_sync(&gate);
    STATUS(fastdb_put(db, "b", 1, payload, sizeof(payload)), FASTDB_BUSY);
    CHECK(!atomic_load(&request.done));
    release_sync(&gate);
    CHECK(pthread_join(thread, NULL) == 0);
    OK(request.status);
    OK(fastdb_put(db, "b", 1, payload, sizeof(payload)));
    expect_value(db, (FastDBSlice){"b", 1}, request.value, 3);
    OK(fastdb_close(db));
    destroy_gate(&gate);
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void acknowledged_group_survives_process_kill(void)
{
    char path[] = "/tmp/fastdb-group-crash.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = prime_wal(path, FASTDB_MODE_LSM, 2, 4096, 4000);
    int channel[2];
    CHECK(pipe(channel) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        CHECK(close(channel[0]) == 0);
        FastDB *db;
        OK(fastdb_open(options, &db));
        Request first = {.db = db, .key = {"a", 1}, .value = {"A", 1}};
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, submit, &first) == 0);
        wait_pending(db, 1);
        FastDBBatch *batch;
        OK(fastdb_batch_create(options, &batch));
        OK(fastdb_batch_expect(batch, "a", 1, 2));
        OK(fastdb_batch_put(batch, "b", 1, "B", 1));
        OK(fastdb_batch_delete(batch, "k", 1));
        OK(fastdb_batch_commit(db, batch));
        CHECK(pthread_join(thread, NULL) == 0);
        OK(first.status);
        fastdb_batch_destroy(batch);
        CHECK(write(channel[1], "D", 1) == 1);
        for (;;)
            (void)pause();
    }
    CHECK(close(channel[1]) == 0);
    char done;
    CHECK(read(channel[0], &done, 1) == 1 && done == 'D');
    CHECK(kill(child, SIGKILL) == 0);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    CHECK(close(channel[0]) == 0);
    FastDB *db;
    OK(fastdb_open(options, &db));
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"A", 1}, 2);
    expect_value(db, (FastDBSlice){"b", 1}, (FastDBSlice){"B", 1}, 3);
    expect_missing(db, (FastDBSlice){"k", 1});
    FastDBStats stats;
    OK(fastdb_stats(db, &stats));
    CHECK(stats.revision == 4);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    group_acknowledgements(FASTDB_MODE_LSM, false);
    group_acknowledgements(FASTDB_MODE_MEMORY_PERSISTENT, false);
    group_acknowledgements(FASTDB_MODE_LSM, true);
    rotate_during_group();
    bounded_queue_bytes();
    acknowledged_group_survives_process_kill();
}
