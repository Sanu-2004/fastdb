#include "fs.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { LEGACY_LINE_BYTES = 32760 };

typedef struct LegacyLayout {
    char wal[sizeof("wal_18446744073709551615.log")];
} LegacyLayout;

static FastDBStatus inspect_name(void *context, const char *name)
{
    LegacyLayout *layout = context;
    if (strcmp(name, "tables") == 0 || strcmp(name, "MANIFEST") == 0 ||
        strncmp(name, "sstable_", 8) == 0 || strncmp(name, "wal-", 4) == 0 ||
        strncmp(name, "sst-", 4) == 0 || strncmp(name, "chk-", 4) == 0)
        return FASTDB_UNSUPPORTED;
    if (strncmp(name, "wal_", 4) != 0)
        return FASTDB_OK;
    size_t size = strlen(name);
    if (layout->wal[0] != '\0' || size < 9 || size >= sizeof(layout->wal) ||
        strcmp(name + size - 4, ".log") != 0)
        return FASTDB_UNSUPPORTED;
    for (size_t i = 4; i < size - 4; ++i) {
        if (name[i] < '0' || name[i] > '9')
            return FASTDB_UNSUPPORTED;
    }
    memcpy(layout->wal, name, size + 1);
    return FASTDB_OK;
}

static FastDBStatus write_hex(FILE *out, const unsigned char *bytes, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        if (fputc(digits[bytes[i] >> 4], out) == EOF || fputc(digits[bytes[i] & 15U], out) == EOF)
            return FASTDB_IO_ERROR;
    }
    return FASTDB_OK;
}

static FastDBStatus export_line(FILE *out, const unsigned char *line, size_t size)
{
    if (size < 7 || memcmp(line, "SET ", 4) != 0)
        return FASTDB_CORRUPTION;
    const unsigned char *key = line + 4;
    const unsigned char *space = memchr(key, ' ', size - 4);
    if (space == NULL || space == key || space + 1 == line + size)
        return FASTDB_CORRUPTION;
    for (const unsigned char *p = key; p < space; ++p) {
        if (*p <= 0x20 || *p == 0x7f)
            return FASTDB_CORRUPTION;
    }
    if (fputs("put ", out) < 0 || write_hex(out, key, (size_t)(space - key)) != FASTDB_OK ||
        fputc(' ', out) == EOF ||
        write_hex(out, space + 1, (size_t)(line + size - space - 1)) != FASTDB_OK ||
        fputc('\n', out) == EOF)
        return FASTDB_IO_ERROR;
    return FASTDB_OK;
}

static FastDBStatus export_stream(FILE *in, FILE *out)
{
    unsigned char line[LEGACY_LINE_BYTES];
    size_t used = 0;
    int c;
    while ((c = fgetc(in)) != EOF) {
        if (c == '\n') {
            FastDBStatus status = export_line(out, line, used);
            if (status != FASTDB_OK)
                return status;
            used = 0;
        } else {
            if (c == 0 || c == '\r')
                return FASTDB_CORRUPTION;
            if (used == sizeof(line))
                return FASTDB_LIMIT_EXCEEDED;
            line[used++] = (unsigned char)c;
        }
    }
    if (ferror(in))
        return FASTDB_IO_ERROR;
    return used == 0 ? FASTDB_OK : FASTDB_CORRUPTION;
}

