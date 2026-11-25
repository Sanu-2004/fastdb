#include "disk_fixture.h"
#include "engine.h"

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

typedef struct Gate {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t foreground;
    FDBFsOp operation;
    size_t remaining;
    int error;
    bool reached;
    bool released;
} Gate;

static const unsigned char old_value[120] = {1};
static const unsigned char new_value[120] = {2};
static const FastDBSlice old_bytes = {old_value, sizeof(old_value)};
static const FastDBSlice new_bytes = {new_value, sizeof(new_value)};

static FDBFsFault enter_gate(void *context, FDBFsOp operation, size_t size)
{
    (void)size;
    Gate *gate = context;
    if (pthread_equal(pthread_self(), gate->foreground) || operation != gate->operation)
        return (FDBFsFault){0};
    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    int error = 0;
    if (gate->remaining != 0 && --gate->remaining == 0) {
        gate->reached = true;
        CHECK(pthread_cond_signal(&gate->condition) == 0);
        while (!gate->released)
            CHECK(pthread_cond_wait(&gate->condition, &gate->mutex) == 0);
        error = gate->error;
    }
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
    return (FDBFsFault){.error = error};
}

static void wait_for_gate(Gate *gate)
{
    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    while (!gate->reached)
        CHECK(pthread_cond_wait(&gate->condition, &gate->mutex) == 0);
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void release_gate(Gate *gate)
{
    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    gate->released = true;
    CHECK(pthread_cond_signal(&gate->condition) == 0);
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
}

static FastDBOptions *background_options(const char *path)
{
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    size_t node_bytes;
    CHECK(fdb_node_size((FastDBSlice){"k", 1}, old_bytes, &node_bytes));
    STATUS(fastdb_options_set_lsm_flush_threshold(options, 128 * 1024), FASTDB_INVALID_ARGUMENT);
    OK(fastdb_options_set_lsm_flush_threshold(options, 3 * node_bytes));
    return options;
}

static void install_frozen(FastDB *db)
{
    OK(fastdb_put(db, "k", 1, old_bytes.data, old_bytes.size));
    OK(fastdb_put(db, "a", 1, old_bytes.data, old_bytes.size));
    OK(fastdb_put(db, "z", 1, old_bytes.data, old_bytes.size));
}

static void update_while_frozen(FastDB *db)
{
    expect_value(db, (FastDBSlice){"k", 1}, old_bytes, 1);
    OK(fastdb_compare_and_swap(db, "k", 1, 1, new_bytes.data, new_bytes.size));
    OK(fastdb_delete(db, "a", 1));
    OK(fastdb_put(db, "x", 1, new_bytes.data, new_bytes.size));
    /* This frame rotates the new active WAL while the worker owns an earlier output ID. */
    OK(fastdb_put(db, "y", 1, new_bytes.data, new_bytes.size));
    STATUS(fastdb_put(db, "q", 1, NULL, 0), FASTDB_BUSY);
    STATUS(fastdb_compact(db), FASTDB_BUSY);
    FastDBStats stats;
    OK(fastdb_stats(db, &stats));
    CHECK(stats.record_count == 7 && stats.revision == 7 && stats.memory_bytes <= 128 * 1024);
}

static void check_visible(FastDB *db)
{
    expect_value(db, (FastDBSlice){"k", 1}, new_bytes, 4);
    expect_missing(db, (FastDBSlice){"a", 1});
    expect_value(db, (FastDBSlice){"z", 1}, old_bytes, 3);
    expect_value(db, (FastDBSlice){"x", 1}, new_bytes, 6);
    expect_value(db, (FastDBSlice){"y", 1}, new_bytes, 7);
    expect_missing(db, (FastDBSlice){"q", 1});
}

static void concurrent_build(int error)
{
    char path[] = "/tmp/fastdb-bg-build.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = background_options(path);
    Gate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                 .foreground = pthread_self(), .operation = FDB_FS_WRITE, .remaining = 5,
                 .error = error};
    const FDBFs fs = {enter_gate, &gate};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    install_frozen(db);
    wait_for_gate(&gate);
    update_while_frozen(db);
    check_visible(db);
    release_gate(&gate);
    if (error != 0) {
        CHECK(pthread_mutex_lock(&db->mutex) == 0);
        while (db->flush->error == FASTDB_OK)
            CHECK(pthread_cond_wait(&db->flush->condition, &db->mutex) == 0);
        CHECK(db->flush->error == FASTDB_IO_ERROR && db->flush->frozen.count == 3);
        CHECK(pthread_mutex_unlock(&db->mutex) == 0);
        check_visible(db);
        STATUS(fastdb_put(db, "q", 1, NULL, 0), FASTDB_IO_ERROR);
    }
    OK(fastdb_flush(db));
    check_visible(db);
    OK(fastdb_close(db));
    CHECK(pthread_cond_destroy(&gate.condition) == 0);
    CHECK(pthread_mutex_destroy(&gate.mutex) == 0);
    OK(fastdb_options_set_lsm_flush_threshold(options, 0));
    OK(fastdb_open(options, &db));
    check_visible(db);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void crash_during_build(void)
{
    char path[] = "/tmp/fastdb-bg-crash.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = background_options(path);
    int channel[2];
    CHECK(pipe(channel) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        CHECK(close(channel[0]) == 0);
        Gate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                     .foreground = pthread_self(), .operation = FDB_FS_WRITE, .remaining = 5};
        const FDBFs fs = {enter_gate, &gate};
        FastDB *db;
        OK(fdb_open_with_fs(options, &fs, &db));
        install_frozen(db);
        wait_for_gate(&gate);
        update_while_frozen(db);
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
    OK(fastdb_options_set_lsm_flush_threshold(options, 0));
    FastDB *db;
    OK(fastdb_open(options, &db));
    check_visible(db);
    OK(fastdb_put(db, "q", 1, NULL, 0));
    expect_value(db, (FastDBSlice){"q", 1}, (FastDBSlice){0}, 8);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void failed_rotation_keeps_active(void)
{
    char path[] = "/tmp/fastdb-bg-rotate.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = background_options(path);
    Gate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                 .foreground = pthread_self(), .operation = FDB_FS_RENAME, .remaining = 2,
                 .error = EIO};
    const FDBFs fs = {enter_gate, &gate};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    install_frozen(db);
    wait_for_gate(&gate);
    release_gate(&gate);
    CHECK(pthread_mutex_lock(&db->mutex) == 0);
    while (db->flush->error == FASTDB_OK)
        CHECK(pthread_cond_wait(&db->flush->condition, &db->mutex) == 0);
    CHECK(db->flush->error == FASTDB_IO_ERROR && db->flush->frozen.count == 0 && db->tree.count == 3);
    CHECK(pthread_mutex_unlock(&db->mutex) == 0);
    expect_value(db, (FastDBSlice){"k", 1}, old_bytes, 1);
    STATUS(fastdb_put(db, "p", 1, NULL, 0), FASTDB_IO_ERROR);
    OK(fastdb_flush(db));
    OK(fastdb_put(db, "p", 1, NULL, 0));
    expect_value(db, (FastDBSlice){"p", 1}, (FastDBSlice){0}, 4);
    OK(fastdb_close(db));
    CHECK(pthread_cond_destroy(&gate.condition) == 0);
    CHECK(pthread_mutex_destroy(&gate.mutex) == 0);
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void *close_db(void *arg)
{
    OK(fastdb_close(arg));
    return NULL;
}

static void close_during_build(void)
{
    char path[] = "/tmp/fastdb-bg-close.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = background_options(path);
    Gate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                 .foreground = pthread_self(), .operation = FDB_FS_WRITE, .remaining = 5};
    const FDBFs fs = {enter_gate, &gate};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    install_frozen(db);
    wait_for_gate(&gate);
    update_while_frozen(db);
    pthread_t closer;
    CHECK(pthread_create(&closer, NULL, close_db, db) == 0);
    CHECK(pthread_mutex_lock(&db->mutex) == 0);
    while (!db->flush->stopping)
        CHECK(pthread_cond_wait(&db->flush->condition, &db->mutex) == 0);
    CHECK(pthread_mutex_unlock(&db->mutex) == 0);
    release_gate(&gate);
    CHECK(pthread_join(closer, NULL) == 0);
    CHECK(pthread_cond_destroy(&gate.condition) == 0);
    CHECK(pthread_mutex_destroy(&gate.mutex) == 0);
    OK(fastdb_options_set_lsm_flush_threshold(options, 0));
    OK(fastdb_open(options, &db));
    check_visible(db);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    concurrent_build(0);
    concurrent_build(ENOSPC);
    failed_rotation_keeps_active();
    close_during_build();
    crash_during_build();
}
