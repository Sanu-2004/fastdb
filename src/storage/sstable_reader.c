#include "sstable_format.h"
#include "bloom.h"
#include "crc32c.h"

#include <stdlib.h>
#include <string.h>

typedef struct IndexEntry {
    uint64_t offset;
    size_t bytes;
    FastDBSlice last_key;
    size_t filter_bytes;
    FDBBloom bloom;
} IndexEntry;

struct FDBSSTReader {
    int fd;
    FDBFs fs;
    FDBSSTLimits limits;
    FDBSSTProperties properties;
    FDBBlockCache *cache;
    unsigned char *properties_image;
    unsigned char *index_image;
    unsigned char *filter_image;
    IndexEntry *index;
    size_t block_count;
    size_t corrupt_partitions;
    uint32_t features;
};

struct FDBSSTIterator {
    const FDBSSTReader *reader;
    FDBCachedBlock *pin;
    const unsigned char *block;
    const unsigned char *offsets;
    size_t block_number;
    size_t row_count;
    size_t row;
    size_t record_bytes;
    FDBRecord record;
    FastDBStatus status;
    bool valid;
    FDBSSTBloomStats bloom_stats;
    unsigned char scratch[];
};

typedef struct Regions {
    uint64_t index_offset;
    size_t index_size;
    uint64_t properties_offset;
    size_t properties_size;
} Regions;

bool fdb_sst_reader_budget(const FDBSSTLimits *limits, size_t *bytes)
{
    size_t keys = limits->max_key_bytes;
    size_t blocks = limits->max_index_bytes / (SST_INDEX_ENTRY_BYTES + 1);
    *bytes = sizeof(FDBSSTReader) + sizeof(FDBSSTIterator) +
             SST_PROPERTIES_HEADER_BYTES + SST_CRC_BYTES;
    return blocks <= SIZE_MAX / sizeof(IndexEntry) &&
           fdb_add_size(*bytes, blocks * sizeof(IndexEntry), bytes) &&
           fdb_add_size(*bytes, limits->max_block_bytes, bytes) &&
           fdb_add_size(*bytes, limits->max_index_bytes, bytes) &&
           fdb_add_size(*bytes, limits->max_filter_bytes, bytes) &&
           keys <= (SIZE_MAX - *bytes) / 2 &&
           fdb_add_size(*bytes, 2 * keys, bytes);
}

bool fdb_sst_reader_workspace(const FDBSSTReader *reader, size_t *bytes)
{
    return fdb_sst_reader_budget(&reader->limits, bytes);
}

static FastDBStatus read_exact(const FDBSSTReader *reader, uint64_t offset, void *data, size_t size)
{
    size_t done;
    FastDBStatus status = fdb_fs_read(&reader->fs, reader->fd, offset, data, size, &done);
    return status == FASTDB_OK && done != size ? FASTDB_CORRUPTION : status;
}

static FastDBStatus decode_regions(FDBSSTReader *reader, Regions *regions)
{
    unsigned char header[SST_HEADER_BYTES];
    FastDBStatus status = read_exact(reader, 0, header, sizeof(header));
    if (status != FASTDB_OK)
        return status;
    if (memcmp(header, SST_MAGIC, 8) != 0 || fdb_decode_u32(header + 28) != fdb_crc32c(header, 28))
        return FASTDB_CORRUPTION;
    if (fdb_decode_u32(header + 8) != SST_VERSION)
        return FASTDB_UNSUPPORTED;
    reader->features = fdb_decode_u32(header + 12);
    if ((reader->features & ~(uint32_t)SST_FEATURE_BLOOM) != 0)
        return FASTDB_UNSUPPORTED;
    if (fdb_decode_u32(header + 24) != 0 || fdb_decode_u64(header + 16) != reader->properties.id)
        return FASTDB_CORRUPTION;

    if (reader->properties.file_bytes < SST_HEADER_BYTES + SST_FOOTER_BYTES)
        return FASTDB_CORRUPTION;
    uint64_t footer_offset = reader->properties.file_bytes - SST_FOOTER_BYTES;
    unsigned char footer[SST_FOOTER_BYTES];
    status = read_exact(reader, footer_offset, footer, sizeof(footer));
    if (status != FASTDB_OK)
        return status;
    if (memcmp(footer, SST_FOOTER_MAGIC, 8) != 0 ||
        fdb_decode_u32(footer + 52) != fdb_crc32c(footer, 52) ||
        fdb_decode_u64(footer + 40) != reader->properties.id ||
        fdb_decode_u32(footer + 48) != reader->features)
        return FASTDB_CORRUPTION;

    uint64_t index_offset = fdb_decode_u64(footer + 8);
    uint64_t index_size = fdb_decode_u64(footer + 16);
    uint64_t properties_offset = fdb_decode_u64(footer + 24);
    uint64_t properties_size = fdb_decode_u64(footer + 32);
    if (properties_offset > footer_offset || properties_size != footer_offset - properties_offset ||
        index_offset > properties_offset || index_size != properties_offset - index_offset ||
        index_offset < SST_HEADER_BYTES || index_size < SST_INDEX_HEADER_BYTES + SST_CRC_BYTES ||
        properties_size < SST_PROPERTIES_HEADER_BYTES + SST_CRC_BYTES)
        return FASTDB_CORRUPTION;
    if (index_size > reader->limits.max_index_bytes ||
        properties_size > SST_PROPERTIES_HEADER_BYTES + 2 * (uint64_t)reader->limits.max_key_bytes +
                          SST_CRC_BYTES ||
        properties_size > SIZE_MAX)
        return FASTDB_LIMIT_EXCEEDED;
    *regions = (Regions){index_offset, (size_t)index_size, properties_offset,
                         (size_t)properties_size};
    return FASTDB_OK;
}

