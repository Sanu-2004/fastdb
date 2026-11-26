#include "check.h"
#include "fs.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct Injection {
    FDBFsOp operation;
    int error;
    bool interrupt_first;
    size_t chunk;
    size_t calls;
    size_t fail_on;
    bool space_override;
    uint64_t available_bytes;
} Injection;

static FDBFsFault inject(void *context, FDBFsOp op, size_t requested)
{
    (void)requested;
    Injection *injection = context;
    if (op != injection->operation)
        return (FDBFsFault){0};
    if (op == FDB_FS_SPACE && injection->space_override)
        return (FDBFsFault){.space_override = true,
                            .available_bytes = injection->available_bytes};
    ++injection->calls;
    if (injection->interrupt_first && injection->calls == 1)
        return (FDBFsFault){.error = EINTR};
    if (injection->calls == injection->fail_on)
        return (FDBFsFault){.error = injection->error};
    return (FDBFsFault){.max_transfer = injection->chunk};
}

static void check_lock(const char *path)
{
    FDBDirectory conflict;
    STATUS(fdb_dir_lock(path, NULL, &conflict), FASTDB_BUSY);
    CHECK(conflict.fd == -1 && conflict.lock_fd == -1);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0)
        _exit(fdb_dir_lock(path, NULL, &conflict) == FASTDB_BUSY ? 0 : 1);
    int result;
    CHECK(waitpid(child, &result, 0) == child);
    CHECK(WIFEXITED(result) && WEXITSTATUS(result) == 0);
}

static void test_io(FDBDirectory *dir, Injection *injection, int fd)
{
    const uint64_t large_offset = UINT64_C(0x100001000);
    const char payload[] = "new data";
    size_t done;
    uint64_t size;
    *injection = (Injection){.operation = FDB_FS_WRITE, .error = ENOSPC,
                            .interrupt_first = true, .chunk = 2, .fail_on = 3};
    STATUS(fdb_fs_write(&dir->fs, fd, large_offset, payload, sizeof(payload), &done),
           FASTDB_IO_ERROR);
    CHECK(done == 2 && injection->calls == 3);
    OK(fdb_fs_size(fd, &size));
    CHECK(size == large_offset + 2);
    *injection = (Injection){.operation = FDB_FS_WRITE, .interrupt_first = true, .chunk = 2};
    OK(fdb_fs_write(&dir->fs, fd, large_offset, payload, sizeof(payload), &done));
    CHECK(done == sizeof(payload) && injection->calls > 3);

    char buffer[sizeof(payload) + 1] = {0};
    *injection = (Injection){.operation = FDB_FS_READ, .error = EIO,
                            .interrupt_first = true, .chunk = 3, .fail_on = 3};
    STATUS(fdb_fs_read(&dir->fs, fd, large_offset, buffer, sizeof(buffer), &done), FASTDB_IO_ERROR);
    CHECK(done == 3 && memcmp(buffer, payload, done) == 0);
    *injection = (Injection){.operation = FDB_FS_READ, .interrupt_first = true, .chunk = 3};
    OK(fdb_fs_read(&dir->fs, fd, large_offset, buffer, sizeof(buffer), &done));
    CHECK(done == sizeof(payload) && memcmp(buffer, payload, done) == 0);
    STATUS(fdb_fs_read(&dir->fs, fd, UINT64_MAX, buffer, 1, &done), FASTDB_LIMIT_EXCEEDED);
    CHECK(done == 0);
    STATUS(fdb_fs_write(&dir->fs, fd, INT64_MAX, payload, 1, &done), FASTDB_LIMIT_EXCEEDED);
    CHECK(done == 0);

    *injection = (Injection){.operation = FDB_FS_SYNC, .error = EIO,
                            .interrupt_first = true, .fail_on = 2};
    STATUS(fdb_fs_sync(&dir->fs, fd), FASTDB_IO_ERROR);
    CHECK(injection->calls == 2);
    OK(fdb_fs_sync(&dir->fs, fd));

    *injection = (Injection){.operation = FDB_FS_TRUNCATE, .error = EIO,
                            .interrupt_first = true, .fail_on = 2};
    STATUS(fdb_fs_truncate(&dir->fs, fd, 0), FASTDB_IO_ERROR);
    OK(fdb_fs_size(fd, &size));
    CHECK(size == large_offset + sizeof(payload));
    OK(fdb_fs_truncate(&dir->fs, fd, 0));
    OK(fdb_fs_size(fd, &size));
    CHECK(size == 0);
}

