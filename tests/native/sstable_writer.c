#include "check.h"
#include "crc32c.h"
#include "sstable_format.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static const FDBSSTLimits limits = {20, 100, 80, 160, 256, 0, 0};

static FDBFsFault inject(void *context, FDBFsOp operation, size_t requested)
{
    (void)requested;
    bool *fail = context;
    if (operation == FDB_FS_SYNC && *fail) {
        *fail = false;
        return (FDBFsFault){.error = EIO};
    }
    return (FDBFsFault){0};
}

static void read_exact(int fd, uint64_t offset, void *bytes, size_t size)
{
    size_t transferred;
    OK(fdb_fs_read(NULL, fd, offset, bytes, size, &transferred));
    CHECK(transferred == size);
}

static void verify_image(const FDBDirectory *dir, const FDBSSTProperties *properties)
{
    int fd;
    OK(fdb_dir_open_file(dir, "table.tmp", FDB_FILE_READ, &fd));
    uint64_t file_size;
    OK(fdb_fs_size(fd, &file_size));
    CHECK(file_size == properties->file_bytes);
    unsigned char header[SST_HEADER_BYTES];
    read_exact(fd, 0, header, sizeof(header));
    CHECK(memcmp(header, SST_MAGIC, 8) == 0 && fdb_decode_u32(header + 8) == 1);
    CHECK(fdb_decode_u64(header + 16) == 10);
    CHECK(fdb_decode_u32(header + 28) == fdb_crc32c(header, 28));

    unsigned char footer[SST_FOOTER_BYTES];
    read_exact(fd, file_size - sizeof(footer), footer, sizeof(footer));
    CHECK(memcmp(footer, SST_FOOTER_MAGIC, 8) == 0 && fdb_decode_u64(footer + 40) == 10);
    CHECK(fdb_decode_u32(footer + 52) == fdb_crc32c(footer, 52));
    uint64_t index_offset = fdb_decode_u64(footer + 8);
    uint64_t index_size = fdb_decode_u64(footer + 16);
    uint64_t properties_offset = fdb_decode_u64(footer + 24);
    uint64_t properties_size = fdb_decode_u64(footer + 32);
    CHECK(index_size == 78 && properties_size == 55);
    CHECK(index_offset + index_size == properties_offset);
    CHECK(properties_offset + properties_size + sizeof(footer) == file_size);
    unsigned char index[78], stats[55];
    read_exact(fd, index_offset, index, sizeof(index));
    read_exact(fd, properties_offset, stats, sizeof(stats));
    CHECK(fdb_decode_u32(index) == 4);
    CHECK(fdb_decode_u32(index + 74) == fdb_crc32c(index, 74));
    CHECK(fdb_decode_u32(stats + 51) == fdb_crc32c(stats, 51));
    CHECK(fdb_decode_u32(stats) == 2 && fdb_decode_u32(stats + 4) == 1);
    CHECK(fdb_decode_u64(stats + 8) == 4 && fdb_decode_u64(stats + 16) == 1);
    CHECK(fdb_decode_u64(stats + 24) == 2 && fdb_decode_u64(stats + 32) == 9);
    CHECK(memcmp(stats + 48, "a\0z", 3) == 0);

    const uint64_t sequences[] = {9, 3, 5, 2};
    const size_t value_lengths[] = {1, 0, 80, 0};
    uint64_t previous_end = SST_HEADER_BYTES;
    const unsigned char *entry = index + 4;
    for (size_t i = 0; i < 4; ++i) {
        uint64_t offset = fdb_decode_u64(entry);
        size_t bytes = fdb_decode_u32(entry + 8);
        size_t key_bytes = fdb_decode_u32(entry + 12);
        const char *key = i < 2 ? "a\0" : "z";
        CHECK(key_bytes == (i < 2 ? 2U : 1U));
        CHECK(offset == previous_end && memcmp(entry + SST_INDEX_ENTRY_BYTES, key, key_bytes) == 0);
        unsigned char block[160];
        CHECK(bytes >= SST_BLOCK_HEADER_BYTES && bytes <= sizeof(block));
        read_exact(fd, offset, block, bytes);
        CHECK(fdb_decode_u32(block) == SST_BLOCK_MAGIC && fdb_decode_u32(block + 8) == 1);
        size_t record_bytes = fdb_decode_u32(block + 4);
        CHECK(record_bytes + SST_BLOCK_HEADER_BYTES + 4 == bytes);
        CHECK(fdb_decode_u32(block + bytes - 4) == 0);
        CHECK(fdb_decode_u32(block + 12) ==
              fdb_crc32c(block + SST_BLOCK_HEADER_BYTES, record_bytes + 4));
        FDBRecord record;
        size_t used;
        OK(fdb_record_decode(block + SST_BLOCK_HEADER_BYTES, record_bytes, 20, 100, &record, &used));
        CHECK(used == record_bytes && record.value.size == value_lengths[i]);
        CHECK(record.sequence == sequences[i] && memcmp(record.key.data, key, key_bytes) == 0);
        CHECK(record.kind == (i == 1 ? FDB_DELETE : FDB_PUT));
        previous_end = offset + bytes;
        entry += SST_INDEX_ENTRY_BYTES + key_bytes;
    }
    CHECK(previous_end == index_offset);
    OK(fdb_fs_close(fd));
}

