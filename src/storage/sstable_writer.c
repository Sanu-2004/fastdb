#include "sstable_format.h"
#include "bloom.h"
#include "crc32c.h"

#include <stdlib.h>
#include <string.h>

struct FDBSSTWriter {
    int fd;
    FDBFs fs;
    FDBSSTLimits limits;
    FDBSSTProperties properties;
    FastDBStatus error;
    uint64_t offset;
    uint64_t previous_sequence;
    unsigned char *block;
    uint32_t *row_offsets;
    size_t row_count;
    size_t record_bytes;
    unsigned char *index;
    size_t index_bytes;
    uint32_t block_count;
    unsigned char *directory;
    size_t directory_bytes;
    uint32_t leaf_count;
    unsigned char *filters;
    size_t filter_bytes;
    size_t pending_filter_bytes;
    unsigned char *min_key;
    unsigned char *max_key;
};

static size_t leaf_capacity(const FDBSSTLimits *limits)
{
    return limits->partitioned ? limits->max_index_bytes / 2 : limits->max_index_bytes;
}

bool fdb_sst_valid_limits(const FDBSSTLimits *limits)
{
    return limits != NULL && limits->max_key_bytes != 0 &&
           limits->max_key_bytes <= UINT32_MAX && limits->max_value_bytes <= UINT32_MAX &&
           limits->target_block_bytes >= SST_BLOCK_HEADER_BYTES + SST_MIN_ROW_BYTES &&
           limits->target_block_bytes <= limits->max_block_bytes &&
           limits->max_block_bytes <= UINT32_MAX &&
           leaf_capacity(limits) >= SST_INDEX_HEADER_BYTES + SST_MIN_INDEX_ENTRY_BYTES + SST_CRC_BYTES &&
           limits->max_index_bytes <= UINT32_MAX && limits->max_filter_bytes <= UINT32_MAX &&
           limits->bloom_bits_per_key <= FDB_BLOOM_MAX_BITS_PER_KEY &&
           (limits->bloom_bits_per_key == 0 ||
            limits->max_filter_bytes >= FDB_BLOOM_MIN_IMAGE_BYTES);
}

bool fdb_sst_writer_budget(const FDBSSTLimits *limits, size_t *bytes)
{
    size_t keys = limits->max_key_bytes;
    size_t offsets = limits->max_block_bytes / SST_MIN_ROW_BYTES * sizeof(uint32_t);
    *bytes = sizeof(FDBSSTWriter) + SST_PROPERTIES_HEADER_BYTES + SST_CRC_BYTES;
    return fdb_add_size(*bytes, limits->max_block_bytes, bytes) &&
           fdb_add_size(*bytes, offsets, bytes) &&
           fdb_add_size(*bytes, limits->max_index_bytes, bytes) &&
           fdb_add_size(*bytes, limits->max_filter_bytes, bytes) &&
           keys <= (SIZE_MAX - *bytes) / 4 &&
           fdb_add_size(*bytes, 4 * keys, bytes);
}

bool fdb_sst_tree_fits(const FDBSSTLimits *limits, size_t keys, size_t key_bytes)
{
    if (keys == 0)
        return key_bytes == 0;
    size_t capacity = leaf_capacity(limits);
    size_t available = capacity - SST_INDEX_HEADER_BYTES - SST_CRC_BYTES;
    bool bloom = limits->bloom_bits_per_key != 0;
    size_t overhead = SST_INDEX_ENTRY_BYTES + (bloom ? sizeof(uint32_t) : 0);
    if (limits->partitioned) {
        size_t max_key = key_bytes < limits->max_key_bytes ? key_bytes : limits->max_key_bytes;
        size_t entry;
        if (max_key == 0 || !fdb_add_size(overhead, max_key, &entry))
            return false;
        size_t per_leaf = available / entry;
        size_t filter_rows = limits->max_filter_bytes / FDB_BLOOM_MIN_IMAGE_BYTES;
        if (bloom && filter_rows < per_leaf)
            per_leaf = filter_rows;
        if (per_leaf == 0)
            return false;
        size_t leaves = 1 + (keys - 1) / per_leaf;
        size_t root_keys = leaves <= key_bytes / max_key ? leaves * max_key : key_bytes;
        size_t root_room = limits->max_index_bytes - capacity - SST_INDEX_HEADER_BYTES - SST_CRC_BYTES;
        return root_keys <= root_room && leaves <= (root_room - root_keys) / SST_INDEX_ENTRY_BYTES;
    }
    /* One index entry and one minimal Bloom partition per row is the worst case. */
    return key_bytes <= available && keys <= (available - key_bytes) / overhead &&
           (!bloom || keys <= limits->max_filter_bytes / FDB_BLOOM_MIN_IMAGE_BYTES);
}