static FastDBStatus load_properties(FDBSSTReader *reader, const Regions *regions)
{
    size_t size = regions->properties_size;
    unsigned char *bytes = malloc(size);
    if (bytes == NULL)
        return FASTDB_OUT_OF_MEMORY;
    reader->properties_image = bytes;
    FastDBStatus status = read_exact(reader, regions->properties_offset, bytes, size);
    if (status != FASTDB_OK)
        return status;
    if (fdb_decode_u32(bytes + size - SST_CRC_BYTES) != fdb_crc32c(bytes, size - SST_CRC_BYTES))
        return FASTDB_CORRUPTION;
    if (fdb_decode_u32(bytes + 40) != 0)
        return FASTDB_UNSUPPORTED;
    if (fdb_decode_u32(bytes + 44) != 0)
        return FASTDB_CORRUPTION;

    size_t min_size = fdb_decode_u32(bytes);
    size_t max_size = fdb_decode_u32(bytes + 4);
    size_t key_bytes = size - SST_PROPERTIES_HEADER_BYTES - SST_CRC_BYTES;
    if (min_size > reader->limits.max_key_bytes ||
        max_size > reader->limits.max_key_bytes ||
        min_size > key_bytes || max_size != key_bytes - min_size)
        return FASTDB_CORRUPTION;

    FDBSSTProperties *properties = &reader->properties;
    properties->records = fdb_decode_u64(bytes + 8);
    properties->tombstones = fdb_decode_u64(bytes + 16);
    properties->min_sequence = fdb_decode_u64(bytes + 24);
    properties->max_sequence = fdb_decode_u64(bytes + 32);
    properties->min_key = (FastDBSlice){bytes + SST_PROPERTIES_HEADER_BYTES, min_size};
    properties->max_key = (FastDBSlice){bytes + SST_PROPERTIES_HEADER_BYTES + min_size, max_size};
    if (properties->records == 0)
        return min_size == 0 && max_size == 0 && properties->tombstones == 0 &&
               properties->min_sequence == 0 && properties->max_sequence == 0 ?
               FASTDB_OK : FASTDB_CORRUPTION;
    if (min_size == 0 || max_size == 0 || properties->tombstones > properties->records ||
        properties->min_sequence == 0 || properties->min_sequence > properties->max_sequence ||
        fdb_compare(properties->min_key, properties->max_key) > 0)
        return FASTDB_CORRUPTION;
    return FASTDB_OK;
}

