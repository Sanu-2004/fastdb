#define _GNU_SOURCE
#include "fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

_Static_assert(sizeof(off_t) == sizeof(int64_t), "FastDB requires 64-bit off_t");

static const char lock_name[] = ".fastdb.lock";

static FDBFsFault fault(const FDBFs *fs, FDBFsOp op, size_t requested)
{
    return fs == NULL || fs->before == NULL ? (FDBFsFault){0} :
           fs->before(fs->context, op, requested);
}

static FastDBStatus file_error(int error)
{
    switch (error) {
    case ENOENT: return FASTDB_NOT_FOUND;
    case ENOMEM: return FASTDB_OUT_OF_MEMORY;
    case EMFILE:
    case ENFILE: return FASTDB_LIMIT_EXCEEDED;
    case ELOOP:
    case ENOTDIR: return FASTDB_CORRUPTION;
    default: return FASTDB_IO_ERROR;
    }
}

static bool valid_name(const char *name)
{
    return name != NULL && *name != '\0' && strchr(name, '/') == NULL &&
           strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
           strcmp(name, lock_name) != 0;
}

static FastDBStatus open_regular(const FDBFs *fs, int root, const char *name, int flags, int *out)
{
    int fd;
    do {
        int error = fault(fs, FDB_FS_OPEN, 0).error;
        if (error != 0) {
            errno = error;
            fd = -1;
        } else {
            fd = openat(root, name, flags | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        }
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return file_error(errno);
    uint64_t bytes;
    FastDBStatus status = fdb_fs_size(fd, &bytes);
    if (status == FASTDB_OK)
        *out = fd;
    else
        (void)fdb_fs_close(fd);
    return status;
}

FastDBStatus fdb_dir_lock_checked(const char *path, const FDBFs *fs,
                                  FDBDirectoryGuard guard, FDBDirectory *out)
{
    *out = (FDBDirectory){.fd = -1, .lock_fd = -1};
    if (path == NULL || *path == '\0')
        return FASTDB_INVALID_ARGUMENT;
    size_t length = strlen(path);
    if (length > 1 && path[length - 1] == '/')
        return FASTDB_INVALID_ARGUMENT;
    int fd;
    do {
        fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return file_error(errno);

    if (guard != NULL) {
        const FDBDirectory unlocked = {.fd = fd, .lock_fd = -1};
        FastDBStatus status = guard(&unlocked);
        if (status != FASTDB_OK) {
            (void)fdb_fs_close(fd);
            return status;
        }
    }
    int lock_fd;
    FastDBStatus status = open_regular(fs, fd, lock_name, O_RDWR | O_CREAT, &lock_fd);
    if (status == FASTDB_OK) {
        int result;
        do {
            result = flock(lock_fd, LOCK_EX | LOCK_NB);
        } while (result < 0 && errno == EINTR);
        if (result == 0) {
            *out = (FDBDirectory){fd, lock_fd, fs == NULL ? (FDBFs){0} : *fs};
            return FASTDB_OK;
        }
        status = errno == EWOULDBLOCK ? FASTDB_BUSY : file_error(errno);
        (void)fdb_fs_close(lock_fd);
    }
    (void)fdb_fs_close(fd);
    return status;
}

FastDBStatus fdb_dir_lock(const char *path, const FDBFs *fs, FDBDirectory *out)
{
    return fdb_dir_lock_checked(path, fs, NULL, out);
}

FastDBStatus fdb_fs_close(int fd)
{
    /* Linux releases the descriptor even when close reports EINTR. */
    return close(fd) == 0 ? FASTDB_OK : FASTDB_IO_ERROR;
}

FastDBStatus fdb_dir_close(FDBDirectory *dir)
{
    FastDBStatus close_dir = fdb_fs_close(dir->fd);
    FastDBStatus close_lock = fdb_fs_close(dir->lock_fd);
    *dir = (FDBDirectory){.fd = -1, .lock_fd = -1};
    return close_dir == FASTDB_OK ? close_lock : close_dir;
}

FastDBStatus fdb_dir_open_file(const FDBDirectory *dir, const char *name,
                                FDBFileMode mode, int *out)
{
    *out = -1;
    if (!valid_name(name))
        return FASTDB_INVALID_ARGUMENT;
    int flags;
    switch (mode) {
    case FDB_FILE_READ: flags = O_RDONLY; break;
    case FDB_FILE_READ_WRITE: flags = O_RDWR; break;
    case FDB_FILE_CREATE_NEW: flags = O_RDWR | O_CREAT | O_EXCL; break;
    default: return FASTDB_INVALID_ARGUMENT;
    }
    return open_regular(&dir->fs, dir->fd, name, flags, out);
}

static FastDBStatus rename_file(const FDBDirectory *dir, const char *source, const char *target,
                                  unsigned int flags)
{
    if (!valid_name(source) || !valid_name(target))
        return FASTDB_INVALID_ARGUMENT;
    int error = fault(&dir->fs, FDB_FS_RENAME, 0).error;
    if (error != 0)
        return file_error(error);
    int result = flags == 0 ? renameat(dir->fd, source, dir->fd, target) :
                 renameat2(dir->fd, source, dir->fd, target, flags);
    return result == 0 ? FASTDB_OK : file_error(errno);
}

FastDBStatus fdb_dir_rename(const FDBDirectory *dir, const char *source, const char *target)
{
    return rename_file(dir, source, target, 0);
}

FastDBStatus fdb_dir_rename_new(const FDBDirectory *dir, const char *source, const char *target)
{
    return rename_file(dir, source, target, RENAME_NOREPLACE);
}

FastDBStatus fdb_dir_unlink(const FDBDirectory *dir, const char *name)
{
    if (!valid_name(name))
        return FASTDB_INVALID_ARGUMENT;
    int error = fault(&dir->fs, FDB_FS_UNLINK, 0).error;
    if (error != 0)
        return file_error(error);
    return unlinkat(dir->fd, name, 0) == 0 ? FASTDB_OK : file_error(errno);
}

FastDBStatus fdb_fs_size(int fd, uint64_t *size)
{
    *size = 0;
    struct stat info;
    int result;
    do {
        result = fstat(fd, &info);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        return FASTDB_IO_ERROR;
    if (!S_ISREG(info.st_mode) || info.st_size < 0)
        return FASTDB_CORRUPTION;
    *size = (uint64_t)info.st_size;
    return FASTDB_OK;
}

FastDBStatus fdb_dir_available_bytes(const FDBDirectory *dir, uint64_t *bytes)
{
    if (dir == NULL || dir->fd < 0 || bytes == NULL)
        return FASTDB_INVALID_ARGUMENT;
    FDBFsFault injection = fault(&dir->fs, FDB_FS_SPACE, 0);
    if (injection.error != 0)
        return file_error(injection.error);
    if (injection.space_override) {
        *bytes = injection.available_bytes;
        return FASTDB_OK;
    }
    struct statvfs info;
    int result;
    do {
        result = fstatvfs(dir->fd, &info);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        return file_error(errno);
    uint64_t unit = info.f_frsize != 0 ? (uint64_t)info.f_frsize : (uint64_t)info.f_bsize;
    uint64_t blocks = (uint64_t)info.f_bavail;
    if (unit != 0 && blocks > UINT64_MAX / unit)
        *bytes = UINT64_MAX;
    else
        *bytes = blocks * unit;
    return FASTDB_OK;
}

static bool valid_offset(uint64_t offset, size_t length)
{
    return offset <= INT64_MAX && length <= (uint64_t)INT64_MAX - offset;
}

static size_t transfer_size(size_t remaining, FDBFsFault injection)
{
    size_t size = remaining > (size_t)SSIZE_MAX ? (size_t)SSIZE_MAX : remaining;
    if (injection.max_transfer != 0 && injection.max_transfer < size)
        size = injection.max_transfer;
    return size;
}

FastDBStatus fdb_fs_read(const FDBFs *fs, int fd, uint64_t offset, void *buffer,
                         size_t size, size_t *done)
{
    *done = 0;
    if (!valid_offset(offset, size))
        return FASTDB_LIMIT_EXCEEDED;
    unsigned char *bytes = buffer;
    while (*done < size) {
        size_t left = size - *done;
        FDBFsFault injection = fault(fs, FDB_FS_READ, left);
        ssize_t result;
        if (injection.error != 0) {
            errno = injection.error;
            result = -1;
        } else {
            result = pread(fd, bytes + *done, transfer_size(left, injection),
                           (off_t)(offset + *done));
        }
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0)
            return FASTDB_IO_ERROR;
        if (result == 0)
            break;
        *done += (size_t)result;
    }
    return FASTDB_OK;
}

FastDBStatus fdb_fs_write(const FDBFs *fs, int fd, uint64_t offset, const void *buffer,
                          size_t size, size_t *done)
{
    *done = 0;
    if (!valid_offset(offset, size))
        return FASTDB_LIMIT_EXCEEDED;
    const unsigned char *bytes = buffer;
    while (*done < size) {
        size_t left = size - *done;
        FDBFsFault injection = fault(fs, FDB_FS_WRITE, left);
        ssize_t result;
        if (injection.error != 0) {
            errno = injection.error;
            result = -1;
        } else {
            result = pwrite(fd, bytes + *done, transfer_size(left, injection),
                            (off_t)(offset + *done));
        }
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return FASTDB_IO_ERROR;
        *done += (size_t)result;
    }
    return FASTDB_OK;
}

FastDBStatus fdb_fs_sync(const FDBFs *fs, int fd)
{
    int result;
    do {
        int error = fault(fs, FDB_FS_SYNC, 0).error;
        if (error != 0) {
            errno = error;
            result = -1;
        } else {
            result = fsync(fd);
        }
    } while (result < 0 && errno == EINTR);
    return result == 0 ? FASTDB_OK : FASTDB_IO_ERROR;
}

FastDBStatus fdb_dir_sync(const FDBDirectory *dir)
{
    return fdb_fs_sync(&dir->fs, dir->fd);
}

FastDBStatus fdb_dir_visit(const FDBDirectory *dir, FDBDirectoryVisit visit, void *context)
{
    int fd;
    do {
        fd = openat(dir->fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return file_error(errno);
    DIR *entries = fdopendir(fd);
    if (entries == NULL) {
        FastDBStatus status = file_error(errno);
        (void)fdb_fs_close(fd);
        return status;
    }
    FastDBStatus status = FASTDB_OK;
    while (status == FASTDB_OK) {
        errno = 0;
        const struct dirent *entry = readdir(entries);
        if (entry == NULL) {
            if (errno != 0)
                status = FASTDB_IO_ERROR;
            break;
        }
        if (valid_name(entry->d_name))
            status = visit(context, entry->d_name);
    }
    int error = closedir(entries);
    return status == FASTDB_OK && error != 0 ? FASTDB_IO_ERROR : status;
}

FastDBStatus fdb_dir_is_regular(const FDBDirectory *dir, const char *name, bool *out)
{
    *out = false;
    if (!valid_name(name))
        return FASTDB_INVALID_ARGUMENT;
    struct stat info;
    int result;
    do {
        result = fstatat(dir->fd, name, &info, AT_SYMLINK_NOFOLLOW);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        return file_error(errno);
    *out = S_ISREG(info.st_mode);
    return FASTDB_OK;
}

FastDBStatus fdb_fs_truncate(const FDBFs *fs, int fd, uint64_t size)
{
    if (!valid_offset(size, 0))
        return FASTDB_LIMIT_EXCEEDED;
    int result;
    do {
        int error = fault(fs, FDB_FS_TRUNCATE, 0).error;
        if (error != 0) {
            errno = error;
            result = -1;
        } else {
            result = ftruncate(fd, (off_t)size);
        }
    } while (result < 0 && errno == EINTR);
    return result == 0 ? FASTDB_OK : FASTDB_IO_ERROR;
}
