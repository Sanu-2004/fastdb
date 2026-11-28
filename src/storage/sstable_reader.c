#include "sstable_format.h"
#include "block_codec.h"
#include "bloom.h"
#include "crc32c.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct IndexEntry {
    uint64_t offset;
    size_t bytes;
    FastDBSlice last_key;
    size_t filter_bytes;
    union {
        FDBBloom bloom;
        struct {
            uint64_t offset;
            atomic_bool corrupt;
        } streamed;
    } filter;
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
    size_t streamed_filter_bytes;
    size_t max_leaf_bytes;
    atomic_size_t verified_leaf_corruption;
    bool partitioned;
    uint32_t features;
    uint32_t codec;
};

struct FDBSSTIterator {
    const FDBSSTReader *reader;
    FDBCachedBlock *pin;
    const unsigned char *block;
    const unsigned char *offsets;
    unsigned char *leaf_image;
    IndexEntry *leaf;
    size_t leaf_number;
    size_t leaf_blocks;
    size_t leaf_corruption;
    size_t earlier_corruption;
    bool leaf_loaded;
    size_t block_number;
    size_t row_count;
    size_t row;
    size_t record_bytes;
    size_t block_capacity;
    void *decode_state;
    size_t decode_state_size;
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

typedef struct FilterRegion {
    uint64_t offset;
    size_t size;
} FilterRegion;

bool fdb_sst_reader_budget(const FDBSSTLimits *limits, size_t *bytes)
{
    size_t keys = limits->max_key_bytes;
    size_t block_capacity, decode_state_size;
    size_t blocks = limits->max_index_bytes / SST_MIN_INDEX_ENTRY_BYTES;
    *bytes = sizeof(FDBSSTReader) + sizeof(FDBSSTIterator) +
             SST_PROPERTIES_HEADER_BYTES + SST_CRC_BYTES;
    return fdb_block_decode_workspace(limits->codec, limits->max_block_bytes,
                                      &block_capacity, &decode_state_size) &&
           blocks <= SIZE_MAX / sizeof(IndexEntry) &&
           fdb_add_size(*bytes, blocks * sizeof(IndexEntry), bytes) &&
           fdb_add_size(*bytes, block_capacity, bytes) &&
           fdb_add_size(*bytes, decode_state_size, bytes) &&
           fdb_add_size(*bytes, limits->max_index_bytes, bytes) &&
           fdb_add_size(*bytes, limits->max_filter_bytes, bytes) &&
           keys <= (SIZE_MAX - *bytes) / 2 &&
           fdb_add_size(*bytes, 2 * keys, bytes);
}

bool fdb_sst_reader_workspace(const FDBSSTReader *reader, size_t *bytes)
{
    FDBSSTLimits limits = reader->limits;
    limits.codec = (uint8_t)reader->codec;
    return fdb_sst_reader_budget(&limits, bytes);
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
    uint32_t version = fdb_decode_u32(header + 8);
    if (version != SST_VERSION && version != SST_PARTITIONED_VERSION)
        return FASTDB_UNSUPPORTED;
    reader->partitioned = version == SST_PARTITIONED_VERSION;
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
    const char *magic = reader->partitioned ? SST_PARTITIONED_FOOTER_MAGIC : SST_FOOTER_MAGIC;
    if (memcmp(footer, magic, 8) != 0 ||
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
    reader->codec = fdb_decode_u32(bytes + 40);
    if (!fdb_block_codec_supported(reader->codec) ||
        (reader->codec != FASTDB_COMPRESSION_NONE && !reader->partitioned))
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

static FastDBStatus index_streamed_filters(FDBSSTReader *reader, uint64_t offset)
{
    /* parse_index already proved that all partition lengths fit before the index. */
    for (size_t i = 0; i < reader->block_count; ++i) {
        IndexEntry *entry = &reader->index[i];
        size_t bytes = entry->filter_bytes;
        if (bytes > reader->limits.max_filter_bytes)
            return FASTDB_LIMIT_EXCEEDED;
        if (bytes > reader->streamed_filter_bytes)
            reader->streamed_filter_bytes = bytes;
        entry->filter.streamed.offset = offset;
        atomic_init(&entry->filter.streamed.corrupt, false);
        offset += bytes;
    }
    return FASTDB_OK;
}

static bool decode_filter(const IndexEntry *entry, const unsigned char *image, FDBBloom *filter)
{
    return fdb_bloom_decode(image, entry->filter_bytes, filter) &&
           fdb_bloom_may_contain(filter, entry->last_key);
}

static size_t decode_filters(IndexEntry *entries, size_t count, const unsigned char *image)
{
    size_t corrupt = 0;
    for (size_t i = 0; i < count; ++i) {
        IndexEntry *entry = &entries[i];
        if (!decode_filter(entry, image, &entry->filter.bloom)) {
            entry->filter.bloom = (FDBBloom){0};
            ++corrupt;
        }
        image += entry->filter_bytes;
    }
    return corrupt;
}

static FastDBStatus load_filters(FDBSSTReader *reader, uint64_t offset, size_t size)
{
    if (size > reader->limits.max_filter_bytes)
        return index_streamed_filters(reader, offset);
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
    reader->corrupt_partitions = decode_filters(reader->index, reader->block_count, image);
    return FASTDB_OK;
}

static FastDBStatus parse_blocks(const FDBSSTReader *reader, const unsigned char *bytes, size_t size,
                                  uint64_t data_start, uint64_t index_offset, FastDBSlice lower,
                                  FastDBSlice last, IndexEntry *index, size_t capacity,
                                  size_t *block_count, FilterRegion *filters)
{
    if (fdb_decode_u32(bytes + size - SST_CRC_BYTES) != fdb_crc32c(bytes, size - SST_CRC_BYTES))
        return FASTDB_CORRUPTION;
    size_t count = fdb_decode_u32(bytes);
    size_t left = size - SST_INDEX_HEADER_BYTES - SST_CRC_BYTES;
    bool has_filters = (reader->features & SST_FEATURE_BLOOM) != 0;
    size_t overhead = SST_INDEX_ENTRY_BYTES + (has_filters ? sizeof(uint32_t) : 0);
    size_t minimum_block = SST_BLOCK_HEADER_BYTES +
        (reader->codec == FASTDB_COMPRESSION_NONE ? SST_MIN_ROW_BYTES : 1);
    if (reader->properties.records == 0)
        return count == 0 && left == 0 && index_offset == SST_HEADER_BYTES ?
               FASTDB_OK : FASTDB_CORRUPTION;
    if (count == 0 || count > reader->properties.records ||
        count > left / (overhead + 1) || count > capacity)
        return FASTDB_CORRUPTION;

    uint64_t next_offset = data_start;
    size_t filter_bytes = 0;
    FastDBSlice previous_key = lower;
    bytes += SST_INDEX_HEADER_BYTES;
    for (size_t i = 0; i < count; ++i) {
        if (left < overhead)
            return FASTDB_CORRUPTION;
        uint64_t offset = fdb_decode_u64(bytes);
        size_t block_bytes = fdb_decode_u32(bytes + 8);
        size_t key_size = fdb_decode_u32(bytes + 12);
        if (offset != next_offset || block_bytes < minimum_block ||
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
        fdb_compare(previous_key, last) != 0)
        return FASTDB_CORRUPTION;
    *block_count = count;
    *filters = (FilterRegion){next_offset, filter_bytes};
    return FASTDB_OK;
}

static FastDBStatus parse_directory(FDBSSTReader *reader, size_t size, uint64_t root_offset)
{
    const unsigned char *image = reader->index_image;
    size_t count = fdb_decode_u32(image);
    size_t left = size - SST_INDEX_HEADER_BYTES - SST_CRC_BYTES;
    if (fdb_decode_u32(image + size - SST_CRC_BYTES) != fdb_crc32c(image, size - SST_CRC_BYTES))
        return FASTDB_CORRUPTION;
    if (count == 0)
        return reader->properties.records == 0 && left == 0 && root_offset == SST_HEADER_BYTES ?
               FASTDB_OK : FASTDB_CORRUPTION;
    if (count > reader->properties.records)
        return FASTDB_CORRUPTION;
    const unsigned char *entry = image + SST_INDEX_HEADER_BYTES;
    uint64_t data_start = SST_HEADER_BYTES;
    FastDBSlice previous = reader->properties.min_key;
    for (size_t i = 0; i < count; ++i) {
        if (left < SST_INDEX_ENTRY_BYTES)
            return FASTDB_CORRUPTION;
        uint64_t offset = fdb_decode_u64(entry);
        size_t length = fdb_decode_u32(entry + 8);
        size_t key_bytes = fdb_decode_u32(entry + 12);
        if (offset <= data_start || offset > root_offset || length > root_offset - offset ||
            length < SST_INDEX_HEADER_BYTES + SST_MIN_INDEX_ENTRY_BYTES + SST_CRC_BYTES ||
            key_bytes == 0 || key_bytes > left - SST_INDEX_ENTRY_BYTES ||
            key_bytes > reader->limits.max_key_bytes)
            return FASTDB_CORRUPTION;
        if (length > reader->limits.max_index_bytes - size)
            return FASTDB_LIMIT_EXCEEDED;
        FastDBSlice key = {entry + SST_INDEX_ENTRY_BYTES, key_bytes};
        if (fdb_compare(previous, key) > 0)
            return FASTDB_CORRUPTION;
        reader->index[i] = (IndexEntry){.offset = offset, .bytes = length, .last_key = key};
        if (length > reader->max_leaf_bytes)
            reader->max_leaf_bytes = length;
        size_t used = SST_INDEX_ENTRY_BYTES + key_bytes;
        entry += used;
        left -= used;
        previous = key;
        data_start = offset + length;
    }
    if (left != 0 || data_start != root_offset ||
        fdb_compare(previous, reader->properties.max_key) != 0)
        return FASTDB_CORRUPTION;
    reader->block_count = count;
    return FASTDB_OK;
}

static FastDBStatus parse_index(FDBSSTReader *reader, size_t size, uint64_t index_offset)
{
    size_t count = fdb_decode_u32(reader->index_image);
    size_t entries_size = size - SST_INDEX_HEADER_BYTES - SST_CRC_BYTES;
    if (count > entries_size / SST_MIN_INDEX_ENTRY_BYTES || count > SIZE_MAX / sizeof(IndexEntry))
        return FASTDB_CORRUPTION;
    if (count != 0) {
        reader->index = malloc(count * sizeof(*reader->index));
        if (reader->index == NULL)
            return FASTDB_OUT_OF_MEMORY;
    }
    if (reader->partitioned)
        return parse_directory(reader, size, index_offset);
    FilterRegion filters = {0};
    FastDBStatus status = parse_blocks(reader, reader->index_image, size, SST_HEADER_BYTES,
        index_offset, reader->properties.min_key, reader->properties.max_key, reader->index,
        count, &reader->block_count, &filters);
    if (status == FASTDB_OK && (reader->features & SST_FEATURE_BLOOM) != 0)
        status = load_filters(reader, filters.offset, filters.size);
    return status;
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
    atomic_init(&reader->verified_leaf_corruption, 0);
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

bool fdb_sst_reader_declares_codec(const FDBSSTReader *reader)
{
    return reader->codec != FASTDB_COMPRESSION_NONE;
}

void fdb_sst_reader_set_cache(FDBSSTReader *reader, FDBBlockCache *cache)
{
    reader->cache = cache;
}

size_t fdb_sst_reader_corrupt_partitions(const FDBSSTReader *reader)
{
    if (reader->partitioned)
        return atomic_load_explicit(&reader->verified_leaf_corruption, memory_order_relaxed);
    if (reader->streamed_filter_bytes == 0)
        return reader->corrupt_partitions;
    size_t count = 0;
    for (size_t i = 0; i < reader->block_count; ++i) {
        if (atomic_load_explicit(&reader->index[i].filter.streamed.corrupt, memory_order_relaxed))
            ++count;
    }
    return count;
}

FastDBStatus fdb_sst_iter_open(const FDBSSTReader *reader, FDBSSTIterator **out)
{
    *out = NULL;
    size_t bytes, block_capacity, decode_state_size;
    size_t filter_capacity = reader->streamed_filter_bytes;
    if (reader->partitioned && (reader->features & SST_FEATURE_BLOOM) != 0 && reader->max_leaf_bytes != 0)
        filter_capacity = reader->limits.max_filter_bytes;
    if (!fdb_block_decode_workspace(reader->codec, reader->limits.max_block_bytes,
                                    &block_capacity, &decode_state_size) ||
        !fdb_add_size(sizeof(FDBSSTIterator), block_capacity, &bytes) ||
        !fdb_add_size(bytes, filter_capacity, &bytes))
        return FASTDB_LIMIT_EXCEEDED;
    FDBSSTIterator *iter = malloc(bytes);
    if (iter == NULL)
        return FASTDB_OUT_OF_MEMORY;
    *iter = (FDBSSTIterator){.reader = reader, .block_capacity = block_capacity};
    if (decode_state_size != 0) {
        iter->decode_state = malloc(decode_state_size);
        if (iter->decode_state == NULL) {
            fdb_sst_iter_close(iter);
            return FASTDB_OUT_OF_MEMORY;
        }
        iter->decode_state_size = decode_state_size;
    }
    if (reader->max_leaf_bytes != 0) {
        size_t entries = reader->max_leaf_bytes / SST_MIN_INDEX_ENTRY_BYTES;
        if (entries > SIZE_MAX / sizeof(*iter->leaf)) {
            fdb_sst_iter_close(iter);
            return FASTDB_LIMIT_EXCEEDED;
        }
        iter->leaf_image = malloc(reader->max_leaf_bytes);
        iter->leaf = malloc(entries * sizeof(*iter->leaf));
        if (iter->leaf_image == NULL || iter->leaf == NULL) {
            fdb_sst_iter_close(iter);
            return FASTDB_OUT_OF_MEMORY;
        }
    }
    *out = iter;
    return FASTDB_OK;
}

static IndexEntry *cursor_index(const FDBSSTIterator *iter)
{
    return iter->reader->partitioned ? iter->leaf : iter->reader->index;
}

static size_t cursor_blocks(const FDBSSTIterator *iter)
{
    return iter->reader->partitioned ? iter->leaf_blocks : iter->reader->block_count;
}

static FastDBStatus load_leaf(FDBSSTIterator *iter, size_t number)
{
    if (iter->leaf_loaded && iter->leaf_number == number)
        return FASTDB_OK;
    iter->leaf_loaded = false;
    const FDBSSTReader *reader = iter->reader;
    const IndexEntry *leaf = &reader->index[number];
    uint64_t start = SST_HEADER_BYTES;
    FastDBSlice lower = reader->properties.min_key;
    if (number != 0) {
        const IndexEntry *prior = &reader->index[number - 1];
        start = prior->offset + prior->bytes;
        lower = prior->last_key;
    }
    FastDBStatus status = read_exact(reader, leaf->offset, iter->leaf_image, leaf->bytes);
    FilterRegion filters;
    if (status == FASTDB_OK)
        status = parse_blocks(reader, iter->leaf_image, leaf->bytes, start, leaf->offset,
            lower, leaf->last_key, iter->leaf, reader->max_leaf_bytes / SST_MIN_INDEX_ENTRY_BYTES,
            &iter->leaf_blocks, &filters);
    if (status != FASTDB_OK)
        return status;
    if (filters.size > reader->limits.max_filter_bytes)
        return FASTDB_LIMIT_EXCEEDED;
    iter->leaf_corruption = 0;
    if (filters.size != 0) {
        unsigned char *filter_image = iter->scratch + iter->block_capacity;
        status = read_exact(reader, filters.offset, filter_image, filters.size);
        if (status != FASTDB_OK)
            return status;
        iter->leaf_corruption = decode_filters(iter->leaf, iter->leaf_blocks, filter_image);
    } else if ((reader->features & SST_FEATURE_BLOOM) != 0) {
        iter->leaf_corruption = iter->leaf_blocks;
    }
    iter->leaf_number = number;
    iter->leaf_loaded = true;
    return FASTDB_OK;
}

static FastDBStatus read_filter(FDBSSTIterator *iter, size_t index, FDBBloom *out)
{
    const FDBSSTReader *reader = iter->reader;
    IndexEntry *entry = &cursor_index(iter)[index];
    if (reader->streamed_filter_bytes == 0) {
        *out = entry->filter.bloom;
        return FASTDB_OK;
    }
    *out = (FDBBloom){0};
    if (atomic_load_explicit(&entry->filter.streamed.corrupt, memory_order_relaxed))
        return FASTDB_OK;

    unsigned char *image = iter->scratch + iter->block_capacity;
    if (entry->filter_bytes >= FDB_BLOOM_MIN_IMAGE_BYTES) {
        FastDBStatus status = read_exact(reader, entry->filter.streamed.offset, image,
                                          entry->filter_bytes);
        if (status != FASTDB_OK)
            return status;
        if (decode_filter(entry, image, out))
            return FASTDB_OK;
    }
    *out = (FDBBloom){0};
    atomic_store_explicit(&entry->filter.streamed.corrupt, true, memory_order_relaxed);
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

static FastDBStatus verify_block(const FDBSSTReader *reader, FDBSSTIterator *iter, size_t index,
                                  const FDBBloom *bloom)
{
    if (fdb_decode_u32(iter->offsets) != 0)
        return FASTDB_CORRUPTION;
    const IndexEntry *entries = cursor_index(iter);
    bool first_block = index == 0 && iter->leaf_number == 0;
    FastDBSlice lower = reader->properties.min_key;
    if (index != 0)
        lower = entries[index - 1].last_key;
    else if (iter->leaf_number != 0)
        lower = reader->index[iter->leaf_number - 1].last_key;
    FDBRecord previous = {0};
    for (size_t row = 0; row < iter->row_count; ++row) {
        FDBRecord record;
        FastDBStatus status = decode_row(iter, row, &record);
        if (status != FASTDB_OK)
            return status;
        int order = fdb_compare(row == 0 ? lower : previous.key, record.key);
        if (order > 0 || (row == 0 && first_block && order != 0) ||
            (row != 0 && order == 0 && previous.sequence <= record.sequence))
            return FASTDB_CORRUPTION;
        if (bloom->bits != NULL && !fdb_bloom_may_contain(bloom, record.key))
            return FASTDB_CORRUPTION;
        previous = record;
    }
    return fdb_compare(previous.key, entries[index].last_key) == 0 ?
           FASTDB_OK : FASTDB_CORRUPTION;
}

static FastDBStatus load_block(FDBSSTIterator *iter, size_t index, const FDBBloom *filter)
{
    const FDBSSTReader *reader = iter->reader;
    const IndexEntry *entry = &cursor_index(iter)[index];
    if (reader->cache != NULL) {
        fdb_block_cache_release(reader->cache, iter->pin);
        iter->pin = fdb_block_cache_acquire(reader->cache, reader->properties.id,
                                            entry->offset, entry->bytes);
    }
    const unsigned char *block;
    size_t size = entry->bytes;
    if (iter->pin != NULL) {
        block = fdb_cached_block_data(iter->pin);
        size = fdb_cached_block_size(iter->pin);
    } else {
        unsigned char *stored = iter->scratch;
        if (reader->codec != FASTDB_COMPRESSION_NONE)
            stored += iter->block_capacity - size;
        FastDBStatus status = read_exact(reader, entry->offset, stored, size);
        if (status != FASTDB_OK)
            return status;
        block = stored;
        uint32_t magic = fdb_block_codec_magic(reader->codec);
        if (magic != 0 && fdb_decode_u32(stored) == magic) {
            status = fdb_block_decode(reader->codec, stored, size, iter->scratch,
                                      reader->limits.max_block_bytes, iter->decode_state,
                                      iter->decode_state_size, &size);
            if (status != FASTDB_OK)
                return status;
            block = iter->scratch;
        }
    }
    if (size < SST_BLOCK_HEADER_BYTES + SST_MIN_ROW_BYTES)
        return FASTDB_CORRUPTION;
    size_t record_bytes = fdb_decode_u32(block + 4);
    size_t rows = fdb_decode_u32(block + 8);
    size_t body_size = size - SST_BLOCK_HEADER_BYTES;
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
    FDBBloom partition;
    if (filter == NULL) {
        FastDBStatus status = read_filter(iter, index, &partition);
        if (status != FASTDB_OK)
            return status;
        filter = &partition;
    }
    FastDBStatus status = verify_block(reader, iter, index, filter);
    if (status == FASTDB_OK && reader->cache != NULL)
        fdb_block_cache_store(reader->cache, reader->properties.id, entry->offset, block, size,
                              entry->bytes);
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
    iter->earlier_corruption = 0;
    if (iter->reader->block_count == 0) {
        iter->status = FASTDB_OK;
        return;
    }
    iter->status = iter->reader->partitioned ? load_leaf(iter, 0) : FASTDB_OK;
    if (iter->status == FASTDB_OK)
        iter->status = load_block(iter, 0, NULL);
    if (iter->status == FASTDB_OK)
        move_to(iter, 0);
}

static size_t block_bound(const IndexEntry *entries, size_t count, FastDBSlice key)
{
    size_t low = 0, high = count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (fdb_compare(entries[mid].last_key, key) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    return low;
}

static size_t choose_block(FDBSSTIterator *iter, FastDBSlice key)
{
    const FDBSSTReader *reader = iter->reader;
    if (reader->partitioned) {
        size_t leaf = block_bound(reader->index, reader->block_count, key);
        if (leaf == reader->block_count)
            return SIZE_MAX;
        iter->status = load_leaf(iter, leaf);
        if (iter->status != FASTDB_OK)
            return SIZE_MAX;
    }
    size_t count = cursor_blocks(iter);
    size_t block = block_bound(cursor_index(iter), count, key);
    return block == count ? SIZE_MAX : block;
}

static bool validate_seek(FDBSSTIterator *iter, FastDBSlice key)
{
    iter->valid = false;
    iter->status = FASTDB_OK;
    iter->earlier_corruption = 0;
    if (key.data == NULL || key.size == 0)
        iter->status = FASTDB_INVALID_ARGUMENT;
    else if (key.size > iter->reader->limits.max_key_bytes)
        iter->status = FASTDB_LIMIT_EXCEEDED;
    return iter->status == FASTDB_OK;
}

static void seek_block(FDBSSTIterator *iter, FastDBSlice key, size_t block, const FDBBloom *filter)
{
    if (block == SIZE_MAX)
        return;
    iter->status = load_block(iter, block, filter);
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
    if (validate_seek(iter, key)) {
        size_t block = choose_block(iter, key);
        seek_block(iter, key, block, NULL);
    }
}

void fdb_sst_iter_seek_version(FDBSSTIterator *iter, FastDBSlice key, uint64_t sequence)
{
    if (!validate_seek(iter, key))
        return;
    size_t block = choose_block(iter, key);
    if (block == SIZE_MAX)
        return;
    FDBBloom filter;
    iter->status = read_filter(iter, block, &filter);
    if (iter->status != FASTDB_OK)
        return;
    if (filter.bits != NULL) {
        ++iter->bloom_stats.checks;
        if (!fdb_bloom_may_contain(&filter, key)) {
            ++iter->bloom_stats.negatives;
            return;
        }
        ++iter->bloom_stats.positives;
    }
    seek_block(iter, key, block, &filter);
    if (filter.bits != NULL && iter->valid && fdb_compare(iter->record.key, key) != 0)
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
    FastDBSlice last_key = cursor_index(iter)[previous].last_key;
    if (next == cursor_blocks(iter)) {
        const FDBSSTReader *reader = iter->reader;
        if (!reader->partitioned || iter->leaf_number + 1 == reader->block_count)
            return;
        last_key = reader->index[iter->leaf_number].last_key;
        if (!fdb_add_size(iter->earlier_corruption, iter->leaf_corruption, &iter->earlier_corruption)) {
            iter->status = FASTDB_LIMIT_EXCEEDED;
            return;
        }
        iter->status = load_leaf(iter, iter->leaf_number + 1);
        if (iter->status != FASTDB_OK)
            return;
        next = 0;
    }
    uint64_t last_sequence = iter->record.sequence;
    iter->status = load_block(iter, next, NULL);
    if (iter->status != FASTDB_OK)
        return;
    move_to(iter, 0);
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
    if (iter->reader->partitioned) {
        if (!fdb_add_size(iter->earlier_corruption, iter->leaf_corruption, &out->corrupt_partitions))
            out->corrupt_partitions = SIZE_MAX;
    } else {
        out->corrupt_partitions = fdb_sst_reader_corrupt_partitions(iter->reader);
    }
}

void fdb_sst_iter_close(FDBSSTIterator *iter)
{
    if (iter != NULL && iter->pin != NULL)
        fdb_block_cache_release(iter->reader->cache, iter->pin);
    if (iter != NULL) {
        free(iter->decode_state);
        free(iter->leaf_image);
        free(iter->leaf);
    }
    free(iter);
}

FastDBStatus fdb_sst_reader_walk(FDBSSTReader *reader, FDBSSTVisit visit, void *context)
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
    if (status == FASTDB_OK && reader->partitioned) {
        FDBSSTBloomStats stats;
        fdb_sst_iter_bloom_stats(iter, &stats);
        atomic_store_explicit(&reader->verified_leaf_corruption, stats.corrupt_partitions,
                              memory_order_relaxed);
    }
    fdb_sst_iter_close(iter);
    return status;
}

FastDBStatus fdb_sst_reader_verify(FDBSSTReader *reader)
{
    return fdb_sst_reader_walk(reader, NULL, NULL);
}
