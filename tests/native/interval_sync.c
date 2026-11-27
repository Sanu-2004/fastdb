#include "disk_fixture.h"
#include "engine.h"

#include <errno.h>
#include <time.h>

typedef struct SyncProbe {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t foreground;
    struct timespec first_sync;
    size_t foreground_syncs;
    size_t worker_syncs;
    bool release;
    bool fail;
} SyncProbe;

static FDBFsFault watch_sync(void *context, FDBFsOp operation, size_t size)
{
    (void)size;
    SyncProbe *probe = context;
    if (operation != FDB_FS_SYNC)
        return (FDBFsFault){0};
    CHECK(pthread_mutex_lock(&probe->mutex) == 0);
    bool foreground = pthread_equal(probe->foreground, pthread_self());
    if (foreground) {
        ++probe->foreground_syncs;
    } else if (++probe->worker_syncs == 1) {
        CHECK(clock_gettime(CLOCK_MONOTONIC, &probe->first_sync) == 0);
        CHECK(pthread_cond_signal(&probe->condition) == 0);
        while (!probe->release)
            CHECK(pthread_cond_wait(&probe->condition, &probe->mutex) == 0);
    }
    bool fail = probe->fail;
    CHECK(pthread_mutex_unlock(&probe->mutex) == 0);
    return (FDBFsFault){.error = fail ? EIO : 0};
}

static uint64_t milliseconds(struct timespec time)
{
    return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}

static void sync_after_ack(FastDBMode mode, bool fail)
{
    char path[] = "/tmp/fastdb-periodic-sync.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    const uint32_t interval = mode == FASTDB_MODE_LSM ? 20U : 100U;
    FastDBOptions *options = disk_options_with_mode(path, mode, FASTDB_DURABILITY_INTERVAL);
    STATUS(fastdb_options_set_wal_interval(options, 0), FASTDB_INVALID_ARGUMENT);
    if (mode == FASTDB_MODE_LSM)
        OK(fastdb_options_set_wal_interval(options, interval));
    SyncProbe probe = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                       .foreground = pthread_self()};
    const FDBFs fs = {watch_sync, &probe};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    struct timespec before;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    OK(fastdb_put(db, "a", 1, "acknowledged", 12));
    CHECK(pthread_mutex_lock(&probe.mutex) == 0);
    while (probe.worker_syncs == 0)
        CHECK(pthread_cond_wait(&probe.condition, &probe.mutex) == 0);
    CHECK(milliseconds(probe.first_sync) >= milliseconds(before) + interval);
    size_t foreground_syncs = probe.foreground_syncs;
    probe.fail = fail;
    probe.release = true;
    CHECK(pthread_cond_signal(&probe.condition) == 0);
    CHECK(pthread_mutex_unlock(&probe.mutex) == 0);

    if (fail) {
        STATUS(fastdb_put(db, "b", 1, NULL, 0), FASTDB_COMMIT_UNCERTAIN);
        void *value;
        size_t size;
        STATUS(fastdb_get(db, "a", 1, &value, &size), FASTDB_COMMIT_UNCERTAIN);
        CHECK(value == NULL && size == 0);
    } else {
        expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"acknowledged", 12}, 1);
    }
    STATUS(fastdb_close(db), fail ? FASTDB_COMMIT_UNCERTAIN : FASTDB_OK);
    CHECK(probe.worker_syncs == 1 && probe.foreground_syncs == foreground_syncs);
    CHECK(pthread_cond_destroy(&probe.condition) == 0);
    CHECK(pthread_mutex_destroy(&probe.mutex) == 0);
    OK(fastdb_open(options, &db));
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){"acknowledged", 12}, 1);
    expect_missing(db, (FastDBSlice){"b", 1});
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void close_drains_pending_sync(bool fail)
{
    char path[] = "/tmp/fastdb-close-sync.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_INTERVAL);
    OK(fastdb_options_set_wal_interval(options, 60000));
    SyncProbe probe = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                       .foreground = pthread_self(), .release = true};
    const FDBFs fs = {watch_sync, &probe};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    OK(fastdb_put(db, "k", 1, "pending", 7));
    CHECK(pthread_mutex_lock(&probe.mutex) == 0);
    size_t writes = probe.foreground_syncs;
    probe.fail = fail;
    CHECK(pthread_mutex_unlock(&probe.mutex) == 0);
    STATUS(fastdb_close(db), fail ? FASTDB_COMMIT_UNCERTAIN : FASTDB_OK);
    CHECK(probe.foreground_syncs == writes + 1 && probe.worker_syncs == 0);
    CHECK(pthread_cond_destroy(&probe.condition) == 0);
    CHECK(pthread_mutex_destroy(&probe.mutex) == 0);

    OK(fastdb_open(options, &db));
    expect_value(db, (FastDBSlice){"k", 1}, (FastDBSlice){"pending", 7}, 1);
    OK(fastdb_flush(db));
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void rotate_dirty_segment(void)
{
    char path[] = "/tmp/fastdb-interval-rotation.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_INTERVAL);
    OK(fastdb_options_set_wal_interval(options, 60000));
    SyncProbe probe = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                       .foreground = pthread_self(), .release = true};
    const FDBFs fs = {watch_sync, &probe};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    const unsigned char data[120] = {7};
    OK(fastdb_put(db, "a", 1, data, sizeof(data)));
    OK(fastdb_put(db, "b", 1, data, sizeof(data)));
    size_t before_rotation = probe.foreground_syncs;
    OK(fastdb_put(db, "c", 1, data, sizeof(data)));
    size_t after_rotation = probe.foreground_syncs;
    CHECK(after_rotation > before_rotation);
    OK(fastdb_close(db));
    CHECK(probe.foreground_syncs == after_rotation + 1 && probe.worker_syncs == 0);
    CHECK(pthread_cond_destroy(&probe.condition) == 0);
    CHECK(pthread_mutex_destroy(&probe.mutex) == 0);

    OK(fastdb_open(options, &db));
    expect_value(db, (FastDBSlice){"a", 1}, (FastDBSlice){data, sizeof(data)}, 1);
    expect_value(db, (FastDBSlice){"b", 1}, (FastDBSlice){data, sizeof(data)}, 2);
    expect_value(db, (FastDBSlice){"c", 1}, (FastDBSlice){data, sizeof(data)}, 3);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    FastDBOptions *memory;
    OK(fastdb_options_create(&memory));
    STATUS(fastdb_options_set_wal_interval(memory, 20), FASTDB_INVALID_ARGUMENT);
    fastdb_options_destroy(memory);
    sync_after_ack(FASTDB_MODE_LSM, false);
    sync_after_ack(FASTDB_MODE_MEMORY_PERSISTENT, true);
    close_drains_pending_sync(false);
    close_drains_pending_sync(true);
    rotate_dirty_segment();
}