static FastDBStatus append_bytes(FDBSSTWriter *writer, const void *bytes, size_t count)
{
    size_t written;
    FastDBStatus status = fdb_fs_write(&writer->fs, writer->fd, writer->offset,
                                       bytes, count, &written);
    if (status != FASTDB_OK)
        writer->error = status;
    else
        writer->offset += count;
    return status;
}

FastDBStatus fdb_sst_writer_destroy(FDBSSTWriter *writer)
{
    if (writer == NULL)
        return FASTDB_OK;
    FastDBStatus status = writer->error;
    if (writer->fd >= 0) {
        FastDBStatus close_status = fdb_fs_close(writer->fd);
        if (status == FASTDB_OK)
            status = close_status;
    }
    free(writer->block);
    free(writer->row_offsets);
    free(writer->index);
    free(writer->directory);
    free(writer->filters);
    free(writer->min_key);
    free(writer->max_key);
    free(writer);
    return status;
}

static bool allocate_buffers(FDBSSTWriter *writer)
{
    size_t row_capacity = writer->limits.max_block_bytes / SST_MIN_ROW_BYTES;
    writer->row_offsets = malloc(row_capacity * sizeof(*writer->row_offsets));
    writer->block = malloc(writer->limits.max_block_bytes);
    size_t capacity = leaf_capacity(&writer->limits);
    writer->index = malloc(capacity);
    if (writer->limits.partitioned)
        writer->directory = malloc(writer->limits.max_index_bytes - capacity);
    if (writer->limits.bloom_bits_per_key != 0)
        writer->filters = malloc(writer->limits.max_filter_bytes);
    writer->min_key = malloc(writer->limits.max_key_bytes);
    writer->max_key = malloc(writer->limits.max_key_bytes);
    return writer->row_offsets != NULL && writer->block != NULL && writer->index != NULL &&
           (!writer->limits.partitioned || writer->directory != NULL) &&
           (writer->limits.bloom_bits_per_key == 0 || writer->filters != NULL) &&
           writer->min_key != NULL && writer->max_key != NULL;
}

FastDBStatus fdb_sst_writer_open(const FDBDirectory *dir, const char *temporary_name, uint64_t id,
                                  const FDBSSTLimits *limits, FDBSSTWriter **out)
{
    *out = NULL;
    if (id == 0 || !fdb_sst_valid_limits(limits))
        return FASTDB_INVALID_ARGUMENT;
    FDBSSTWriter *writer = calloc(1, sizeof(*writer));
    if (writer == NULL)
        return FASTDB_OUT_OF_MEMORY;
    writer->fd = -1;
    writer->fs = dir->fs;
    writer->limits = *limits;
    writer->properties.id = id;
    writer->properties.min_sequence = UINT64_MAX;
    writer->index_bytes = SST_INDEX_HEADER_BYTES;
    writer->directory_bytes = SST_INDEX_HEADER_BYTES;
    if (!allocate_buffers(writer)) {
        (void)fdb_sst_writer_destroy(writer);
        return FASTDB_OUT_OF_MEMORY;
    }
    FastDBStatus status = fdb_dir_open_file(dir, temporary_name, FDB_FILE_CREATE_NEW, &writer->fd);
    if (status == FASTDB_OK) {
        unsigned char header[SST_HEADER_BYTES] = {0};
        memcpy(header, SST_MAGIC, 8);
        fdb_encode_u32(header + 8, limits->partitioned ? SST_PARTITIONED_VERSION : SST_VERSION);
        if (writer->filters != NULL)
            fdb_encode_u32(header + 12, SST_FEATURE_BLOOM);
        fdb_encode_u64(header + 16, id);
        fdb_encode_u32(header + 28, fdb_crc32c(header, 28));
        status = append_bytes(writer, header, sizeof(header));
    }
    if (status != FASTDB_OK) {
        (void)fdb_sst_writer_destroy(writer);
        return status;
    }
    *out = writer;
    return FASTDB_OK;
}

