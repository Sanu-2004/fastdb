#include "disk_fixture.h"
#include "block_codec.h"
#include "crc32c.h"
#include "sstable_format.h"

static void read_at(int fd, uint64_t offset, void *bytes, size_t size)
{
    size_t done;
    OK(fdb_fs_read(NULL, fd, offset, bytes, size, &done));
    CHECK(done == size);
}

static void write_at(int fd, uint64_t offset, const void *bytes, size_t size)
{
    size_t done;
    OK(fdb_fs_write(NULL, fd, offset, bytes, size, &done));
    CHECK(done == size);
}

static void codec_table(const FDBDirectory *dir, FastDBCompression codec,
                        uint32_t magic, bool enabled)
{
    const FDBSSTLimits limits = {.max_key_bytes = 16, .max_value_bytes = 512,
        .target_block_bytes = 80, .max_block_bytes = 640, .max_index_bytes = 512,
        .partitioned = true};
    FDBSSTLimits write = limits;
    if (enabled)
        write.codec = (uint8_t)codec;
    FDBSSTWriter *writer;
    OK(fdb_sst_writer_open(dir, "codec", 55, &write, &writer));
    unsigned char repeated[512], random[512];
    memset(repeated, 'R', sizeof(repeated));
    uint32_t seed = 17;
    for (size_t i = 0; i < sizeof(random); ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        random[i] = (unsigned char)(seed >> 24);
    }
    FDBRecord a = {{"a", 1}, {repeated, sizeof(repeated)}, 1, 0, FDB_PUT};
    FDBRecord b = {{"b", 1}, {random, sizeof(random)}, 2, 0, FDB_PUT};
    OK(fdb_sst_writer_add(writer, &a));
    OK(fdb_sst_writer_add(writer, &b));
    FDBSSTProperties props;
    OK(fdb_sst_writer_finish(writer, &props));
    OK(fdb_sst_writer_destroy(writer));

    int fd;
    OK(fdb_dir_open_file(dir, "codec", FDB_FILE_READ_WRITE, &fd));
    unsigned char footer[SST_FOOTER_BYTES];
    read_at(fd, props.file_bytes - sizeof(footer), footer, sizeof(footer));
    uint64_t properties = fdb_decode_u64(footer + 24);
    size_t size = (size_t)fdb_decode_u64(footer + 32);
    unsigned char image[128];
    CHECK(size <= sizeof(image));
    read_at(fd, properties, image, size);
    FDBSSTReader *reader;
    if (!enabled) {
        fdb_encode_u32(image + 40, (uint32_t)codec);
        fdb_encode_u32(image + size - SST_CRC_BYTES, fdb_crc32c(image, size - SST_CRC_BYTES));
        write_at(fd, properties, image, size);
        STATUS(fdb_sst_reader_open(dir, "codec", 55, &limits, &reader), FASTDB_UNSUPPORTED);
        OK(fdb_fs_close(fd));
        OK(fdb_dir_unlink(dir, "codec"));
        return;
    }
    CHECK(fdb_decode_u32(image + 40) == (uint32_t)codec);
    unsigned char header[SST_BLOCK_HEADER_BYTES];
    read_at(fd, SST_HEADER_BYTES, header, sizeof(header));
    CHECK(fdb_decode_u32(header) == magic);
    uint64_t second_offset = SST_HEADER_BYTES + sizeof(header) + fdb_decode_u32(header + 8);
    unsigned char second_magic[4];
    read_at(fd, second_offset, second_magic, sizeof(second_magic));
    CHECK(fdb_decode_u32(second_magic) == SST_BLOCK_MAGIC);
    OK(fdb_sst_reader_open(dir, "codec", 55, &limits, &reader));
    FDBBlockCache *cache;
    OK(fdb_block_cache_create(4096, &cache));
    FastDBBlockCacheStats empty;
    fdb_block_cache_stats(cache, &empty);
    fdb_sst_reader_set_cache(reader, cache);
    OK(fdb_sst_reader_verify(reader));
    FDBSSTIterator *it;
    OK(fdb_sst_iter_open(reader, &it));
    fdb_sst_iter_seek_version(it, a.key, 1);
    const FDBRecord *record = fdb_sst_iter_current(it);
    CHECK(record != NULL && record->sequence == 1 && record->value.size == sizeof(repeated) &&
          memcmp(record->value.data, repeated, sizeof(repeated)) == 0);
    fdb_sst_iter_next(it);
    record = fdb_sst_iter_current(it);
    CHECK(record != NULL && record->sequence == 2 && record->value.size == sizeof(random) &&
          memcmp(record->value.data, random, sizeof(random)) == 0);
    fdb_sst_iter_close(it);
    FastDBBlockCacheStats stats;
    fdb_block_cache_stats(cache, &stats);
    CHECK(stats.hits == 2 && stats.misses == 2);
    CHECK(stats.memory_bytes - empty.memory_bytes >= sizeof(repeated) + sizeof(random));
    OK(fdb_sst_reader_close(reader));
    fdb_block_cache_destroy(cache);

    header[12] ^= 1;
    write_at(fd, SST_HEADER_BYTES, header, sizeof(header));
    OK(fdb_sst_reader_open(dir, "codec", 55, &limits, &reader));
    STATUS(fdb_sst_reader_verify(reader), FASTDB_CORRUPTION);
    OK(fdb_sst_reader_close(reader));
    header[12] ^= 1;
    size_t payload_size = fdb_decode_u32(header + 8);
    unsigned char encoded[640], malformed[640] = {0};
    CHECK(payload_size > 0 && payload_size <= sizeof(encoded));
    read_at(fd, SST_HEADER_BYTES + sizeof(header), encoded, payload_size);
    fdb_encode_u32(header + 12, fdb_crc32c(malformed, payload_size));
    write_at(fd, SST_HEADER_BYTES + sizeof(header), malformed, payload_size);
    write_at(fd, SST_HEADER_BYTES, header, sizeof(header));
    OK(fdb_sst_reader_open(dir, "codec", 55, &limits, &reader));
    STATUS(fdb_sst_reader_verify(reader), FASTDB_CORRUPTION);
    OK(fdb_sst_reader_close(reader));
    write_at(fd, SST_HEADER_BYTES + sizeof(header), encoded, payload_size);
    fdb_encode_u32(header + 12, fdb_crc32c(encoded, payload_size));

    uint32_t raw_size = fdb_decode_u32(header + 4);
    fdb_encode_u32(header + 4, (uint32_t)limits.max_block_bytes + 1);
    write_at(fd, SST_HEADER_BYTES, header, sizeof(header));
    OK(fdb_sst_reader_open(dir, "codec", 55, &limits, &reader));
    STATUS(fdb_sst_reader_verify(reader), FASTDB_LIMIT_EXCEEDED);
    OK(fdb_sst_reader_close(reader));

    fdb_encode_u32(header + 4, raw_size);
    fdb_encode_u32(header + 8, UINT32_MAX);
    write_at(fd, SST_HEADER_BYTES, header, sizeof(header));
    OK(fdb_sst_reader_open(dir, "codec", 55, &limits, &reader));
    STATUS(fdb_sst_reader_verify(reader), FASTDB_CORRUPTION);
    OK(fdb_sst_reader_close(reader));
    fdb_encode_u32(image + 40, UINT32_MAX);
    fdb_encode_u32(image + size - SST_CRC_BYTES, fdb_crc32c(image, size - SST_CRC_BYTES));
    write_at(fd, properties, image, size);
    STATUS(fdb_sst_reader_open(dir, "codec", 55, &limits, &reader), FASTDB_UNSUPPORTED);
    OK(fdb_fs_close(fd));
    OK(fdb_dir_unlink(dir, "codec"));
}

