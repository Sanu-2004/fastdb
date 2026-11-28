#include "check.h"
#include "bloom.h"
#include "crc32c.h"
#include "sstable_format.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

static const FDBSSTLimits limits = {16, 64, 82, 128, 512, 0, 0, false};

static FDBFsFault inject(void *context, FDBFsOp operation, size_t requested)
{
    (void)requested;
    bool *fail_read = context;
    if (operation == FDB_FS_READ && *fail_read) {
        *fail_read = false;
        return (FDBFsFault){.error = EIO};
    }
    return (FDBFsFault){0};
}

static void write_table(const FDBDirectory *dir, const char *name, const FDBSSTLimits *options)
{
    const FDBRecord rows[] = {
        {{"a", 1}, {0}, 7, 0, FDB_PUT},
        {{"a", 1}, {0}, 5, 0, FDB_DELETE},
        {{"a", 1}, {0}, 3, 0, FDB_PUT},
        {{"b", 1}, {"x\0", 2}, 2, 65, FDB_PUT},
        {{"b", 1}, {0}, 1, 0, FDB_PUT},
        {{"\xff", 1}, {0}, 4, 0, FDB_DELETE},
    };
    FDBSSTWriter *writer;
    OK(fdb_sst_writer_open(dir, name, 42, options, &writer));
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i)
        OK(fdb_sst_writer_add(writer, &rows[i]));
    FDBSSTProperties properties;
    OK(fdb_sst_writer_finish(writer, &properties));
    OK(fdb_sst_writer_destroy(writer));
}

static void expect(FDBSSTIterator *iter, const char *key, uint64_t sequence, FDBRecordKind kind)
{
    OK(fdb_sst_iter_status(iter));
    const FDBRecord *record = fdb_sst_iter_current(iter);
    CHECK(record != NULL && record->sequence == sequence && record->kind == kind);
    CHECK(record->key.size == 1 && memcmp(record->key.data, key, 1) == 0);
}

static void seek_and_scan(const FDBDirectory *dir, bool *fail_read)
{
    write_table(dir, "ordered", &limits);
    FDBSSTReader *reader;
    STATUS(fdb_sst_reader_open(dir, "missing", 42, &limits, &reader), FASTDB_CORRUPTION);
    STATUS(fdb_sst_reader_open(dir, "ordered", 43, &limits, &reader), FASTDB_CORRUPTION);
    OK(fdb_sst_reader_open(dir, "ordered", 42, &limits, &reader));
    OK(fdb_sst_reader_verify(reader));
    const FDBSSTProperties *properties = fdb_sst_reader_properties(reader);
    CHECK(properties->records == 6 && properties->tombstones == 2);
    CHECK(properties->min_sequence == 1 && properties->max_sequence == 7);

    FDBSSTIterator *it, *other;
    OK(fdb_sst_iter_open(reader, &it));
    OK(fdb_sst_iter_open(reader, &other));
    fdb_sst_iter_first(it);
    expect(it, "a", 7, FDB_PUT);
    fdb_sst_iter_next(it);
    expect(it, "a", 5, FDB_DELETE);
    fdb_sst_iter_next(it);
    expect(it, "a", 3, FDB_PUT);
    fdb_sst_iter_next(it);
    expect(it, "b", 2, FDB_PUT);
    const FDBRecord *b = fdb_sst_iter_current(it);
    CHECK(b->expires_at_ms == 65 && b->value.size == 2 && memcmp(b->value.data, "x\0", 2) == 0);
    fdb_sst_iter_seek(other, (FastDBSlice){"c", 1});
    expect(other, "\xff", 4, FDB_DELETE);
    CHECK(b->key.size == 1 && *(const char *)b->key.data == 'b');
    fdb_sst_iter_next(it);
    expect(it, "b", 1, FDB_PUT);
    fdb_sst_iter_next(it);
    expect(it, "\xff", 4, FDB_DELETE);
    fdb_sst_iter_next(it);
    CHECK(fdb_sst_iter_current(it) == NULL);
    OK(fdb_sst_iter_status(it));

    FastDBSlice a = {"a", 1};
    fdb_sst_iter_seek_version(it, a, 6);
    expect(it, "a", 5, FDB_DELETE);
    fdb_sst_iter_seek_version(it, a, 4);
    expect(it, "a", 3, FDB_PUT);
    fdb_sst_iter_seek_version(it, a, 2);
    CHECK(fdb_sst_iter_current(it) == NULL);
    OK(fdb_sst_iter_status(it));
    fdb_sst_iter_seek(it, (FastDBSlice){"\xff\xff", 2});
    CHECK(fdb_sst_iter_current(it) == NULL);
    OK(fdb_sst_iter_status(it));

    *fail_read = true;
    fdb_sst_iter_seek(it, a);
    CHECK(fdb_sst_iter_current(it) == NULL);
    STATUS(fdb_sst_iter_status(it), FASTDB_IO_ERROR);
    fdb_sst_iter_close(other);
    fdb_sst_iter_close(it);
    OK(fdb_sst_reader_close(reader));
    OK(fdb_dir_unlink(dir, "ordered"));
}