static FastDBStatus load_filters(FDBSSTReader *reader, uint64_t offset, size_t size)
{
    if (size > reader->limits.max_filter_bytes)
        return FASTDB_LIMIT_EXCEEDED;
    if (size == 0) {
        reader->corrupt_partitions = reader->block_count;
        return FASTDB_OK;
    }
    unsigned char *image = malloc(size);
    if (image == NULL)
        return FASTDB_OUT_OF_MEMORY;
    reader->filter_image = image;
    FastDBStatus status = read_exact(reader, offset, image, size);
    if (status != FASTDB_OK)
        return status;
    const unsigned char *partition = image;
    for (size_t i = 0; i < reader->block_count; ++i) {
        IndexEntry *entry = &reader->index[i];
        if (!fdb_bloom_decode(partition, entry->filter_bytes, &entry->bloom) ||
            !fdb_bloom_may_contain(&entry->bloom, entry->last_key)) {
            entry->bloom = (FDBBloom){0};
            ++reader->corrupt_partitions;
        }
        partition += entry->filter_bytes;
    }
    return FASTDB_OK;
}

static FastDBStatus parse_index(FDBSSTReader *reader, size_t size, uint64_t index_offset)
{
    const unsigned char *bytes = reader->index_image;
    if (fdb_decode_u32(bytes + size - SST_CRC_BYTES) != fdb_crc32c(bytes, size - SST_CRC_BYTES))
        return FASTDB_CORRUPTION;
    size_t count = fdb_decode_u32(bytes);
    size_t left = size - SST_INDEX_HEADER_BYTES - SST_CRC_BYTES;
    bool has_filters = (reader->features & SST_FEATURE_BLOOM) != 0;
    size_t overhead = SST_INDEX_ENTRY_BYTES + (has_filters ? sizeof(uint32_t) : 0);
    if (reader->properties.records == 0)
        return count == 0 && left == 0 && index_offset == SST_HEADER_BYTES ?
               FASTDB_OK : FASTDB_CORRUPTION;
    if (count == 0 || count > reader->properties.records ||
        count > left / (overhead + 1) ||
        count > SIZE_MAX / sizeof(*reader->index))
        return FASTDB_CORRUPTION;
    IndexEntry *index = malloc(count * sizeof(*index));
    if (index == NULL)
        return FASTDB_OUT_OF_MEMORY;
    reader->index = index;
    reader->block_count = count;

    uint64_t next_offset = SST_HEADER_BYTES;
    size_t filter_bytes = 0;
    FastDBSlice previous_key = reader->properties.min_key;
    bytes += SST_INDEX_HEADER_BYTES;
    for (size_t i = 0; i < count; ++i) {
        if (left < overhead)
            return FASTDB_CORRUPTION;
        uint64_t offset = fdb_decode_u64(bytes);
        size_t block_bytes = fdb_decode_u32(bytes + 8);
        size_t key_size = fdb_decode_u32(bytes + 12);
        if (offset != next_offset || block_bytes < SST_BLOCK_HEADER_BYTES + SST_MIN_ROW_BYTES ||
            block_bytes > index_offset - offset || key_size == 0 ||
            key_size > left - overhead || key_size > reader->limits.max_key_bytes)
            return FASTDB_CORRUPTION;
        if (block_bytes > reader->limits.max_block_bytes)
            return FASTDB_LIMIT_EXCEEDED;
        FastDBSlice last_key = {bytes + SST_INDEX_ENTRY_BYTES, key_size};
        if (fdb_compare(previous_key, last_key) > 0)
            return FASTDB_CORRUPTION;
        index[i] = (IndexEntry){.offset = offset, .bytes = block_bytes, .last_key = last_key};
        if (has_filters) {
            size_t partition_bytes = fdb_decode_u32(bytes + SST_INDEX_ENTRY_BYTES + key_size);
            if (!fdb_add_size(filter_bytes, partition_bytes, &filter_bytes))
                return FASTDB_CORRUPTION;
            index[i].filter_bytes = partition_bytes;
        }
        next_offset += block_bytes;
        previous_key = last_key;
        size_t entry_size = overhead + key_size;
        left -= entry_size;
        bytes += entry_size;
    }
    if (filter_bytes != index_offset - next_offset || left != 0 ||
        fdb_compare(previous_key, reader->properties.max_key) != 0)
        return FASTDB_CORRUPTION;
    return has_filters ? load_filters(reader, next_offset, filter_bytes) : FASTDB_OK;
}

static FastDBStatus load_metadata(FDBSSTReader *reader)
{
    FastDBStatus status = fdb_fs_size(reader->fd, &reader->properties.file_bytes);
    if (status != FASTDB_OK)
        return status;
    Regions regions;
    status = decode_regions(reader, &regions);
    if (status == FASTDB_OK)
        status = load_properties(reader, &regions);
    if (status != FASTDB_OK)
        return status;
    reader->index_image = malloc(regions.index_size);
    if (reader->index_image == NULL)
        return FASTDB_OUT_OF_MEMORY;
    status = read_exact(reader, regions.index_offset, reader->index_image, regions.index_size);
    return status == FASTDB_OK ? parse_index(reader, regions.index_size, regions.index_offset) : status;
}

