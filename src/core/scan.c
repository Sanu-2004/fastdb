#include "disk.h"

#include <stdlib.h>
#include <string.h>

typedef struct ScanBounds {
    FastDBRange range;
    FastDBSlice prefix;
} ScanBounds;

static bool matches(const FDBNode *node, const ScanBounds *bounds)
{
    if (node == NULL)
        return false;
    FastDBSlice key = node->record.key;
    if (bounds->prefix.size != 0)
        return key.size >= bounds->prefix.size &&
               memcmp(key.data, bounds->prefix.data, bounds->prefix.size) == 0;
    return bounds->range.end.size == 0 || fdb_compare(key, bounds->range.end) < 0;
}

static const FDBNode *next_node(const FastDB *db, const FDBNode *node)
{
    return fdb_tree_bound(&db->tree, node->record.key, true);
}

static FastDBStatus build_page(FastDB *db, const ScanBounds *bounds, size_t limit,
                                FastDBIterator **out)
{
    if (db->iterator_count == db->limits.max_open_iterators)
        return FASTDB_LIMIT_EXCEEDED;
    size_t memory = db->limits.max_memory_bytes - db->disk_budget - fdb_memory_bytes(db);
    size_t budget = db->limits.max_scan_bytes < memory ? db->limits.max_scan_bytes : memory;
    size_t bytes = sizeof(FastDBIterator);
    if (bytes > budget)
        return FASTDB_LIMIT_EXCEEDED;

    const FDBNode *first = fdb_tree_bound(&db->tree, bounds->range.start,
                                          bounds->range.start_exclusive);
    const FDBNode *node = first;
    size_t count = 0;
    while (count < limit && matches(node, bounds)) {
        size_t row_bytes;
        if (!fdb_add_size(sizeof(FDBScanRow), node->record.key.size, &row_bytes) ||
            !fdb_add_size(row_bytes, node->record.value.size, &row_bytes) ||
            row_bytes > budget - bytes)
            return FASTDB_LIMIT_EXCEEDED;
        bytes += row_bytes;
        ++count;
        node = next_node(db, node);
    }

    FastDBIterator *iterator = malloc(bytes);
    if (iterator == NULL)
        return FASTDB_OUT_OF_MEMORY;
    *iterator = (FastDBIterator){.db = db, .bytes = bytes, .count = count,
                                 .more = matches(node, bounds)};
    unsigned char *payload = (unsigned char *)(iterator->rows + count);
    node = first;
    for (size_t i = 0; i < count; ++i) {
        size_t key_size = node->record.key.size;
        size_t value_size = node->record.value.size;
        memcpy(payload, node->record.key.data, key_size);
        if (value_size != 0)
            memcpy(payload + key_size, node->record.value.data, value_size);
        iterator->rows[i] = (FDBScanRow){{payload, key_size}, {payload + key_size, value_size},
                                         node->record.sequence};
        payload += key_size + value_size;
        node = next_node(db, node);
    }
    db->iterator_bytes += bytes;
    ++db->iterator_count;
    *out = iterator;
    return FASTDB_OK;
}

static FastDBStatus scan(FastDB *db, const ScanBounds *bounds, size_t limit,
                          FastDBIterator **out)
{
    if (limit == 0)
        return FASTDB_INVALID_ARGUMENT;
    if (limit > db->limits.max_scan_records)
        return FASTDB_LIMIT_EXCEEDED;
    pthread_mutex_lock(&db->mutex);
    FastDBStatus status = FASTDB_OK;
    if (fdb_disk_poisoned(db->disk))
        status = FASTDB_COMMIT_UNCERTAIN;
    else if (fdb_disk_has_tables(db->disk))
        status = FASTDB_UNSUPPORTED;
    else
        status = build_page(db, bounds, limit, out);
    pthread_mutex_unlock(&db->mutex);
    return status;
}

static FastDBStatus validate_bound(const FastDBLimits *limits, FastDBSlice bound)
{
    if (bound.data == NULL && bound.size == 0)
        return FASTDB_OK;
    return fdb_validate_key(limits, bound);
}

FastDBStatus fdb_validate_range(const FastDBLimits *limits, const FastDBRange *range)
{
    FastDBStatus status = validate_bound(limits, range->start);
    if (status == FASTDB_OK)
        status = validate_bound(limits, range->end);
    if (status == FASTDB_OK && range->end.size != 0 && fdb_compare(range->start, range->end) > 0)
        return FASTDB_INVALID_ARGUMENT;
    return status;
}

FastDBStatus fastdb_scan_range(FastDB *db, const FastDBRange *range, size_t limit,
                                FastDBIterator **out)
{
    if (out == NULL)
        return FASTDB_INVALID_ARGUMENT;
    *out = NULL;
    if (db == NULL || range == NULL)
        return FASTDB_INVALID_ARGUMENT;
    FastDBStatus status = fdb_validate_range(&db->limits, range);
    if (status != FASTDB_OK)
        return status;
    const ScanBounds bounds = {.range = *range};
    return scan(db, &bounds, limit, out);
}

FastDBStatus fastdb_scan_prefix(FastDB *db, FastDBSlice prefix, size_t limit,
                                 FastDBIterator **out)
{
    if (out == NULL)
        return FASTDB_INVALID_ARGUMENT;
    *out = NULL;
    if (db == NULL)
        return FASTDB_INVALID_ARGUMENT;
    if (prefix.size != 0) {
        FastDBStatus status = fdb_validate_key(&db->limits, prefix);
        if (status != FASTDB_OK)
            return status;
    }
    const ScanBounds bounds = {.range.start = prefix, .prefix = prefix};
    return scan(db, &bounds, limit, out);
}

FastDBStatus fastdb_iterator_next(FastDBIterator *iterator, FastDBSlice *key,
                                   FastDBSlice *value, uint64_t *revision)
{
    if (key != NULL)
        *key = (FastDBSlice){0};
    if (value != NULL)
        *value = (FastDBSlice){0};
    if (revision != NULL)
        *revision = 0;
    if (iterator == NULL || key == NULL || value == NULL || revision == NULL)
        return FASTDB_INVALID_ARGUMENT;
    if (iterator->cursor == iterator->count)
        return FASTDB_NOT_FOUND;
    const FDBScanRow *row = &iterator->rows[iterator->cursor++];
    *key = row->key;
    *value = row->value;
    *revision = row->sequence;
    return FASTDB_OK;
}

bool fastdb_iterator_has_more(const FastDBIterator *iterator)
{
    return iterator != NULL && iterator->more;
}

void fastdb_iterator_close(FastDBIterator *iterator)
{
    if (iterator == NULL)
        return;
    FastDB *db = iterator->db;
    pthread_mutex_lock(&db->mutex);
    db->iterator_bytes -= iterator->bytes;
    --db->iterator_count;
    free(iterator);
    pthread_mutex_unlock(&db->mutex);
}