static void damage_block(const FDBDirectory *dir, bool valid_checksum)
{
    write_table(dir, "damaged", &limits);
    int fd;
    OK(fdb_dir_open_file(dir, "damaged", FDB_FILE_READ_WRITE, &fd));
    unsigned char block[82];
    size_t done;
    OK(fdb_fs_read(NULL, fd, SST_HEADER_BYTES, block, sizeof(block), &done));
    CHECK(done == sizeof(block) && fdb_decode_u32(block + 8) == 2);
    size_t record_bytes = fdb_decode_u32(block + 4);
    CHECK(record_bytes == 58);
    fdb_encode_u32(block + SST_BLOCK_HEADER_BYTES + record_bytes, 1);
    if (valid_checksum)
        fdb_encode_u32(block + 12, fdb_crc32c(block + SST_BLOCK_HEADER_BYTES,
                                             sizeof(block) - SST_BLOCK_HEADER_BYTES));
    OK(fdb_fs_write(NULL, fd, SST_HEADER_BYTES, block, sizeof(block), &done));
    CHECK(done == sizeof(block));
    OK(fdb_fs_close(fd));

    FDBSSTReader *reader;
    FDBBlockCache *cache;
    OK(fdb_block_cache_create(1024, &cache));
    OK(fdb_sst_reader_open(dir, "damaged", 42, &limits, &reader));
    fdb_sst_reader_set_cache(reader, cache);
    STATUS(fdb_sst_reader_verify(reader), FASTDB_CORRUPTION);
    FDBSSTIterator *it;
    OK(fdb_sst_iter_open(reader, &it));
    fdb_sst_iter_seek(it, (FastDBSlice){"a", 1});
    CHECK(fdb_sst_iter_current(it) == NULL);
    STATUS(fdb_sst_iter_status(it), FASTDB_CORRUPTION);
    fdb_sst_iter_close(it);
    OK(fdb_sst_reader_close(reader));
    FastDBBlockCacheStats stats;
    fdb_block_cache_stats(cache, &stats);
    CHECK(stats.hits == 0 && stats.misses == 2 && stats.entries == 0);
    fdb_block_cache_destroy(cache);

    OK(fdb_dir_open_file(dir, "damaged", FDB_FILE_READ_WRITE, &fd));
    uint64_t size;
    OK(fdb_fs_size(fd, &size));
    OK(fdb_fs_truncate(NULL, fd, size - 1));
    OK(fdb_fs_close(fd));
    STATUS(fdb_sst_reader_open(dir, "damaged", 42, &limits, &reader), FASTDB_CORRUPTION);
    OK(fdb_dir_unlink(dir, "damaged"));
}

static void hold_cached_iterator(const FDBDirectory *dir)
{
    write_table(dir, "cached", &limits);
    FDBBlockCache *cache;
    OK(fdb_block_cache_create(320, &cache));
    FDBSSTReader *reader;
    OK(fdb_sst_reader_open(dir, "cached", 42, &limits, &reader));
    fdb_sst_reader_set_cache(reader, cache);
    FDBSSTIterator *pin, *other;
    OK(fdb_sst_iter_open(reader, &pin));
    OK(fdb_sst_iter_open(reader, &other));
    fdb_sst_iter_first(pin);
    fdb_sst_iter_first(pin);
    expect(pin, "a", 7, FDB_PUT);
    const FDBRecord *held = fdb_sst_iter_current(pin);
    FastDBSlice key = {"b", 1};
    fdb_sst_iter_seek(other, key);
    expect(other, "b", 2, FDB_PUT);
    CHECK(held->sequence == 7 && memcmp(held->key.data, "a", 1) == 0);
    FastDBBlockCacheStats stats;
    fdb_block_cache_stats(cache, &stats);
    CHECK(stats.hits == 1 && stats.misses == 2 && stats.evictions == 0 && stats.entries == 1);
    fdb_sst_iter_close(pin);
    fdb_sst_iter_seek(other, key);
    expect(other, "b", 2, FDB_PUT);
    fdb_block_cache_stats(cache, &stats);
    CHECK(stats.hits == 1 && stats.misses == 3 && stats.evictions == 1 && stats.entries == 1);
    fdb_sst_iter_close(other);
    OK(fdb_sst_reader_close(reader));
    fdb_block_cache_destroy(cache);
    OK(fdb_dir_unlink(dir, "cached"));
}

