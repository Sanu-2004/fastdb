#include "disk_fixture.h"
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

static void codec_table(const FDBDirectory *dir)
{
    const FDBSSTLimits limits = {.max_key_bytes = 16, .max_value_bytes = 512,
        .target_block_bytes = 80, .max_block_bytes = 640, .max_index_bytes = 512,
        .partitioned = true};
    FDBSSTLimits write = limits;
#ifdef FASTDB_HAVE_LZ4
    write.codec = FASTDB_COMPRESSION_LZ4;
#endif
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
#ifdef FASTDB_HAVE_LZ4
    CHECK(fdb_decode_u32(image + 40) == FASTDB_COMPRESSION_LZ4);
    unsigned char header[SST_BLOCK_HEADER_BYTES];
    read_at(fd, SST_HEADER_BYTES, header, sizeof(header));
    CHECK(fdb_decode_u32(header) == SST_LZ4_BLOCK_MAGIC);
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
#else
    fdb_encode_u32(image + 40, FASTDB_COMPRESSION_LZ4);
    fdb_encode_u32(image + size - SST_CRC_BYTES, fdb_crc32c(image, size - SST_CRC_BYTES));
    write_at(fd, properties, image, size);
    STATUS(fdb_sst_reader_open(dir, "codec", 55, &limits, &reader), FASTDB_UNSUPPORTED);
#endif
    OK(fdb_fs_close(fd));
    OK(fdb_dir_unlink(dir, "codec"));
}

int main(void)
{
    char path[] = "/tmp/fastdb-codec.XXXXXXXX";
    CHECK(mkdtemp(path) != NULL);
    FDBDirectory dir;
    OK(fdb_dir_lock(path, NULL, &dir));
    codec_table(&dir);
    OK(fdb_dir_close(&dir));
    remove_disk_fixture(path);
}