static size_t block_size(const FDBSSTWriter *writer)
{
    return SST_BLOCK_HEADER_BYTES + writer->record_bytes + writer->row_count * SST_OFFSET_BYTES;
}

FastDBSlice fdb_sst_writer_last_key(const FDBSSTWriter *writer)
{
    return writer->properties.max_key;
}

uint64_t fdb_sst_writer_file_bytes(const FDBSSTWriter *writer)
{
    if (writer->fd < 0)
        return writer->properties.file_bytes;
    /* Each pending region is bounded by the 32-bit table settings. */
    uint64_t buffered = writer->index_bytes + (uint64_t)writer->filter_bytes +
                        SST_PROPERTIES_HEADER_BYTES + SST_FOOTER_BYTES +
                        writer->properties.min_key.size + writer->properties.max_key.size +
                        2 * SST_CRC_BYTES;
    if (writer->row_count != 0) {
        buffered += block_size(writer) + (uint64_t)SST_INDEX_ENTRY_BYTES +
                    writer->properties.max_key.size;
        if (writer->filters != NULL)
            buffered += sizeof(uint32_t) + (uint64_t)writer->pending_filter_bytes;
    }
    if (writer->directory != NULL) {
        buffered += writer->directory_bytes + (uint64_t)SST_CRC_BYTES;
        if (writer->block_count != 0 || writer->row_count != 0)
            buffered += SST_INDEX_ENTRY_BYTES + (uint64_t)writer->properties.max_key.size;
    }
    return writer->offset + buffered;
}

static void write_filter(FDBSSTWriter *writer, unsigned char *index_position)
{
    size_t size = writer->pending_filter_bytes;
    unsigned char *image = writer->filters + writer->filter_bytes;
    fdb_bloom_init(image, size, writer->limits.bloom_bits_per_key);
    for (size_t i = 0; i < writer->row_count; ++i) {
        const unsigned char *row = writer->block + SST_BLOCK_HEADER_BYTES + writer->row_offsets[i];
        FastDBSlice key = {row + FDB_RECORD_HEADER_BYTES, fdb_decode_u32(row)};
        fdb_bloom_add(image, key);
    }
    fdb_bloom_finish(image, size);
    fdb_encode_u32(index_position, (uint32_t)size);
    writer->filter_bytes += size;
    writer->index_bytes += sizeof(uint32_t);
}

static FastDBStatus flush_block(FDBSSTWriter *writer)
{
    if (writer->row_count == 0)
        return FASTDB_OK;
    unsigned char *block = writer->block;
    unsigned char *offsets = block + SST_BLOCK_HEADER_BYTES + writer->record_bytes;
    for (size_t i = 0; i < writer->row_count; ++i)
        fdb_encode_u32(offsets + i * SST_OFFSET_BYTES, writer->row_offsets[i]);
    size_t size = block_size(writer);
    fdb_encode_u32(block, SST_BLOCK_MAGIC);
    fdb_encode_u32(block + 4, (uint32_t)writer->record_bytes);
    fdb_encode_u32(block + 8, (uint32_t)writer->row_count);
    fdb_encode_u32(block + 12, fdb_crc32c(block + SST_BLOCK_HEADER_BYTES,
                                         size - SST_BLOCK_HEADER_BYTES));

    uint64_t offset = writer->offset;
    FastDBStatus status = append_bytes(writer, block, size);
    if (status != FASTDB_OK)
        return status;
    unsigned char *entry = writer->index + writer->index_bytes;
    size_t key_size = writer->properties.max_key.size;
    fdb_encode_u64(entry, offset);
    fdb_encode_u32(entry + 8, (uint32_t)size);
    fdb_encode_u32(entry + 12, (uint32_t)key_size);
    memcpy(entry + SST_INDEX_ENTRY_BYTES, writer->max_key, key_size);
    writer->index_bytes += SST_INDEX_ENTRY_BYTES + key_size;
    if (writer->filters != NULL)
        write_filter(writer, writer->index + writer->index_bytes);
    ++writer->block_count;
    writer->record_bytes = 0;
    writer->row_count = 0;
    return FASTDB_OK;
}