static void bloom_no_false_negatives(void)
{
    enum { KEYS = 2048 };
    size_t length;
    CHECK(fdb_bloom_size(KEYS, 10, &length));
    unsigned char *image = malloc(length);
    CHECK(image != NULL);
    fdb_bloom_init(image, length, 10);
    unsigned char key[4];
    for (uint32_t i = 0; i < KEYS; ++i) {
        fdb_encode_u32(key, i * UINT32_C(2654435761));
        fdb_bloom_add(image, (FastDBSlice){key, sizeof(key)});
    }
    fdb_bloom_finish(image, length);
    FDBBloom filter;
    CHECK(fdb_bloom_decode(image, length, &filter));
    for (uint32_t i = 0; i < KEYS; ++i) {
        fdb_encode_u32(key, i * UINT32_C(2654435761));
        CHECK(fdb_bloom_may_contain(&filter, (FastDBSlice){key, sizeof(key)}));
    }
    CHECK(!fdb_bloom_decode(image, length - 1, &filter));
    free(image);
}

static void check_bloom_lookup(const FDBDirectory *dir, const FDBSSTLimits *options)
{
    FDBSSTReader *reader;
    OK(fdb_sst_reader_open(dir, "filtered", 42, options, &reader));
    OK(fdb_sst_reader_verify(reader));
    FDBSSTIterator *it;
    OK(fdb_sst_iter_open(reader, &it));
    fdb_sst_iter_seek_version(it, (FastDBSlice){"a", 1}, 6);
    expect(it, "a", 5, FDB_DELETE);
    fdb_sst_iter_seek_version(it, (FastDBSlice){"b", 1}, 2);
    expect(it, "b", 2, FDB_PUT);
    fdb_sst_iter_seek_version(it, (FastDBSlice){"\xff", 1}, 4);
    expect(it, "\xff", 4, FDB_DELETE);
    for (unsigned char absent = 0; absent < 'a'; ++absent) {
        fdb_sst_iter_seek_version(it, (FastDBSlice){&absent, 1}, 10);
        CHECK(fdb_sst_iter_current(it) == NULL);
        OK(fdb_sst_iter_status(it));
    }
    FDBSSTBloomStats stats;
    fdb_sst_iter_bloom_stats(it, &stats);
    CHECK(stats.checks == 'a' + 3 && stats.negatives > 0 && stats.corrupt_partitions == 0);
    CHECK(stats.checks == stats.negatives + stats.positives);
    CHECK(stats.false_positives + 3 == stats.positives);
    fdb_sst_iter_close(it);
    OK(fdb_sst_reader_close(reader));
}

static void check_damaged_bloom(const FDBDirectory *dir, const FDBSSTLimits *options)
{
    FDBSSTReader *reader;
    FDBSSTIterator *it;
    OK(fdb_sst_reader_open(dir, "filtered", 42, options, &reader));
    OK(fdb_sst_reader_verify(reader));
    CHECK(fdb_sst_reader_corrupt_partitions(reader) == 1);
    OK(fdb_sst_iter_open(reader, &it));
    fdb_sst_iter_seek_version(it, (FastDBSlice){"a", 1}, 6);
    expect(it, "a", 5, FDB_DELETE);
    FDBSSTBloomStats stats;
    fdb_sst_iter_bloom_stats(it, &stats);
    CHECK(stats.corrupt_partitions == 1 && stats.checks == 0);
    fdb_sst_iter_close(it);
    OK(fdb_sst_reader_close(reader));
}

