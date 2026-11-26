#include "disk_fixture.h"
#include "engine.h"

#include <errno.h>

typedef struct ReadGate {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t foreground;
    bool entered;
    bool release;
} ReadGate;

typedef struct ReadRequest {
    FastDB *db;
    FastDBStatus expected;
    FastDBSlice value;
    uint64_t revision;
} ReadRequest;

static FDBFsFault hold_open(void *context, FDBFsOp operation, size_t size)
{
    (void)size;
    ReadGate *gate = context;
    if (operation != FDB_FS_OPEN || pthread_equal(pthread_self(), gate->foreground))
        return (FDBFsFault){0};
    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    if (!gate->entered) {
        gate->entered = true;
        CHECK(pthread_cond_signal(&gate->condition) == 0);
        while (!gate->release)
            CHECK(pthread_cond_wait(&gate->condition, &gate->mutex) == 0);
    }
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
    return (FDBFsFault){0};
}

static void *read_k(void *arg)
{
    ReadRequest *request = arg;
    void *value;
    size_t size;
    uint64_t revision;
    STATUS(fastdb_get_with_revision(request->db, "k", 1, &value, &size, &revision), request->expected);
    if (request->expected == FASTDB_OK) {
        CHECK(revision == request->revision && size == request->value.size);
        CHECK(memcmp(value, request->value.data, size) == 0);
    } else {
        CHECK(value == NULL && size == 0 && revision == 0);
    }
    fastdb_free(value);
    return NULL;
}

static void wait_read(ReadGate *gate)
{
    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    while (!gate->entered)
        CHECK(pthread_cond_wait(&gate->condition, &gate->mutex) == 0);
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void finish_read(ReadGate *gate, pthread_t reader)
{
    CHECK(pthread_mutex_lock(&gate->mutex) == 0);
    gate->release = true;
    CHECK(pthread_cond_signal(&gate->condition) == 0);
    CHECK(pthread_mutex_unlock(&gate->mutex) == 0);
    CHECK(pthread_join(reader, NULL) == 0);
}

static void destroy_gate(ReadGate *gate)
{
    CHECK(pthread_cond_destroy(&gate->condition) == 0);
    CHECK(pthread_mutex_destroy(&gate->mutex) == 0);
}

static void collect_tables(const char *path, char names[2][25])
{
    DIR *dir = opendir(path);
    CHECK(dir != NULL);
    size_t count = 0;
    const struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "sst-", 4) == 0) {
            CHECK(count < 2 && strlen(entry->d_name) == 24);
            memcpy(names[count++], entry->d_name, sizeof(*names));
        }
    }
    CHECK(count == 2 && closedir(dir) == 0);
}

static void check_old_files(const char *path, char names[2][25], bool present)
{
    DIR *dir = opendir(path);
    CHECK(dir != NULL);
    for (size_t i = 0; i < 2; ++i) {
        int exists = faccessat(dirfd(dir), names[i], F_OK, 0);
        CHECK(present ? exists == 0 : exists < 0 && errno == ENOENT);
    }
    CHECK(closedir(dir) == 0);
}

static void across_compaction(bool start_with_tombstone)
{
    char path[] = "/tmp/fastdb-read-view.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDBCompactionOptions policy;
    fastdb_compaction_options_default(&policy);
    policy.max_levels = 2;
    OK(fastdb_options_set_compaction(options, &policy));

    ReadGate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                     .foreground = pthread_self()};
    const FDBFs fs = {hold_open, &gate};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    OK(fastdb_put(db, "k", 1, "old", 3));
    OK(fastdb_flush(db));
    OK(fastdb_compact(db));
    if (start_with_tombstone)
        OK(fastdb_delete(db, "k", 1));
    else
        OK(fastdb_put(db, "k", 1, "middle", 6));
    OK(fastdb_flush(db));

    char old_names[2][25];
    collect_tables(path, old_names);
    FastDBStats before, during;
    OK(fastdb_stats(db, &before));
    ReadRequest request = {db, start_with_tombstone ? FASTDB_NOT_FOUND : FASTDB_OK, {"middle", 6}, 2};
    pthread_t reader;
    CHECK(pthread_create(&reader, NULL, read_k, &request) == 0);
    wait_read(&gate);
    OK(fastdb_stats(db, &during));
    CHECK(during.memory_bytes > before.memory_bytes && during.memory_bytes <= 128 * 1024);

    OK(fastdb_compact(db));
    check_old_files(path, old_names, true);
    if (start_with_tombstone)
        expect_missing(db, (FastDBSlice){"k", 1});
    else
        expect_value(db, (FastDBSlice){"k", 1}, (FastDBSlice){"middle", 6}, 2);
    OK(fastdb_put(db, "k", 1, "new", 3));
    expect_value(db, (FastDBSlice){"k", 1}, (FastDBSlice){"new", 3}, 3);
    STATUS(fastdb_flush(db), FASTDB_BUSY);
    STATUS(fastdb_close(db), FASTDB_BUSY);

    finish_read(&gate, reader);
    OK(fastdb_flush(db));
    check_old_files(path, old_names, false);
    OK(fastdb_close(db));
    destroy_gate(&gate);
    OK(fastdb_open(options, &db));
    expect_value(db, (FastDBSlice){"k", 1}, (FastDBSlice){"new", 3}, 3);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void wait_worker_flush(FastDB *db)
{
    CHECK(pthread_mutex_lock(&db->mutex) == 0);
    while ((db->tree.count != 0 || db->flush->frozen.count != 0) && db->flush->error == FASTDB_OK)
        CHECK(pthread_cond_wait(&db->flush->condition, &db->mutex) == 0);
    CHECK(db->flush->error == FASTDB_OK);
    CHECK(pthread_mutex_unlock(&db->mutex) == 0);
}

static void across_background_flush(void)
{
    char path[] = "/tmp/fastdb-read-worker.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "k", 1, "old", 3));
    OK(fastdb_flush(db));
    OK(fastdb_close(db));

    size_t threshold;
    CHECK(fdb_node_size((FastDBSlice){"w", 1}, (FastDBSlice){0}, &threshold));
    OK(fastdb_options_set_lsm_flush_threshold(options, threshold));
    ReadGate gate = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER,
                     .foreground = pthread_self()};
    const FDBFs fs = {hold_open, &gate};
    OK(fdb_open_with_fs(options, &fs, &db));
    ReadRequest request = {db, FASTDB_OK, {"old", 3}, 1};
    pthread_t reader;
    CHECK(pthread_create(&reader, NULL, read_k, &request) == 0);
    wait_read(&gate);
    OK(fastdb_put(db, "w", 1, NULL, 0));
    wait_worker_flush(db);
    expect_value(db, (FastDBSlice){"w", 1}, (FastDBSlice){0}, 2);
    OK(fastdb_put(db, "x", 1, NULL, 0));
    STATUS(fastdb_put(db, "y", 1, NULL, 0), FASTDB_BUSY);
    STATUS(fastdb_flush(db), FASTDB_BUSY);

    finish_read(&gate, reader);
    wait_worker_flush(db);
    expect_value(db, (FastDBSlice){"x", 1}, (FastDBSlice){0}, 3);
    expect_missing(db, (FastDBSlice){"y", 1});
    OK(fastdb_close(db));
    destroy_gate(&gate);
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    across_compaction(false);
    across_compaction(true);
    across_background_flush();
}