static bool same_file(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static bool unchanged(const struct stat *a, const struct stat *b)
{
    return same_file(a, b) && a->st_size == b->st_size &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static FastDBStatus confirm_source(const FDBDirectory *source, const char *wal,
                                   const struct stat *before)
{
    struct stat after;
    if (fstatat(source->fd, wal, &after, AT_SYMLINK_NOFOLLOW) != 0 || !unchanged(before, &after))
        return FASTDB_BUSY;
    LegacyLayout layout = {0};
    FastDBStatus status = fdb_dir_visit(source, inspect_name, &layout);
    if (status == FASTDB_OK && strcmp(wal, layout.wal) != 0)
        status = FASTDB_BUSY;
    return status;
}

static FastDBStatus read_source(const FDBDirectory *source, FILE *out)
{
    LegacyLayout layout = {0};
    FastDBStatus status = fdb_dir_visit(source, inspect_name, &layout);
    if (status != FASTDB_OK)
        return status;
    if (layout.wal[0] == '\0')
        return FASTDB_UNSUPPORTED;
    int fd;
    status = fdb_dir_open_file(source, layout.wal, FDB_FILE_READ, &fd);
    if (status != FASTDB_OK)
        return status;
    struct stat before;
    FILE *in = fdopen(fd, "r");
    if (in == NULL) {
        (void)fdb_fs_close(fd);
        return FASTDB_IO_ERROR;
    }
    status = fstat(fd, &before) == 0 ? export_stream(in, out) : FASTDB_IO_ERROR;
    if (status == FASTDB_OK)
        status = confirm_source(source, layout.wal, &before);
    int closed = fclose(in);
    return status == FASTDB_OK && closed != 0 ? FASTDB_IO_ERROR : status;
}

static FastDBStatus export_file(const FDBDirectory *source, int dest, const char *name)
{
    char temp[64];
    int fd = -1;
    for (unsigned int attempt = 0; attempt < 128; ++attempt) {
        (void)snprintf(temp, sizeof(temp), ".fastdb-export-%jd-%u.tmp", (intmax_t)getpid(), attempt);
        if (strcmp(temp, name) == 0)
            continue;
        fd = openat(dest, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST)
            break;
    }
    if (fd < 0)
        return FASTDB_IO_ERROR;
    FILE *out = fdopen(fd, "w");
    FastDBStatus status = FASTDB_IO_ERROR;
    if (out != NULL) {
        status = read_source(source, out);
        if (status == FASTDB_OK)
            status = fflush(out) == 0 ? fdb_fs_sync(NULL, fd) : FASTDB_IO_ERROR;
        if (fclose(out) != 0 && status == FASTDB_OK)
            status = FASTDB_IO_ERROR;
    } else {
        (void)fdb_fs_close(fd);
    }
    if (status == FASTDB_OK && linkat(dest, temp, dest, name, 0) != 0)
        status = errno == EEXIST ? FASTDB_CONFLICT : FASTDB_IO_ERROR;
    if (unlinkat(dest, temp, 0) != 0 && status == FASTDB_OK)
        status = FASTDB_IO_ERROR;
    return status == FASTDB_OK ? fdb_fs_sync(NULL, dest) : status;
}

static FastDBStatus export_legacy(const char *source_path, char *destination)
{
    char *separator = strrchr(destination, '/');
    const char *name = separator == NULL ? destination : separator + 1;
    if (*name == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return FASTDB_INVALID_ARGUMENT;
    const char *parent = ".";
    if (separator == destination) {
        parent = "/";
    } else if (separator != NULL) {
        *separator = '\0';
        parent = destination;
    }
    int fd = open(source_path, O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return FASTDB_IO_ERROR;
    int dest = open(parent, O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    FastDBStatus status = FASTDB_IO_ERROR;
    if (dest >= 0) {
        struct stat a, b;
        if (fstat(fd, &a) == 0 && fstat(dest, &b) == 0) {
            FDBDirectory source = {.fd = fd, .lock_fd = -1};
            status = same_file(&a, &b) ? FASTDB_INVALID_ARGUMENT :
                     export_file(&source, dest, name);
        }
        if (fdb_fs_close(dest) != FASTDB_OK && status == FASTDB_OK)
            status = FASTDB_IO_ERROR;
    }
    if (fdb_fs_close(fd) != FASTDB_OK && status == FASTDB_OK)
        status = FASTDB_IO_ERROR;
    return status;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fputs("usage: fastdb-legacy-export OFFLINE_SOURCE_DIRECTORY NEW_COMMAND_FILE\n", stderr);
        return EXIT_FAILURE;
    }
    FastDBStatus status = export_legacy(argv[1], argv[2]);
    if (status != FASTDB_OK)
        fprintf(stderr, "legacy export: %s\n", fastdb_status_string(status));
    return status == FASTDB_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