FastDBStatus fdb_sst_reader_close(FDBSSTReader *reader)
{
    if (reader == NULL)
        return FASTDB_OK;
    FastDBStatus status = reader->fd < 0 ? FASTDB_OK : fdb_fs_close(reader->fd);
    free(reader->properties_image);
    free(reader->index_image);
    free(reader->filter_image);
    free(reader->index);
    free(reader);
    return status;
}

FastDBStatus fdb_sst_reader_open(const FDBDirectory *dir, const char *name, uint64_t id,
                                  const FDBSSTLimits *limits, FDBSSTReader **out)
{
    *out = NULL;
    if (id == 0 || !fdb_sst_valid_limits(limits))
        return FASTDB_INVALID_ARGUMENT;
    FDBSSTReader *reader = calloc(1, sizeof(*reader));
    if (reader == NULL)
        return FASTDB_OUT_OF_MEMORY;
    reader->fs = dir->fs;
    reader->limits = *limits;
    reader->properties.id = id;
    FastDBStatus status = fdb_dir_open_file(dir, name, FDB_FILE_READ, &reader->fd);
    if (status == FASTDB_OK)
        status = load_metadata(reader);
    if (status != FASTDB_OK) {
        (void)fdb_sst_reader_close(reader);
        return status == FASTDB_NOT_FOUND ? FASTDB_CORRUPTION : status;
    }
    *out = reader;
    return FASTDB_OK;
}

const FDBSSTProperties *fdb_sst_reader_properties(const FDBSSTReader *reader)
{
    return &reader->properties;
}

void fdb_sst_reader_set_cache(FDBSSTReader *reader, FDBBlockCache *cache)
{
    reader->cache = cache;
}

size_t fdb_sst_reader_corrupt_partitions(const FDBSSTReader *reader)
{
    return reader->corrupt_partitions;
}

FastDBStatus fdb_sst_iter_open(const FDBSSTReader *reader, FDBSSTIterator **out)
{
    *out = NULL;
    size_t bytes;
    if (!fdb_add_size(sizeof(FDBSSTIterator), reader->limits.max_block_bytes, &bytes))
        return FASTDB_LIMIT_EXCEEDED;
    FDBSSTIterator *iter = malloc(bytes);
    if (iter == NULL)
        return FASTDB_OUT_OF_MEMORY;
    *iter = (FDBSSTIterator){.reader = reader};
    *out = iter;
    return FASTDB_OK;
}

static FastDBStatus decode_row(const FDBSSTIterator *iter, size_t row, FDBRecord *record)
{
    size_t begin = fdb_decode_u32(iter->offsets + row * SST_OFFSET_BYTES);
    size_t end = row + 1 == iter->row_count ? iter->record_bytes :
                 fdb_decode_u32(iter->offsets + (row + 1) * SST_OFFSET_BYTES);
    if (begin >= end || end > iter->record_bytes)
        return FASTDB_CORRUPTION;
    size_t used;
    const FDBSSTLimits *limits = &iter->reader->limits;
    const unsigned char *bytes = iter->block + SST_BLOCK_HEADER_BYTES;
    FastDBStatus status = fdb_record_decode(bytes + begin, end - begin, limits->max_key_bytes,
                                             limits->max_value_bytes, record, &used);
    if (status != FASTDB_OK || used != end - begin ||
        record->sequence < iter->reader->properties.min_sequence ||
        record->sequence > iter->reader->properties.max_sequence)
        return FASTDB_CORRUPTION;
    return FASTDB_OK;
}