static void writer_image(const FDBDirectory *dir)
{
    FDBSSTWriter *writer;
    OK(fdb_sst_writer_open(dir, "table.tmp", 10, &limits, &writer));
    FDBSSTProperties properties;
    STATUS(fdb_sst_writer_finish(writer, &properties), FASTDB_INVALID_ARGUMENT);
    const unsigned char value[80] = {0xfe};
    const FDBRecord records[] = {
        {{"a\0", 2}, {value, 1}, 9, 0, FDB_PUT},
        {{"a\0", 2}, {0}, 3, 0, FDB_DELETE},
        {{"z", 1}, {value, sizeof(value)}, 5, 90, FDB_PUT},
        {{"z", 1}, {0}, 2, 0, FDB_PUT},
    };
    OK(fdb_sst_writer_add(writer, &records[0]));
    STATUS(fdb_sst_writer_add(writer, &records[0]), FASTDB_INVALID_ARGUMENT);
    for (size_t i = 1; i < sizeof(records) / sizeof(records[0]); ++i)
        OK(fdb_sst_writer_add(writer, &records[i]));
    STATUS(fdb_sst_writer_add(writer, &records[1]), FASTDB_INVALID_ARGUMENT);
    OK(fdb_sst_writer_finish(writer, &properties));
    CHECK(properties.records == 4 && properties.tombstones == 1 && properties.id == 10);
    CHECK(properties.min_sequence == 2 && properties.max_sequence == 9);
    verify_image(dir, &properties);
    STATUS(fdb_sst_writer_add(writer, &records[0]), FASTDB_CLOSED);
    OK(fdb_sst_writer_destroy(writer));
    OK(fdb_dir_unlink(dir, "table.tmp"));
}

static void limits_and_failure(const FDBDirectory *dir, bool *fail_sync)
{
    FDBSSTLimits small = limits;
    small.max_index_bytes = 26;
    FDBSSTWriter *writer;
    OK(fdb_sst_writer_open(dir, "small.tmp", 11, &small, &writer));
    const FDBRecord a = {{"ak", 2}, {"v", 1}, 1, 0, FDB_PUT};
    const FDBRecord b = {{"bk", 2}, {"w", 1}, 2, 0, FDB_PUT};
    OK(fdb_sst_writer_add(writer, &a));
    STATUS(fdb_sst_writer_add(writer, &b), FASTDB_LIMIT_EXCEEDED);
    FDBSSTProperties properties;
    OK(fdb_sst_writer_finish(writer, &properties));
    CHECK(properties.records == 1);
    OK(fdb_sst_writer_destroy(writer));
    OK(fdb_dir_unlink(dir, "small.tmp"));

    OK(fdb_sst_writer_open(dir, "interrupted.tmp", 12, &limits, &writer));
    OK(fdb_sst_writer_add(writer, &a));
    *fail_sync = true;
    STATUS(fdb_sst_writer_finish(writer, &properties), FASTDB_IO_ERROR);
    CHECK(properties.file_bytes == 0);
    STATUS(fdb_sst_writer_destroy(writer), FASTDB_IO_ERROR);
    OK(fdb_dir_unlink(dir, "interrupted.tmp"));
}

int main(void)
{
    char path[] = "/tmp/fastdb-sst.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    bool fail_sync = false;
    const FDBFs fs = {inject, &fail_sync};
    FDBDirectory dir;
    OK(fdb_dir_lock(path, &fs, &dir));
    writer_image(&dir);
    limits_and_failure(&dir, &fail_sync);
    OK(fdb_dir_close(&dir));
    char lock_path[sizeof(path) + sizeof("/.fastdb.lock")];
    CHECK(snprintf(lock_path, sizeof(lock_path), "%s/.fastdb.lock", path) > 0);
    CHECK(unlink(lock_path) == 0 && rmdir(path) == 0);
}