int main(void)
{
    char path[] = "/tmp/fastdb-fs.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    Injection injection = {.operation = FDB_FS_OPEN, .interrupt_first = true};
    const FDBFs fs = {inject, &injection};
    FDBDirectory dir;
    OK(fdb_dir_lock(path, &fs, &dir));
    CHECK(injection.calls == 2);
    CHECK((fcntl(dir.fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((fcntl(dir.lock_fd, F_GETFD) & FD_CLOEXEC) != 0);
    uint64_t available;
    injection = (Injection){.operation = FDB_FS_SPACE, .space_override = true,
                            .available_bytes = 123456};
    OK(fdb_dir_available_bytes(&dir, &available));
    CHECK(available == 123456);
    injection = (Injection){.operation = FDB_FS_SPACE, .error = EIO, .fail_on = 1};
    STATUS(fdb_dir_available_bytes(&dir, &available), FASTDB_IO_ERROR);
    check_lock(path);

    int fd;
    STATUS(fdb_dir_open_file(&dir, "../external", FDB_FILE_CREATE_NEW, &fd),
           FASTDB_INVALID_ARGUMENT);
    STATUS(fdb_dir_unlink(&dir, ".fastdb.lock"), FASTDB_INVALID_ARGUMENT);
    CHECK(symlinkat("pending", dir.fd, "link") == 0);
    STATUS(fdb_dir_open_file(&dir, "link", FDB_FILE_READ, &fd), FASTDB_CORRUPTION);
    OK(fdb_dir_unlink(&dir, "link"));
    CHECK(mkfifoat(dir.fd, "pipe", 0600) == 0);
    STATUS(fdb_dir_open_file(&dir, "pipe", FDB_FILE_READ, &fd), FASTDB_CORRUPTION);
    OK(fdb_dir_unlink(&dir, "pipe"));

    STATUS(fdb_dir_open_file(&dir, "pending", FDB_FILE_READ, &fd), FASTDB_NOT_FOUND);
    OK(fdb_dir_open_file(&dir, "pending", FDB_FILE_CREATE_NEW, &fd));
    int duplicate;
    STATUS(fdb_dir_open_file(&dir, "pending", FDB_FILE_CREATE_NEW, &duplicate), FASTDB_IO_ERROR);
    CHECK(duplicate == -1);
    test_io(&dir, &injection, fd);
    OK(fdb_fs_close(fd));
    injection = (Injection){.operation = FDB_FS_RENAME, .error = EIO, .fail_on = 1};
    STATUS(fdb_dir_rename(&dir, "pending", "published"), FASTDB_IO_ERROR);
    STATUS(fdb_dir_open_file(&dir, "published", FDB_FILE_READ, &fd), FASTDB_NOT_FOUND);
    OK(fdb_dir_rename_new(&dir, "pending", "published"));
    OK(fdb_dir_open_file(&dir, "pending", FDB_FILE_CREATE_NEW, &fd));
    OK(fdb_fs_close(fd));
    STATUS(fdb_dir_rename_new(&dir, "pending", "published"), FASTDB_IO_ERROR);
    OK(fdb_dir_unlink(&dir, "pending"));
    OK(fdb_dir_sync(&dir));
    OK(fdb_dir_open_file(&dir, "published", FDB_FILE_READ, &fd));
    OK(fdb_fs_close(fd));
    OK(fdb_dir_unlink(&dir, "published"));
    OK(fdb_dir_close(&dir));

    OK(fdb_dir_lock(path, NULL, &dir));
    OK(fdb_dir_close(&dir));
    char lock_path[sizeof(path) + sizeof("/.fastdb.lock")];
    CHECK(snprintf(lock_path, sizeof(lock_path), "%s/.fastdb.lock", path) > 0);
    CHECK(unlink(lock_path) == 0 && rmdir(path) == 0);
}
