#include <fastdb/fastdb.h>
#include <stdio.h>

int main(void)
{
    FastDB *db;
    FastDBStatus status = fastdb_open(NULL, &db);
    if (status != FASTDB_OK)
        return 1;

    const unsigned char key[] = {'k', 0, 'v'};
    status = fastdb_put(db, key, sizeof(key), "hello", 5);
    if (status == FASTDB_OK) {
        void *value;
        size_t size;
        status = fastdb_get(db, key, sizeof(key), &value, &size);
        if (status == FASTDB_OK) {
            if (fwrite(value, 1, size, stdout) != size || putchar('\n') == EOF)
                status = FASTDB_IO_ERROR;
            fastdb_free(value);
        }
    }
    FastDBStatus close_status = fastdb_close(db);
    if (status != FASTDB_OK || close_status != FASTDB_OK) {
        fprintf(stderr, "%s\n", fastdb_status_string(status != FASTDB_OK ? status : close_status));
        return 1;
    }
    return fflush(stdout) == EOF ? 1 : 0;
}