static FastDBStatus verify_block(const FDBSSTReader *reader, FDBSSTIterator *iter, size_t index)
{
    if (fdb_decode_u32(iter->offsets) != 0)
        return FASTDB_CORRUPTION;
    FastDBSlice lower = index == 0 ? reader->properties.min_key : reader->index[index - 1].last_key;
    FDBRecord previous = {0};
    for (size_t row = 0; row < iter->row_count; ++row) {
        FDBRecord record;
        FastDBStatus status = decode_row(iter, row, &record);
        if (status != FASTDB_OK)
            return status;
        int order = fdb_compare(row == 0 ? lower : previous.key, record.key);
        if (order > 0 || (row == 0 && index == 0 && order != 0) ||
            (row != 0 && order == 0 && previous.sequence <= record.sequence))
            return FASTDB_CORRUPTION;
        const FDBBloom *bloom = &reader->index[index].bloom;
        if (bloom->bits != NULL && !fdb_bloom_may_contain(bloom, record.key))
            return FASTDB_CORRUPTION;
        previous = record;
    }
    return fdb_compare(previous.key, reader->index[index].last_key) == 0 ?
           FASTDB_OK : FASTDB_CORRUPTION;
}

static FastDBStatus load_block(FDBSSTIterator *iter, size_t index)
{
    const FDBSSTReader *reader = iter->reader;
    const IndexEntry *entry = &reader->index[index];
    if (reader->cache != NULL) {
        fdb_block_cache_release(reader->cache, iter->pin);
        iter->pin = fdb_block_cache_acquire(reader->cache, reader->properties.id,
                                            entry->offset, entry->bytes);
    }
    const unsigned char *block;
    if (iter->pin != NULL) {
        block = fdb_cached_block_data(iter->pin);
    } else {
        FastDBStatus status = read_exact(reader, entry->offset, iter->scratch, entry->bytes);
        if (status != FASTDB_OK)
            return status;
        block = iter->scratch;
    }
    size_t record_bytes = fdb_decode_u32(block + 4);
    size_t rows = fdb_decode_u32(block + 8);
    size_t body_size = entry->bytes - SST_BLOCK_HEADER_BYTES;
    if (fdb_decode_u32(block) != SST_BLOCK_MAGIC || record_bytes > body_size || rows == 0 ||
        rows > record_bytes / (FDB_RECORD_HEADER_BYTES + 1) ||
        (body_size - record_bytes) % SST_OFFSET_BYTES != 0 ||
        rows != (body_size - record_bytes) / SST_OFFSET_BYTES)
        return FASTDB_CORRUPTION;
    iter->block = block;
    iter->offsets = block + SST_BLOCK_HEADER_BYTES + record_bytes;
    iter->block_number = index;
    iter->row_count = rows;
    iter->record_bytes = record_bytes;
    if (iter->pin != NULL)
        return FASTDB_OK;
    if (fdb_decode_u32(block + 12) != fdb_crc32c(block + SST_BLOCK_HEADER_BYTES, body_size))
        return FASTDB_CORRUPTION;
    FastDBStatus status = verify_block(reader, iter, index);
    if (status == FASTDB_OK && reader->cache != NULL)
        fdb_block_cache_store(reader->cache, reader->properties.id, entry->offset, block, entry->bytes);
    return status;
}

static void move_to(FDBSSTIterator *iter, size_t row)
{
    iter->status = decode_row(iter, row, &iter->record);
    iter->row = row;
    iter->valid = iter->status == FASTDB_OK;
}

void fdb_sst_iter_first(FDBSSTIterator *iter)
{
    iter->valid = false;
    if (iter->reader->block_count == 0) {
        iter->status = FASTDB_OK;
        return;
    }
    iter->status = load_block(iter, 0);
    if (iter->status == FASTDB_OK)
        move_to(iter, 0);
}