#ifdef FASTDB_HAVE_ZSTD
static FastDBStatus decode_zstd_fixture(const unsigned char *frame, size_t length)
{
    unsigned char encoded[64];
    unsigned char output[512];
    CHECK(length <= sizeof(encoded) - SST_BLOCK_HEADER_BYTES);
    fdb_encode_u32(encoded, SST_ZSTD_BLOCK_MAGIC);
    fdb_encode_u32(encoded + 4, sizeof(output));
    fdb_encode_u32(encoded + 8, (uint32_t)length);
    fdb_encode_u32(encoded + 12, fdb_crc32c(frame, length));
    memcpy(encoded + SST_BLOCK_HEADER_BYTES, frame, length);
    size_t scratch, state_bytes, decoded;
    CHECK(fdb_block_decode_workspace(FASTDB_COMPRESSION_ZSTD, sizeof(output), &scratch,
                                     &state_bytes));
    CHECK(scratch == 2 * sizeof(output) && state_bytes != 0);
    void *state = malloc(state_bytes);
    CHECK(state != NULL);
    FastDBStatus status = fdb_block_decode(FASTDB_COMPRESSION_ZSTD, encoded,
                                           SST_BLOCK_HEADER_BYTES + length, output, sizeof(output),
                                           state, state_bytes, &decoded);
    if (status == FASTDB_OK) {
        CHECK(decoded == sizeof(output));
        for (size_t i = 0; i < decoded; ++i)
            CHECK(output[i] == 'R');
    }
    free(state);
    return status;
}