static FastDBStatus append_index(FDBSSTWriter *writer, unsigned char *image, size_t used,
                                   uint32_t entries)
{
    fdb_encode_u32(image, entries);
    fdb_encode_u32(image + used, fdb_crc32c(image, used));
    return append_bytes(writer, image, used + SST_CRC_BYTES);
}

static FastDBStatus finish_leaf(FDBSSTWriter *writer)
{
    FastDBStatus status = flush_block(writer);
    if (status == FASTDB_OK && writer->filter_bytes != 0)
        status = append_bytes(writer, writer->filters, writer->filter_bytes);
    uint64_t index_offset = writer->offset;
    if (status == FASTDB_OK)
        status = append_index(writer, writer->index, writer->index_bytes, writer->block_count);
    if (status == FASTDB_OK && writer->directory != NULL) {
        unsigned char *entry = writer->directory + writer->directory_bytes;
        FastDBSlice last = writer->properties.max_key;
        fdb_encode_u64(entry, index_offset);
        fdb_encode_u32(entry + 8, (uint32_t)(writer->index_bytes + SST_CRC_BYTES));
        fdb_encode_u32(entry + 12, (uint32_t)last.size);
        memcpy(entry + SST_INDEX_ENTRY_BYTES, last.data, last.size);
        writer->directory_bytes += SST_INDEX_ENTRY_BYTES + last.size;
        ++writer->leaf_count;
        writer->index_bytes = SST_INDEX_HEADER_BYTES;
        writer->block_count = 0;
        writer->filter_bytes = 0;
    }
    return status;
}

static bool room_for_row(const FDBSSTWriter *writer, size_t incoming, bool flush,
                          size_t *filter_size)
{
    *filter_size = 0;
    size_t entry_overhead = SST_INDEX_ENTRY_BYTES;
    if (writer->filters != NULL) {
        entry_overhead += sizeof(uint32_t);
        size_t records = flush ? 1 : writer->row_count + 1;
        size_t available = writer->limits.max_filter_bytes - writer->filter_bytes;
        if (!fdb_bloom_size(records, writer->limits.bloom_bits_per_key, filter_size) ||
            *filter_size > available || (flush && writer->pending_filter_bytes > available - *filter_size))
            return false;
    }
    size_t spare = leaf_capacity(&writer->limits) - writer->index_bytes - SST_CRC_BYTES;
    size_t required;
    if (!fdb_add_size(entry_overhead, incoming, &required) || required > spare)
        return false;
    if (!flush)
        return true;
    spare -= required;
    return spare >= entry_overhead && writer->properties.max_key.size <= spare - entry_overhead;
}

static FastDBStatus admit_row(FDBSSTWriter *writer, size_t key_size, bool flush, size_t *filter_size)
{
    bool fits = room_for_row(writer, key_size, flush, filter_size);
    if (writer->directory == NULL)
        return fits ? FASTDB_OK : FASTDB_LIMIT_EXCEEDED;
    size_t leaf = leaf_capacity(&writer->limits);
    size_t root_room = writer->limits.max_index_bytes - leaf - writer->directory_bytes - SST_CRC_BYTES;
    if (root_room < SST_INDEX_ENTRY_BYTES || key_size > root_room - SST_INDEX_ENTRY_BYTES)
        return FASTDB_LIMIT_EXCEEDED;
    if (fits)
        return FASTDB_OK;

    /* Leave space for both the old leaf's last key and the new leaf's bound. */
    root_room -= SST_INDEX_ENTRY_BYTES + key_size;
    size_t fresh = leaf - SST_INDEX_HEADER_BYTES - SST_CRC_BYTES - SST_INDEX_ENTRY_BYTES;
    if (writer->filters != NULL) {
        if (fresh < sizeof(uint32_t))
            return FASTDB_LIMIT_EXCEEDED;
        fresh -= sizeof(uint32_t);
    }
    if ((writer->block_count == 0 && writer->row_count == 0) || key_size > fresh ||
        root_room < SST_INDEX_ENTRY_BYTES ||
        writer->properties.max_key.size > root_room - SST_INDEX_ENTRY_BYTES)
        return FASTDB_LIMIT_EXCEEDED;
    FastDBStatus status = finish_leaf(writer);
    if (status != FASTDB_OK)
        return status;
    return room_for_row(writer, key_size, false, filter_size) ? FASTDB_OK : FASTDB_LIMIT_EXCEEDED;
}