static size_t block_bound(const FDBSSTReader *reader, FastDBSlice key)
{
    size_t low = 0, high = reader->block_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (fdb_compare(reader->index[mid].last_key, key) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    return low;
}

static bool validate_seek(FDBSSTIterator *iter, FastDBSlice key)
{
    iter->valid = false;
    iter->status = FASTDB_OK;
    if (key.data == NULL || key.size == 0)
        iter->status = FASTDB_INVALID_ARGUMENT;
    else if (key.size > iter->reader->limits.max_key_bytes)
        iter->status = FASTDB_LIMIT_EXCEEDED;
    return iter->status == FASTDB_OK;
}

static void seek_block(FDBSSTIterator *iter, FastDBSlice key, size_t block)
{
    if (block == iter->reader->block_count)
        return;
    iter->status = load_block(iter, block);
    if (iter->status != FASTDB_OK)
        return;

    size_t low = 0, high = iter->row_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        FDBRecord record;
        iter->status = decode_row(iter, mid, &record);
        if (iter->status != FASTDB_OK)
            return;
        if (fdb_compare(record.key, key) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    if (low == iter->row_count)
        iter->status = FASTDB_CORRUPTION;
    else
        move_to(iter, low);
}

void fdb_sst_iter_seek(FDBSSTIterator *iter, FastDBSlice key)
{
    if (validate_seek(iter, key))
        seek_block(iter, key, block_bound(iter->reader, key));
}

void fdb_sst_iter_seek_version(FDBSSTIterator *iter, FastDBSlice key, uint64_t sequence)
{
    if (!validate_seek(iter, key))
        return;
    size_t block = block_bound(iter->reader, key);
    if (block == iter->reader->block_count)
        return;
    const FDBBloom *filter = &iter->reader->index[block].bloom;
    if (filter->bits != NULL) {
        ++iter->bloom_stats.checks;
        if (!fdb_bloom_may_contain(filter, key)) {
            ++iter->bloom_stats.negatives;
            return;
        }
        ++iter->bloom_stats.positives;
    }
    seek_block(iter, key, block);
    if (filter->bits != NULL && iter->valid && fdb_compare(iter->record.key, key) != 0)
        ++iter->bloom_stats.false_positives;
    while (iter->valid && fdb_compare(iter->record.key, key) == 0) {
        if (iter->record.sequence <= sequence)
            return;
        fdb_sst_iter_next(iter);
    }
    iter->valid = false;
}

const FDBRecord *fdb_sst_iter_current(const FDBSSTIterator *iter)
{
    return iter->valid ? &iter->record : NULL;
}

void fdb_sst_iter_next(FDBSSTIterator *iter)
{
    if (!iter->valid)
        return;
    if (iter->row + 1 < iter->row_count) {
        move_to(iter, iter->row + 1);
        return;
    }
    iter->valid = false;
    size_t previous = iter->block_number;
    size_t next = previous + 1;
    if (next == iter->reader->block_count)
        return;
    uint64_t last_sequence = iter->record.sequence;
    iter->status = load_block(iter, next);
    if (iter->status != FASTDB_OK)
        return;
    move_to(iter, 0);
    const FastDBSlice last_key = iter->reader->index[previous].last_key;
    if (iter->valid && fdb_compare(last_key, iter->record.key) == 0 &&
        last_sequence <= iter->record.sequence) {
        iter->valid = false;
        iter->status = FASTDB_CORRUPTION;
    }
}

FastDBStatus fdb_sst_iter_status(const FDBSSTIterator *iter)
{
    return iter->status;
}

void fdb_sst_iter_bloom_stats(const FDBSSTIterator *iter, FDBSSTBloomStats *out)
{
    *out = iter->bloom_stats;
    out->corrupt_partitions = iter->reader->corrupt_partitions;
}

void fdb_sst_iter_close(FDBSSTIterator *iter)
{
    if (iter != NULL && iter->pin != NULL)
        fdb_block_cache_release(iter->reader->cache, iter->pin);
    free(iter);
}

FastDBStatus fdb_sst_reader_walk(const FDBSSTReader *reader, FDBSSTVisit visit, void *context)
{
    if (reader->properties.records == 0)
        return FASTDB_OK;
    FDBSSTIterator *iter;
    FastDBStatus status = fdb_sst_iter_open(reader, &iter);
    if (status != FASTDB_OK)
        return status;
    uint64_t records = 0, tombstones = 0, min = UINT64_MAX, max = 0;
    for (fdb_sst_iter_first(iter); iter->valid; fdb_sst_iter_next(iter)) {
        uint64_t sequence = iter->record.sequence;
        if (sequence < min)
            min = sequence;
        if (sequence > max)
            max = sequence;
        ++records;
        if (iter->record.kind == FDB_DELETE)
            ++tombstones;
        if (visit != NULL) {
            status = visit(context, &iter->record);
            if (status != FASTDB_OK) {
                fdb_sst_iter_close(iter);
                return status;
            }
        }
    }
    status = iter->status;
    if (status == FASTDB_OK &&
        (records != reader->properties.records || tombstones != reader->properties.tombstones ||
         min != reader->properties.min_sequence || max != reader->properties.max_sequence))
        status = FASTDB_CORRUPTION;
    fdb_sst_iter_close(iter);
    return status;
}

FastDBStatus fdb_sst_reader_verify(const FDBSSTReader *reader)
{
    return fdb_sst_reader_walk(reader, NULL, NULL);
}