static void streamed_bloom_io(const FDBDirectory *dir, const FDBSSTLimits *options, bool *fail_read)
{
    FDBSSTReader *reader;
    FDBSSTIterator *first, *second;
    OK(fdb_sst_reader_open(dir, "filtered", 42, options, &reader));
    OK(fdb_sst_iter_open(reader, &first));
    OK(fdb_sst_iter_open(reader, &second));
    fdb_sst_iter_seek_version(first, (FastDBSlice){"a", 1}, 7);
    expect(first, "a", 7, FDB_PUT);
    const FDBRecord *held = fdb_sst_iter_current(first);
    *fail_read = true;
    fdb_sst_iter_seek_version(second, (FastDBSlice){"b", 1}, 2);
    STATUS(fdb_sst_iter_status(second), FASTDB_IO_ERROR);
    CHECK(!*fail_read && fdb_sst_iter_current(second) == NULL);
    fdb_sst_iter_seek_version(second, (FastDBSlice){"b", 1}, 2);
    expect(second, "b", 2, FDB_PUT);
    CHECK(held->sequence == 7 && held->key.size == 1 && memcmp(held->key.data, "a", 1) == 0);
    fdb_sst_iter_close(second);
    fdb_sst_iter_close(first);
    OK(fdb_sst_reader_close(reader));
}

typedef struct StreamedSeek {
    FDBSSTReader *reader;
    pthread_barrier_t start;
} StreamedSeek;

static void *seek_damaged_partition(void *arg)
{
    StreamedSeek *seek = arg;
    FDBSSTIterator *iter;
    OK(fdb_sst_iter_open(seek->reader, &iter));
    int arrival = pthread_barrier_wait(&seek->start);
    CHECK(arrival == 0 || arrival == PTHREAD_BARRIER_SERIAL_THREAD);
    for (size_t i = 0; i < 20; ++i) {
        fdb_sst_iter_seek_version(iter, (FastDBSlice){"a", 1}, 6);
        expect(iter, "a", 5, FDB_DELETE);
    }
    fdb_sst_iter_close(iter);
    return NULL;
}

static void concurrent_bloom_fallback(const FDBDirectory *dir, const FDBSSTLimits *options)
{
    StreamedSeek seek;
    OK(fdb_sst_reader_open(dir, "filtered", 42, options, &seek.reader));
    CHECK(fdb_sst_reader_corrupt_partitions(seek.reader) == 0);
    CHECK(pthread_barrier_init(&seek.start, NULL, 2) == 0);
    pthread_t other;
    CHECK(pthread_create(&other, NULL, seek_damaged_partition, &seek) == 0);
    seek_damaged_partition(&seek);
    CHECK(pthread_join(other, NULL) == 0);
    CHECK(pthread_barrier_destroy(&seek.start) == 0);
    CHECK(fdb_sst_reader_corrupt_partitions(seek.reader) == 1);
    OK(fdb_sst_reader_close(seek.reader));
}

static void test_bloom(const FDBDirectory *dir, bool *fail_read)
{
    FDBSSTLimits options = limits;
    options.max_filter_bytes = 128;
    options.bloom_bits_per_key = 10;
    write_table(dir, "filtered", &options);
    options.bloom_bits_per_key = 0;
    check_bloom_lookup(dir, &options);
    FDBSSTLimits streamed = options;
    streamed.max_filter_bytes = FDB_BLOOM_MIN_IMAGE_BYTES;
    check_bloom_lookup(dir, &streamed);
    streamed_bloom_io(dir, &streamed, fail_read);

    FDBSSTLimits too_small = streamed;
    --too_small.max_filter_bytes;
    FDBSSTReader *reader;
    STATUS(fdb_sst_reader_open(dir, "filtered", 42, &too_small, &reader), FASTDB_LIMIT_EXCEEDED);
    CHECK(reader == NULL);

    int fd;
    OK(fdb_dir_open_file(dir, "filtered", FDB_FILE_READ_WRITE, &fd));
    uint64_t size;
    size_t done;
    unsigned char footer[SST_FOOTER_BYTES];
    OK(fdb_fs_size(fd, &size));
    OK(fdb_fs_read(NULL, fd, size - sizeof(footer), footer, sizeof(footer), &done));
    CHECK(done == sizeof(footer) && fdb_decode_u32(footer + 48) == SST_FEATURE_BLOOM);
    uint64_t first_filter = fdb_decode_u64(footer + 8) - 4 * FDB_BLOOM_MIN_IMAGE_BYTES;
    unsigned char byte;
    OK(fdb_fs_read(NULL, fd, first_filter, &byte, 1, &done));
    CHECK(done == 1);
    byte ^= 1;
    OK(fdb_fs_write(NULL, fd, first_filter, &byte, 1, &done));
    CHECK(done == 1);
    OK(fdb_fs_close(fd));

    check_damaged_bloom(dir, &options);
    check_damaged_bloom(dir, &streamed);
    concurrent_bloom_fallback(dir, &streamed);
    OK(fdb_dir_unlink(dir, "filtered"));
}