static void update_properties(FDBSSTWriter *writer, const FDBRecord *record)
{
    FDBSSTProperties *properties = &writer->properties;
    if (properties->records == 0) {
        memcpy(writer->min_key, record->key.data, record->key.size);
        properties->min_key = (FastDBSlice){writer->min_key, record->key.size};
        properties->min_sequence = record->sequence;
    }
    memcpy(writer->max_key, record->key.data, record->key.size);
    properties->max_key = (FastDBSlice){writer->max_key, record->key.size};
    if (record->sequence < properties->min_sequence)
        properties->min_sequence = record->sequence;
    if (record->sequence > properties->max_sequence)
        properties->max_sequence = record->sequence;
    ++properties->records;
    if (record->kind == FDB_DELETE)
        ++properties->tombstones;
    writer->previous_sequence = record->sequence;
}

FastDBStatus fdb_sst_writer_add(FDBSSTWriter *writer, const FDBRecord *record)
{
    if (writer->error != FASTDB_OK)
        return writer->error;
    if (writer->fd < 0)
        return FASTDB_CLOSED;
    size_t record_size;
    FastDBStatus status = fdb_record_size(record, &record_size);
    if (status != FASTDB_OK)
        return status;
    size_t row_size;
    if (record->key.size > writer->limits.max_key_bytes ||
        record->value.size > writer->limits.max_value_bytes ||
        writer->properties.records == UINT64_MAX ||
        !fdb_add_size(record_size, SST_OFFSET_BYTES, &row_size) ||
        row_size > writer->limits.max_block_bytes - SST_BLOCK_HEADER_BYTES)
        return FASTDB_LIMIT_EXCEEDED;
    if (writer->properties.records != 0) {
        int order = fdb_compare(writer->properties.max_key, record->key);
        if (order > 0 || (order == 0 && writer->previous_sequence <= record->sequence))
            return FASTDB_INVALID_ARGUMENT;
    }
    size_t used = block_size(writer);
    size_t target = writer->limits.target_block_bytes;
    bool flush = writer->row_count != 0 && (used > target || row_size > target - used);
    size_t filter_size;
    status = admit_row(writer, record->key.size, flush, &filter_size);
    if (status != FASTDB_OK)
        return status;
    if (flush) {
        status = flush_block(writer);
        if (status != FASTDB_OK)
            return status;
    }
    unsigned char *location = writer->block + SST_BLOCK_HEADER_BYTES + writer->record_bytes;
    size_t encoded;
    status = fdb_record_encode(record, location, record_size, &encoded);
    if (status != FASTDB_OK)
        return status;
    writer->row_offsets[writer->row_count++] = (uint32_t)writer->record_bytes;
    writer->record_bytes += encoded;
    writer->pending_filter_bytes = filter_size;
    update_properties(writer, record);
    return FASTDB_OK;
}

static size_t write_properties(unsigned char *bytes, const FDBSSTProperties *properties)
{
    fdb_encode_u32(bytes, (uint32_t)properties->min_key.size);
    fdb_encode_u32(bytes + 4, (uint32_t)properties->max_key.size);
    fdb_encode_u64(bytes + 8, properties->records);
    fdb_encode_u64(bytes + 16, properties->tombstones);
    fdb_encode_u64(bytes + 24, properties->min_sequence);
    fdb_encode_u64(bytes + 32, properties->max_sequence);
    fdb_encode_u32(bytes + 40, 0);
    fdb_encode_u32(bytes + 44, 0);
    if (properties->min_key.size != 0)
        memcpy(bytes + SST_PROPERTIES_HEADER_BYTES,
               properties->min_key.data, properties->min_key.size);
    size_t used = SST_PROPERTIES_HEADER_BYTES + properties->min_key.size;
    if (properties->max_key.size != 0)
        memcpy(bytes + used, properties->max_key.data, properties->max_key.size);
    used += properties->max_key.size;
    fdb_encode_u32(bytes + used, fdb_crc32c(bytes, used));
    return used + SST_CRC_BYTES;
}

