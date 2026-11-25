#ifndef FASTDB_FS_H
#define FASTDB_FS_H

#include "fastdb/fastdb.h"

typedef enum FDBFsOp {
    FDB_FS_READ, FDB_FS_WRITE, FDB_FS_SYNC, FDB_FS_OPEN, FDB_FS_RENAME, FDB_FS_UNLINK,
    FDB_FS_TRUNCATE
} FDBFsOp;

typedef struct FDBFsFault {
    int error;
    size_t max_transfer;
} FDBFsFault;

typedef struct FDBFs {
    FDBFsFault (*before)(void *context, FDBFsOp operation, size_t requested);
    void *context;
} FDBFs;

typedef struct FDBDirectory {
    int fd;
    int lock_fd;
    FDBFs fs;
} FDBDirectory;

typedef enum FDBFileMode {
    FDB_FILE_READ, FDB_FILE_READ_WRITE, FDB_FILE_CREATE_NEW
} FDBFileMode;

typedef FastDBStatus (*FDBDirectoryVisit)(void *context, const char *name);
typedef FastDBStatus (*FDBDirectoryGuard)(const FDBDirectory *dir);

FastDBStatus fdb_dir_lock(const char *path, const FDBFs *fs, FDBDirectory *out);
FastDBStatus fdb_dir_lock_checked(const char *path, const FDBFs *fs,
                                  FDBDirectoryGuard guard, FDBDirectory *out);
FastDBStatus fdb_dir_close(FDBDirectory *dir);
FastDBStatus fdb_dir_open_file(const FDBDirectory *dir, const char *name,
                                FDBFileMode mode, int *out);
FastDBStatus fdb_dir_rename(const FDBDirectory *dir, const char *source, const char *target);
FastDBStatus fdb_dir_rename_new(const FDBDirectory *dir, const char *source, const char *target);
FastDBStatus fdb_dir_unlink(const FDBDirectory *dir, const char *name);
FastDBStatus fdb_dir_sync(const FDBDirectory *dir);
FastDBStatus fdb_dir_visit(const FDBDirectory *dir, FDBDirectoryVisit visit, void *context);
FastDBStatus fdb_dir_is_regular(const FDBDirectory *dir, const char *name, bool *out);
FastDBStatus fdb_fs_close(int fd);
FastDBStatus fdb_fs_size(int fd, uint64_t *size);
/* A read may end early at EOF. Inspect done even if status is OK. */
FastDBStatus fdb_fs_read(const FDBFs *fs, int fd, uint64_t offset, void *buffer,
                         size_t size, size_t *done);
FastDBStatus fdb_fs_write(const FDBFs *fs, int fd, uint64_t offset, const void *buffer,
                          size_t size, size_t *done);
FastDBStatus fdb_fs_sync(const FDBFs *fs, int fd);
FastDBStatus fdb_fs_truncate(const FDBFs *fs, int fd, uint64_t size);

#endif