static void partitioned_index(const FDBDirectory *dir)
{
    FDBSSTLimits write = limits;
    write.max_index_bytes = 256;
    write.max_filter_bytes = 2 * FDB_BLOOM_MIN_IMAGE_BYTES;
    write.bloom_bits_per_key = 10;
    write.partitioned = true;
    const char *name = "partitioned";
    FDBSSTWriter *writer;
    OK(fdb_sst_writer_open(dir, name, 43, &write, &writer));
    const unsigned char payload[48] = {4};
    FDBRecord record = {{"a", 1}, {payload, sizeof(payload)}, 1, 0, FDB_PUT};
    OK(fdb_sst_writer_add(writer, &record));
    record.key = (FastDBSlice){"k", 1};
    for (uint64_t seq = 12; seq >= 5; --seq) {
        record.sequence = seq;
        OK(fdb_sst_writer_add(writer, &record));
    }
    record.key = (FastDBSlice){"z", 1};
    record.sequence = 2;
    OK(fdb_sst_writer_add(writer, &record));
    FDBSSTProperties props;
    OK(fdb_sst_writer_finish(writer, &props));
    CHECK(props.records == 10);
    OK(fdb_sst_writer_destroy(writer));

    FDBSSTLimits read = write;
    read.partitioned = false;
    read.bloom_bits_per_key = 0;
    FDBSSTReader *reader;
    OK(fdb_sst_reader_open(dir, name, 43, &read, &reader));
    OK(fdb_sst_reader_verify(reader));
    FDBSSTIterator *first, *second;
    OK(fdb_sst_iter_open(reader, &first));
    OK(fdb_sst_iter_open(reader, &second));
    const FastDBSlice key = {"k", 1};
    fdb_sst_iter_seek_version(first, key, 8);
    expect(first, "k", 8, FDB_PUT);
    fdb_sst_iter_seek(second, (FastDBSlice){"l", 1});
    expect(second, "z", 2, FDB_PUT);
    fdb_sst_iter_next(first);
    expect(first, "k", 7, FDB_PUT);
    fdb_sst_iter_seek_version(second, key, 4);
    CHECK(fdb_sst_iter_current(second) == NULL);
    OK(fdb_sst_iter_status(second));
    fdb_sst_iter_close(second);
    fdb_sst_iter_close(first);
    OK(fdb_sst_reader_close(reader));

    int fd;
    OK(fdb_dir_open_file(dir, name, FDB_FILE_READ_WRITE, &fd));
    uint64_t length;
    size_t done;
    unsigned char footer[SST_FOOTER_BYTES], root[256], byte;
    OK(fdb_fs_size(fd, &length));
    OK(fdb_fs_read(NULL, fd, length - sizeof(footer), footer, sizeof(footer), &done));
    CHECK(done == sizeof(footer));
    uint64_t root_offset = fdb_decode_u64(footer + 8);
    uint64_t root_size = fdb_decode_u64(footer + 16);
    CHECK(root_size <= sizeof(root));
    OK(fdb_fs_read(NULL, fd, root_offset, root, (size_t)root_size, &done));
    CHECK(done == root_size && fdb_decode_u32(root) >= 3);
    uint64_t leaf_offset = fdb_decode_u64(root + SST_INDEX_HEADER_BYTES);
    uint64_t first_filter = leaf_offset - 2 * FDB_BLOOM_MIN_IMAGE_BYTES;
    OK(fdb_fs_read(NULL, fd, first_filter, &byte, 1, &done));
    CHECK(done == 1);
    byte ^= 1;
    OK(fdb_fs_write(NULL, fd, first_filter, &byte, 1, &done));
    CHECK(done == 1);
    OK(fdb_sst_reader_open(dir, name, 43, &read, &reader));
    OK(fdb_sst_reader_verify(reader));
    CHECK(fdb_sst_reader_corrupt_partitions(reader) == 1);
    OK(fdb_sst_iter_open(reader, &first));
    fdb_sst_iter_seek_version(first, (FastDBSlice){"a", 1}, 1);
    expect(first, "a", 1, FDB_PUT);
    fdb_sst_iter_close(first);
    OK(fdb_sst_reader_close(reader));
    byte ^= 1;
    OK(fdb_fs_write(NULL, fd, first_filter, &byte, 1, &done));
    CHECK(done == 1);
    OK(fdb_fs_read(NULL, fd, leaf_offset, &byte, 1, &done));
    CHECK(done == 1);
    byte ^= 1;
    OK(fdb_fs_write(NULL, fd, leaf_offset, &byte, 1, &done));
    CHECK(done == 1);
    OK(fdb_sst_reader_open(dir, name, 43, &read, &reader));
    STATUS(fdb_sst_reader_verify(reader), FASTDB_CORRUPTION);
    OK(fdb_sst_reader_close(reader));

    byte ^= 1;
    OK(fdb_fs_write(NULL, fd, leaf_offset, &byte, 1, &done));
    CHECK(done == 1);
    fdb_encode_u64(root + SST_INDEX_HEADER_BYTES, 0);
    fdb_encode_u32(root + root_size - SST_CRC_BYTES, fdb_crc32c(root, (size_t)root_size - SST_CRC_BYTES));
    OK(fdb_fs_write(NULL, fd, root_offset, root, (size_t)root_size, &done));
    CHECK(done == root_size);
    OK(fdb_fs_close(fd));
    STATUS(fdb_sst_reader_open(dir, name, 43, &read, &reader), FASTDB_CORRUPTION);
    OK(fdb_dir_unlink(dir, name));
}

