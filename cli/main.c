#include "fastdb/fastdb.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LINE_BYTES = 65536, PAGE_RECORDS = 100 };

static const char help[] =
    "fastdb-cli [--lsm DIRECTORY | --memory-persistent DIRECTORY | --help | --version]\n"
    "Read commands from standard input. With no arguments, use memory storage.\n"
    "  put HEX_KEY HEX_VALUE     Use - for an empty value\n"
    "  get HEX_KEY\n"
    "  delete HEX_KEY\n"
    "  scan [HEX_PREFIX]         Memory modes, up to 100 rows in key order\n"
    "  flush                     Publish an L0 table in LSM mode\n"
    "  compact                   Promote on-disk LSM tables\n"
    "  checkpoint                Publish a persistent memory checkpoint\n"
    "  stats\n"
    "  quit\n";

static int nibble(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool parse_hex(char *token, bool allow_empty, FastDBSlice *out)
{
    *out = (FastDBSlice){0};
    if (allow_empty && (token == NULL || strcmp(token, "-") == 0))
        return true;
    size_t length = strlen(token);
    if (length == 0 || length % 2 != 0)
        return false;
    unsigned char *bytes = (unsigned char *)token;
    for (size_t i = 0; i < length; i += 2) {
        int high = nibble(bytes[i]);
        int low = nibble(bytes[i + 1]);
        if (high < 0 || low < 0)
            return false;
        bytes[i / 2] = (unsigned char)(high * 16 + low);
    }
    *out = (FastDBSlice){bytes, length / 2};
    return true;
}

static void print_hex(FastDBSlice slice)
{
    const unsigned char *bytes = slice.data;
    if (slice.size == 0)
        putchar('-');
    for (size_t i = 0; i < slice.size; ++i)
        printf("%02x", (unsigned int)bytes[i]);
}

static FastDBStatus print_value(FastDB *db, FastDBSlice key)
{
    void *value;
    size_t size;
    uint64_t revision;
    FastDBStatus status = fastdb_get_with_revision(db, key.data, key.size, &value, &size,
                                                    &revision);
    if (status == FASTDB_NOT_FOUND) {
        puts("NOT_FOUND");
        return FASTDB_OK;
    }
    if (status == FASTDB_OK) {
        printf("VALUE %" PRIu64 " ", revision);
        print_hex((FastDBSlice){value, size});
        putchar('\n');
        fastdb_free(value);
    }
    return status;
}

static FastDBStatus print_scan(FastDB *db, FastDBSlice prefix)
{
    FastDBIterator *iterator;
    FastDBStatus status = fastdb_scan_prefix(db, prefix, PAGE_RECORDS, &iterator);
    if (status != FASTDB_OK)
        return status;
    FastDBSlice key, value;
    uint64_t revision;
    while ((status = fastdb_iterator_next(iterator, &key, &value, &revision)) == FASTDB_OK) {
        print_hex(key);
        printf(" %" PRIu64 " ", revision);
        print_hex(value);
        putchar('\n');
    }
    if (fastdb_iterator_has_more(iterator))
        puts("MORE");
    fastdb_iterator_close(iterator);
    return status == FASTDB_NOT_FOUND ? FASTDB_OK : status;
}

static FastDBStatus print_stats(FastDB *db)
{
    FastDBStats stats;
    FastDBStatus status = fastdb_stats(db, &stats);
    if (status == FASTDB_OK)
        printf("revision=%" PRIu64 " records=%zu memory_bytes=%zu\n",
               stats.revision, stats.record_count, stats.memory_bytes);
    return status;
}

static FastDBStatus run_command(FastDB *db, char *line, bool *quit)
{
    char *save;
    const char *name = strtok_r(line, " \t\r", &save);
    if (name == NULL)
        return FASTDB_OK;
    char *key_token = strtok_r(NULL, " \t\r", &save);
    if (strcmp(name, "quit") == 0 && key_token == NULL) {
        *quit = true;
        return FASTDB_OK;
    }
    if (strcmp(name, "stats") == 0 && key_token == NULL)
        return print_stats(db);
    if (key_token == NULL && (strcmp(name, "flush") == 0 || strcmp(name, "checkpoint") == 0 ||
                              strcmp(name, "compact") == 0)) {
        FastDBStatus status;
        if (strcmp(name, "flush") == 0)
            status = fastdb_flush(db);
        else if (strcmp(name, "compact") == 0)
            status = fastdb_compact(db);
        else
            status = fastdb_checkpoint(db);
        if (status == FASTDB_OK)
            puts("OK");
        return status;
    }
    char *value_token = key_token == NULL ? NULL : strtok_r(NULL, " \t\r", &save);
    if (value_token != NULL && strtok_r(NULL, " \t\r", &save) != NULL)
        return FASTDB_INVALID_ARGUMENT;

    FastDBSlice key, value;
    if (strcmp(name, "scan") == 0)
        return value_token == NULL && parse_hex(key_token, true, &key) ?
               print_scan(db, key) : FASTDB_INVALID_ARGUMENT;
    if (key_token == NULL || !parse_hex(key_token, false, &key))
        return FASTDB_INVALID_ARGUMENT;
    if (strcmp(name, "get") == 0 && value_token == NULL)
        return print_value(db, key);

    FastDBStatus status;
    if (strcmp(name, "put") == 0 && value_token != NULL && parse_hex(value_token, true, &value))
        status = fastdb_put(db, key.data, key.size, value.data, value.size);
    else if (strcmp(name, "delete") == 0 && value_token == NULL)
        status = fastdb_delete(db, key.data, key.size);
    else
        return FASTDB_INVALID_ARGUMENT;
    if (status == FASTDB_OK)
        puts("OK");
    return status;
}

static int shell(FastDB *db)
{
    char line[LINE_BYTES];
    int exit_code = EXIT_SUCCESS;
    bool quit = false;
    while (!quit) {
        size_t length = 0;
        bool invalid = false;
        int c;
        while ((c = getchar()) != EOF && c != '\n') {
            if (c == 0 || length == sizeof(line) - 1)
                invalid = true;
            else
                line[length++] = (char)c;
        }
        if (c == EOF && (ferror(stdin) || (length == 0 && !invalid)))
            break;
        line[length] = '\0';
        FastDBStatus status = invalid ? FASTDB_INVALID_ARGUMENT : run_command(db, line, &quit);
        if (status != FASTDB_OK) {
            fprintf(stderr, "error: %s\n", fastdb_status_string(status));
            exit_code = EXIT_FAILURE;
        }
        if (c == EOF)
            break;
    }
    if (ferror(stdin) || fflush(stdout) == EOF || ferror(stdout)) {
        fputs("error: command stream I/O failed\n", stderr);
        return EXIT_FAILURE;
    }
    return exit_code;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0)
        return fputs(help, stdout) < 0 || fflush(stdout) == EOF ? EXIT_FAILURE : EXIT_SUCCESS;
    if (argc == 2 && strcmp(argv[1], "--version") == 0)
        return puts(fastdb_version()) < 0 || fflush(stdout) == EOF ? EXIT_FAILURE : EXIT_SUCCESS;
    FastDBMode mode = FASTDB_MODE_MEMORY;
    if (argc == 3 && strcmp(argv[1], "--lsm") == 0)
        mode = FASTDB_MODE_LSM;
    else if (argc == 3 && strcmp(argv[1], "--memory-persistent") == 0)
        mode = FASTDB_MODE_MEMORY_PERSISTENT;
    else if (argc != 1) {
        fputs(help, stderr);
        return EXIT_FAILURE;
    }
    FastDBOptions *options = NULL;
    FastDBStatus status = FASTDB_OK;
    if (mode != FASTDB_MODE_MEMORY) {
        status = fastdb_options_create(&options);
        if (status == FASTDB_OK)
            status = fastdb_options_set_storage(options, mode, argv[2], NULL);
    }
    FastDB *db;
    if (status == FASTDB_OK)
        status = fastdb_open(options, &db);
    fastdb_options_destroy(options);
    if (status != FASTDB_OK) {
        fprintf(stderr, "error: %s\n", fastdb_status_string(status));
        return EXIT_FAILURE;
    }
    int exit_code = shell(db);
    status = fastdb_close(db);
    if (status != FASTDB_OK) {
        fprintf(stderr, "error: %s\n", fastdb_status_string(status));
        return EXIT_FAILURE;
    }
    return exit_code;
}