static FastDBStatus finish_image(FDBSSTWriter *writer, unsigned char *properties)
{
    FastDBStatus status = FASTDB_OK;
    uint64_t index_offset;
    size_t index_size;
    if (writer->directory != NULL) {
        if (writer->row_count != 0 || writer->block_count != 0)
            status = finish_leaf(writer);
        if (status != FASTDB_OK)
            return status;
        index_offset = writer->offset;
        index_size = writer->directory_bytes + SST_CRC_BYTES;
        status = append_index(writer, writer->directory, writer->directory_bytes, writer->leaf_count);
    } else {
        status = finish_leaf(writer);
        if (status != FASTDB_OK)
            return status;
        index_size = writer->index_bytes + SST_CRC_BYTES;
        index_offset = writer->offset - index_size;
    }
    if (status != FASTDB_OK)
        return status;
    uint64_t properties_offset = writer->offset;
    size_t properties_size = write_properties(properties, &writer->properties);
    status = append_bytes(writer, properties, properties_size);
    if (status != FASTDB_OK)
        return status;

    unsigned char footer[SST_FOOTER_BYTES] = {0};
    memcpy(footer, writer->directory == NULL ? SST_FOOTER_MAGIC : SST_PARTITIONED_FOOTER_MAGIC, 8);
    fdb_encode_u64(footer + 8, index_offset);
    fdb_encode_u64(footer + 16, index_size);
    fdb_encode_u64(footer + 24, properties_offset);
    fdb_encode_u64(footer + 32, properties_size);
    fdb_encode_u64(footer + 40, writer->properties.id);
    if (writer->filters != NULL)
        fdb_encode_u32(footer + 48, SST_FEATURE_BLOOM);
    fdb_encode_u32(footer + 52, fdb_crc32c(footer, 52));
    return append_bytes(writer, footer, sizeof(footer));
}

static FastDBStatus finish(FDBSSTWriter *writer, FDBSSTProperties *out, bool allow_empty)
{
    *out = (FDBSSTProperties){0};
    if (writer->error != FASTDB_OK)
        return writer->error;
    if (writer->fd < 0)
        return FASTDB_CLOSED;
    if (writer->properties.records == 0) {
        if (!allow_empty)
            return FASTDB_INVALID_ARGUMENT;
        writer->properties.min_sequence = 0;
    }
    size_t capacity;
    if (!fdb_add_size(SST_PROPERTIES_HEADER_BYTES + SST_CRC_BYTES, writer->properties.min_key.size,
                      &capacity) || !fdb_add_size(capacity, writer->properties.max_key.size, &capacity))
        return FASTDB_LIMIT_EXCEEDED;
    unsigned char *properties = malloc(capacity);
    if (properties == NULL)
        return FASTDB_OUT_OF_MEMORY;
    FastDBStatus status = finish_image(writer, properties);
    free(properties);
    if (status == FASTDB_OK)
        status = fdb_fs_sync(&writer->fs, writer->fd);
    FastDBStatus close_status = fdb_fs_close(writer->fd);
    writer->fd = -1;
    if (status == FASTDB_OK)
        status = close_status;
    if (status != FASTDB_OK) {
        writer->error = status;
        return status;
    }
    writer->properties.file_bytes = writer->offset;
    *out = writer->properties;
    return FASTDB_OK;
}

FastDBStatus fdb_sst_writer_finish(FDBSSTWriter *writer, FDBSSTProperties *out)
{
    return finish(writer, out, false);
}

FastDBStatus fdb_sst_writer_finish_checkpoint(FDBSSTWriter *writer, FDBSSTProperties *out)
{
    return finish(writer, out, true);
}
