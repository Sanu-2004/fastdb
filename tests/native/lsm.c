#include "disk_fixture.h"

#include <fcntl.h>

static void reopen_and_flush(void)
{
    char path[] = "/tmp/fastdb-lsm-api.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDB *db, *other;
    OK(fastdb_open(options, &db));
    STATUS(fastdb_open(options, &other), FASTDB_BUSY);
    CHECK(other == NULL);
    OK(fastdb_open(NULL, &other));
    const FastDBSlice binary = {"k\0\xff", 3}, empty = {"e", 1}, rotating = {"r", 1};
    const FastDBSlice old = {"old", 3};
    const FastDBSlice current = {"\0\xff", 2};
    OK(fastdb_put(db, binary.data, binary.size, old.data, old.size));
    OK(fastdb_put(db, empty.data, empty.size, NULL, 0));
    expect_missing(other, binary);
    OK(fastdb_close(other));
    OK(fastdb_flush(db));
    expect_value(db, binary, old, 1);
    expect_value(db, empty, (FastDBSlice){0}, 2);

    STATUS(fastdb_compare_and_swap(db, binary.data, binary.size, 2, current.data, current.size),
           FASTDB_CONFLICT);
    FastDBBatch *batch;
    OK(fastdb_batch_create(options, &batch));
    OK(fastdb_batch_expect(batch, binary.data, binary.size, 1));
    OK(fastdb_batch_expect(batch, "absent", 6, FASTDB_EXPECT_ABSENT));
    OK(fastdb_batch_put(batch, binary.data, binary.size, current.data, current.size));
    OK(fastdb_batch_delete(batch, binary.data, binary.size));
    OK(fastdb_batch_put(batch, "x", 1, "X", 1));
    OK(fastdb_batch_commit(db, batch));
    fastdb_batch_destroy(batch);
    expect_missing(db, binary);
    OK(fastdb_flush(db));
    expect_missing(db, binary);
    expect_value(db, (FastDBSlice){"x", 1}, (FastDBSlice){"X", 1}, 5);
    OK(fastdb_compare_and_swap(db, binary.data, binary.size, FASTDB_EXPECT_ABSENT,
                               current.data, current.size));
    unsigned char large[120] = {9};
    for (size_t i = 0; i < 5; ++i) {
        large[0] = (unsigned char)i;
        OK(fastdb_put(db, rotating.data, rotating.size, large, sizeof(large)));
    }
    OK(fastdb_close(db));

    OK(fastdb_open(options, &db));
    fastdb_options_destroy(options);
    expect_value(db, binary, current, 6);
    expect_value(db, empty, (FastDBSlice){0}, 2);
    expect_value(db, rotating, (FastDBSlice){large, sizeof(large)}, 11);
    FastDBStats stats;
    OK(fastdb_stats(db, &stats));
    CHECK(stats.revision == 11 && stats.record_count == 2 && stats.memory_bytes <= 128 * 1024);
    FastDBIterator *it;
    OK(fastdb_scan_prefix(db, (FastDBSlice){0}, 1, &it));
    FastDBSlice key, value;
    uint64_t revision;
    CHECK(fastdb_iterator_has_more(it));
    OK(fastdb_iterator_next(it, &key, &value, &revision));
    CHECK(key.size == 1 && memcmp(key.data, empty.data, 1) == 0 &&
          value.size == 0 && revision == 2);
    STATUS(fastdb_iterator_next(it, &key, &value, &revision), FASTDB_NOT_FOUND);
    fastdb_iterator_close(it);
    OK(fastdb_flush(db));
    OK(fastdb_stats(db, &stats));
    CHECK(stats.revision == 11 && stats.record_count == 0);
    OK(fastdb_close(db));

    options = disk_options(path, FASTDB_DURABILITY_SYNC);
    OK(fastdb_open(options, &db));
    expect_value(db, binary, current, 6);
    expect_value(db, rotating, (FastDBSlice){large, sizeof(large)}, 11);
    OK(fastdb_delete(db, "missing", 7));
    OK(fastdb_flush(db));
    OK(fastdb_put(db, "post", 4, NULL, 0));
    expect_value(db, (FastDBSlice){"post", 4}, (FastDBSlice){0}, 13);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void check_initialization(void)
{
    char path[] = "/tmp/fastdb-lsm-admit.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    int dir = open(path, O_DIRECTORY | O_RDONLY);
    CHECK(dir >= 0);
    int legacy = openat(dir, "wal_1.log", O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(legacy >= 0);
    CHECK(write(legacy, "SET preserved yes\n", 18) == 18 && close(legacy) == 0);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDB *db;
    STATUS(fastdb_open(options, &db), FASTDB_UNSUPPORTED);
    CHECK(db == NULL && faccessat(dir, ".fastdb.lock", F_OK, 0) == -1);
    CHECK(faccessat(dir, "MANIFEST", F_OK, 0) == -1);
    legacy = openat(dir, "wal_1.log", O_RDONLY);
    CHECK(legacy >= 0);
    char content[18];
    CHECK(read(legacy, content, sizeof(content)) == (ssize_t)sizeof(content));
    CHECK(memcmp(content, "SET preserved yes\n", sizeof(content)) == 0 && close(legacy) == 0);
    CHECK(unlinkat(dir, "wal_1.log", 0) == 0);

    const char *initial_temp = ".manifest-1-abcdef0123456789.tmp";
    int fd = openat(dir, initial_temp, O_CREAT | O_EXCL | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    OK(fastdb_open(options, &db));
    CHECK(faccessat(dir, initial_temp, F_OK, 0) == -1);
    OK(fastdb_put(db, "a", 1, "b", 1));
    OK(fastdb_close(db));
    CHECK(unlinkat(dir, "MANIFEST", 0) == 0);
    STATUS(fastdb_open(options, &db), FASTDB_CORRUPTION);
    CHECK(db == NULL && faccessat(dir, "wal-00000000000000000001", F_OK, 0) == 0);
    CHECK(close(dir) == 0);
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

static void bounded_memtable(void)
{
    char path[] = "/tmp/fastdb-lsm-limit.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options(path, FASTDB_DURABILITY_SYNC);
    FastDB *db;
    OK(fastdb_open(options, &db));
    for (unsigned char key = 0; key < 16; ++key)
        OK(fastdb_put(db, &key, 1, NULL, 0));
    unsigned char rejected = 16;
    STATUS(fastdb_put(db, &rejected, 1, NULL, 0), FASTDB_LIMIT_EXCEEDED);
    expect_missing(db, (FastDBSlice){&rejected, 1});
    OK(fastdb_flush(db));
    OK(fastdb_put(db, &rejected, 1, NULL, 0));
    expect_value(db, (FastDBSlice){&rejected, 1}, (FastDBSlice){0}, 17);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}

int main(void)
{
    reopen_and_flush();
    check_initialization();
    bounded_memtable();
}
