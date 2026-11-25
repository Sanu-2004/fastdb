#include "check.h"
#include "recovery.h"

#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const FDBManifestLimits manifest_limits = {8, 4096, 16, 3};
static const FDBWalLimits wal_limits = {16, 64, 8, 256, 1024};
static const FDBSSTLimits sst_limits = {16, 64, 80, 128, 512, 128, 10};

typedef struct CrashSignal {
    int pipe;
    size_t write_steps;
    bool armed;
} CrashSignal;

static const FDBRecord acknowledged[] = {
    {{"a", 1}, {"safe", 4}, 1, 0, FDB_PUT},
    {{"b", 1}, {0}, 2, 0, FDB_DELETE},
};

static const FDBRecord interrupted[] = {
    {{"a", 1}, {0}, 3, 0, FDB_DELETE},
    {{"b", 1}, {"uncommitted", 11}, 4, 0, FDB_PUT},
};

static FDBFsFault pause_after_partial_write(void *context, FDBFsOp op, size_t requested)
{
    CrashSignal *signal = context;
    if (!signal->armed || op != FDB_FS_WRITE)
        return (FDBFsFault){0};
    if (++signal->write_steps == 1) {
        CHECK(requested > FDB_WAL_FRAME_BYTES + 2U);
        return (FDBFsFault){.max_transfer = FDB_WAL_FRAME_BYTES + 2U};
    }
    CHECK(write(signal->pipe, "T", 1) == 1);
    for (;;)
        (void)pause();
}

static void write_child(const char *path, int pipe)
{
    CrashSignal signal = {.pipe = pipe};
    const FDBFs fs = {pause_after_partial_write, &signal};
    FDBDirectory dir;
    OK(fdb_dir_lock(path, &fs, &dir));
    FDBWalSet set;
    OK(fdb_lsm_recover(&dir, &manifest_limits, &wal_limits, &sst_limits, NULL, &set));
    OK(fdb_wal_set_append(&set, acknowledged, 2, true));
    CHECK(write(pipe, "A", 1) == 1);
    signal.armed = true;
    (void)fdb_wal_set_append(&set, interrupted, 2, true);
    _exit(1);
}

static FastDBStatus check_acknowledged(void *context, const unsigned char *payload, size_t size,
                                        size_t count, uint64_t sequence)
{
    size_t *frames = context;
    CHECK(*frames == 0 && count == 2 && sequence == 1);
    for (size_t i = 0; i < count; ++i) {
        FDBRecord decoded;
        size_t used;
        OK(fdb_record_decode(payload, size, 16, 64, &decoded, &used));
        CHECK(decoded.sequence == acknowledged[i].sequence && decoded.kind == acknowledged[i].kind);
        CHECK(fdb_compare(decoded.key, acknowledged[i].key) == 0);
        CHECK(fdb_compare(decoded.value, acknowledged[i].value) == 0);
        payload += used;
        size -= used;
    }
    CHECK(size == 0);
    ++*frames;
    return FASTDB_OK;
}

int main(void)
{
    char path[] = "/tmp/fastdb-crash.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FDBDirectory dir;
    OK(fdb_dir_lock(path, NULL, &dir));
    FDBManifest manifest;
    OK(fdb_manifest_new(FDB_MODE_LSM, &manifest));
    OK(fdb_manifest_publish(&dir, &manifest_limits, 0, &manifest));
    OK(fdb_dir_close(&dir));

    int signals[2];
    CHECK(pipe(signals) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        CHECK(close(signals[0]) == 0);
        write_child(path, signals[1]);
    }
    CHECK(close(signals[1]) == 0);
    char event;
    CHECK(read(signals[0], &event, 1) == 1 && event == 'A');
    CHECK(read(signals[0], &event, 1) == 1 && event == 'T');
    CHECK(kill(child, SIGKILL) == 0);
    int child_status;
    CHECK(waitpid(child, &child_status, 0) == child);
    CHECK(WIFSIGNALED(child_status) && WTERMSIG(child_status) == SIGKILL);
    CHECK(close(signals[0]) == 0);

    OK(fdb_dir_lock(path, NULL, &dir));
    FDBWalSet set;
    size_t frames = 0;
    OK(fdb_lsm_recover(&dir, &manifest_limits, &wal_limits, &sst_limits,
                       &(FDBWalReplay){.visit = check_acknowledged, .context = &frames}, &set));
    CHECK(frames == 1 && set.wal.sequence == 2);
    OK(fdb_wal_set_append(&set, interrupted, 2, true));
    CHECK(set.wal.sequence == 4);
    OK(fdb_wal_set_close(&set));
    OK(fdb_dir_unlink(&dir, "wal-00000000000000000001"));
    OK(fdb_dir_unlink(&dir, "MANIFEST"));
    OK(fdb_dir_close(&dir));
    char lock_path[sizeof(path) + sizeof("/.fastdb.lock")];
    CHECK(snprintf(lock_path, sizeof(lock_path), "%s/.fastdb.lock", path) > 0);
    CHECK(unlink(lock_path) == 0 && rmdir(path) == 0);
}