static void large_partitioned_table(const FDBDirectory *dir)
{
    enum { ROWS = 2048 };
    const FDBSSTLimits write = {16, 1024, 512, 2048, 8192, 256, 10, true};
    FDBSSTWriter *writer;
    OK(fdb_sst_writer_open(dir, "large", 44, &write, &writer));
    unsigned char value[1024] = {0}, key[2];
    FDBRecord record = {{key, sizeof(key)}, {value, sizeof(value)}, 0, 0, FDB_PUT};
    for (uint32_t i = 0; i < ROWS; ++i) {
        key[0] = (unsigned char)(i >> 8);
        key[1] = (unsigned char)i;
        value[0] = key[1];
        record.sequence = i + UINT64_C(1);
        OK(fdb_sst_writer_add(writer, &record));
    }
    FDBSSTProperties props;
    OK(fdb_sst_writer_finish(writer, &props));
    CHECK(props.records == ROWS && props.file_bytes > 2 * 1024 * 1024);
    OK(fdb_sst_writer_destroy(writer));
    FDBSSTReader *reader;
    OK(fdb_sst_reader_open(dir, "large", 44, &write, &reader));
    size_t workspace;
    CHECK(fdb_sst_reader_workspace(reader, &workspace) && workspace < 64 * 1024);
    OK(fdb_sst_reader_verify(reader));
    FDBSSTIterator *iter;
    OK(fdb_sst_iter_open(reader, &iter));
    fdb_sst_iter_seek_version(iter, (FastDBSlice){key, sizeof(key)}, ROWS);
    const FDBRecord *last = fdb_sst_iter_current(iter);
    CHECK(last != NULL && last->sequence == ROWS && last->value.size == sizeof(value) &&
          memcmp(last->value.data, value, sizeof(value)) == 0);
    fdb_sst_iter_close(iter);
    OK(fdb_sst_reader_close(reader));
    OK(fdb_dir_unlink(dir, "large"));
}

int main(void)
{
    char path[] = "/tmp/fastdb-sst-read.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    bool fail_read = false;
    const FDBFs fs = {inject, &fail_read};
    FDBDirectory dir;
    OK(fdb_dir_lock(path, &fs, &dir));
    seek_and_scan(&dir, &fail_read);
    hold_cached_iterator(&dir);
    damage_block(&dir, false);
    damage_block(&dir, true);
    bloom_no_false_negatives();
    test_bloom(&dir, &fail_read);
    partitioned_index(&dir);
    large_partitioned_table(&dir);
    OK(fdb_dir_close(&dir));
    char lock_path[sizeof(path) + sizeof("/.fastdb.lock")];
    CHECK(snprintf(lock_path, sizeof(lock_path), "%s/.fastdb.lock", path) > 0);
    CHECK(unlink(lock_path) == 0 && rmdir(path) == 0);
}
