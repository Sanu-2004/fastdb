#include "disk_fixture.h"
#include "engine.h"

#include <signal.h>
#include <sys/wait.h>

typedef struct Boundary {
    FDBFsOp op;
    size_t occurrence;
    const char *description;
} Boundary;

typedef struct Crash {
    Boundary boundary;
    int pipe;
    bool armed;
} Crash;

static FDBFsFault pause_at_boundary(void *context, FDBFsOp op, size_t requested)
{
    (void)requested;
    Crash *test = context;
    if (!test->armed || op != test->boundary.op || --test->boundary.occurrence != 0)
        return (FDBFsFault){0};
    CHECK(write(test->pipe, "P", 1) == 1);
    for (;;)
        (void)pause();
}

typedef FastDBStatus (*Maintenance)(FastDB *);

static void interrupt(FastDBOptions *options, Maintenance maintain, Boundary boundary, int pipe)
{
    Crash test = {.boundary = boundary, .pipe = pipe};
    const FDBFs fs = {pause_at_boundary, &test};
    FastDB *db;
    OK(fdb_open_with_fs(options, &fs, &db));
    FastDBBatch *batch;
    OK(fastdb_batch_create(options, &batch));
    OK(fastdb_batch_expect(batch, "k", 1, 1));
    OK(fastdb_batch_delete(batch, "k", 1));
    OK(fastdb_batch_put(batch, "new", 3, "complete", 8));
    OK(fastdb_batch_commit(db, batch));
    fastdb_batch_destroy(batch);
    if (maintain == fastdb_compact)
        OK(fastdb_flush(db));
    test.armed = true;
    (void)maintain(db);
    _exit(1);
}

static void crash_and_reopen(FastDBMode mode, Maintenance maintain, Boundary boundary)
{
    fprintf(stderr, "mode=%d compact=%d killing before %s\n", mode,
            maintain == fastdb_compact, boundary.description);
    char path[] = "/tmp/fastdb-public-crash.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options_with_mode(path, mode, FASTDB_DURABILITY_SYNC);
    if (maintain == fastdb_compact) {
        FastDBCompactionOptions compaction;
        fastdb_compaction_options_default(&compaction);
        compaction.max_levels = 2;
        OK(fastdb_options_set_compaction(options, &compaction));
    }
    FastDB *db;
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "k", 1, "old", 3));
    OK(fastdb_put(db, "keep", 4, "base", 4));
    if (maintain == fastdb_compact)
        OK(fastdb_flush(db));
    OK(maintain(db));
    OK(fastdb_close(db));

    int pipes[2];
    CHECK(pipe(pipes) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        CHECK(close(pipes[0]) == 0);
        interrupt(options, maintain, boundary, pipes[1]);
    }
    CHECK(close(pipes[1]) == 0);
    char event;
    CHECK(read(pipes[0], &event, 1) == 1 && event == 'P');
    CHECK(kill(child, SIGKILL) == 0);
    int result;
    CHECK(waitpid(child, &result, 0) == child);
    CHECK(WIFSIGNALED(result) && WTERMSIG(result) == SIGKILL);
    CHECK(close(pipes[0]) == 0);

    OK(fastdb_open(options, &db));
    expect_missing(db, (FastDBSlice){"k", 1});
    expect_value(db, (FastDBSlice){"keep", 4}, (FastDBSlice){"base", 4}, 2);
    expect_value(db, (FastDBSlice){"new", 3}, (FastDBSlice){"complete", 8}, 4);
    OK(fastdb_put(db, "k", 1, NULL, 0));
    expect_value(db, (FastDBSlice){"k", 1}, (FastDBSlice){0}, 5);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    const Boundary boundaries[] = {
        {FDB_FS_WRITE, 1, "writing the file-ID reservation"},
        {FDB_FS_SYNC, 2, "syncing the reservation directory entry"},
        {FDB_FS_WRITE, 3, "writing the output data block"},
        {FDB_FS_SYNC, 3, "syncing the completed output"},
        {FDB_FS_RENAME, 2, "renaming the output"},
        {FDB_FS_SYNC, 4, "syncing the output directory entry"},
        {FDB_FS_RENAME, 3, "replacing the manifest"},
        {FDB_FS_SYNC, 6, "syncing the replacement manifest directory entry"},
        {FDB_FS_UNLINK, 1, "reclaiming an obsolete file"},
    };
    for (size_t i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
        crash_and_reopen(FASTDB_MODE_LSM, fastdb_flush, boundaries[i]);
        crash_and_reopen(FASTDB_MODE_MEMORY_PERSISTENT, fastdb_checkpoint, boundaries[i]);
        crash_and_reopen(FASTDB_MODE_LSM, fastdb_compact, boundaries[i]);
    }
}