static void zstd_frame_shape(void)
{
    /* Single-segment standard frame with one RLE block and a known size. */
    const unsigned char frame[] = {0x28, 0xb5, 0x2f, 0xfd, 0x60, 0, 1, 3, 16, 0, 'R'};
    OK(decode_zstd_fixture(frame, sizeof(frame)));
    /* Same content size and block with a 128 KiB window descriptor. */
    const unsigned char oversized_window[] = {0x28, 0xb5, 0x2f, 0xfd, 0x40, 0x38, 0, 1,
                                               3, 16, 0, 'R'};
    STATUS(decode_zstd_fixture(oversized_window, sizeof(oversized_window)), FASTDB_CORRUPTION);
    const unsigned char skippable[] = {0x50, 0x2a, 0x4d, 0x18, 0, 0, 0, 0};
    STATUS(decode_zstd_fixture(skippable, sizeof(skippable)), FASTDB_CORRUPTION);
    unsigned char appended[sizeof(frame) + sizeof(skippable)];
    memcpy(appended, frame, sizeof(frame));
    memcpy(appended + sizeof(frame), skippable, sizeof(skippable));
    STATUS(decode_zstd_fixture(appended, sizeof(appended)), FASTDB_CORRUPTION);
}

static void zstd_checkpoint_workspace(void)
{
    char path[] = "/tmp/fastdb-zstd-checkpoint.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FastDBOptions *options = disk_options_with_mode(path, FASTDB_MODE_MEMORY_PERSISTENT,
                                                     FASTDB_DURABILITY_SYNC);
    FastDBLimits limits;
    fastdb_limits_default(&limits);
    limits.max_key_bytes = 32;
    limits.max_value_bytes = 128;
    limits.max_memory_bytes = 2 * 1024 * 1024;
    OK(fastdb_options_set_limits(options, &limits));
    OK(fastdb_options_set_sstable_compression(options, FASTDB_COMPRESSION_ZSTD));
    FastDB *db;
    unsigned char payload[128] = {0};
    OK(fastdb_open(options, &db));
    OK(fastdb_put(db, "k", 1, payload, sizeof(payload)));
    OK(fastdb_checkpoint(db));
    OK(fastdb_close(db));

    OK(fastdb_options_set_sstable_compression(options, FASTDB_COMPRESSION_NONE));
    limits.max_memory_bytes = 48 * 1024;
    OK(fastdb_options_set_limits(options, &limits));
    STATUS(fastdb_open(options, &db), FASTDB_LIMIT_EXCEEDED);
    CHECK(db == NULL);
    limits.max_memory_bytes = 2 * 1024 * 1024;
    OK(fastdb_options_set_limits(options, &limits));
    OK(fastdb_open(options, &db));
    expect_value(db, (FastDBSlice){"k", 1}, (FastDBSlice){payload, sizeof(payload)}, 1);
    OK(fastdb_close(db));
    fastdb_options_destroy(options);
    remove_disk_fixture(path);
}
#endif

int main(void)
{
    char path[] = "/tmp/fastdb-codec.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FDBDirectory dir;
    OK(fdb_dir_lock(path, NULL, &dir));
    bool lz4 = false;
    bool zstd = false;
#ifdef FASTDB_HAVE_LZ4
    lz4 = true;
#endif
#ifdef FASTDB_HAVE_ZSTD
    zstd = true;
    zstd_frame_shape();
    zstd_checkpoint_workspace();
#endif
    codec_table(&dir, FASTDB_COMPRESSION_LZ4, SST_LZ4_BLOCK_MAGIC, lz4);
    codec_table(&dir, FASTDB_COMPRESSION_ZSTD, SST_ZSTD_BLOCK_MAGIC, zstd);
    OK(fdb_dir_close(&dir));
    remove_disk_fixture(path);
}
