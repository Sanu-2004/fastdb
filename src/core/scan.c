#include "disk.h"

#include <stdlib.h>
#include <string.h>

typedef struct ScanBounds {
    FastDBRange range;
    FastDBSlice prefix;
} ScanBounds;

typedef struct ScanView {
    const FDBTree *active;
    const FDBTree *frozen;
    const FDBManifest *catalog;
    uint64_t read_time;
} ScanView;

typedef struct PageLayout {
    size_t bytes;
    size_t count;
    bool more;
} PageLayout;

static bool matches(FastDBSlice key, const ScanBounds *bounds)
{
    if (key.size == 0)
        return false;
    if (bounds->prefix.size != 0)
        return key.size >= bounds->prefix.size &&
               memcmp(key.data, bounds->prefix.data, bounds->prefix.size) == 0;
    return bounds->range.end.size == 0 || fdb_compare(key, bounds->range.end) < 0;
}

static bool has_scan_tables(const FDBManifest *catalog)
{
    if (catalog != NULL) {
        for (size_t i = 0; i < catalog->count; ++i)
            if (catalog->entries[i].kind == FDB_FILE_SSTABLE)
                return true;
    }
    return false;
}

static void pick_node(const FDBTree *tree, FastDBSlice lower, bool exclusive,
                      const ScanBounds *bounds, FDBScanCandidate *candidate)
{
    if (tree == NULL)
        return;
    const FDBNode *node = fdb_tree_bound(tree, lower, exclusive);
    if (node == NULL || !matches(node->record.key, bounds))
        return;
    int order = candidate->record.key.size == 0 ? -1 :
                fdb_compare(node->record.key, candidate->record.key);
    if (order < 0 || (order == 0 && node->record.sequence > candidate->record.sequence))
        candidate->record = node->record;
}

static FastDBStatus copy_row(FastDB *db, const FDBScanCandidate *candidate, FDBScanRow *row,
                              unsigned char **payload)
{
    const FDBRecord *record = &candidate->record;
    unsigned char *key = *payload;
    unsigned char *value = key + record->key.size;
    memcpy(key, record->key.data, record->key.size);
    if (record->value.size != 0) {
        if (candidate->file != NULL) {
            FastDBStatus status = fdb_disk_scan_copy(db->disk, candidate, value);
            if (status != FASTDB_OK)
                return status;
        } else {
            memcpy(value, record->value.data, record->value.size);
        }
    }
    *row = (FDBScanRow){{key, record->key.size}, {value, record->value.size}, record->sequence};
    *payload = value + record->value.size;
    return FASTDB_OK;
}

/* The sizing pass and the copy pass share one locked view and one reader workspace. */
static FastDBStatus collect_page(FastDB *db, const ScanView *view, const ScanBounds *bounds,
                                  size_t limit, size_t budget, unsigned char *scratch,
                                  FastDBIterator *page, PageLayout *layout)
{
    *layout = (PageLayout){.bytes = sizeof(FastDBIterator)};
    FastDBSlice lower = bounds->range.start;
    bool exclusive = bounds->range.start_exclusive;
    size_t buffer = 0;
    unsigned char *payload = page == NULL ? NULL : (unsigned char *)(page->rows + page->count);
    for (;;) {
        FDBScanCandidate candidate = {0};
        pick_node(view->active, lower, exclusive, bounds, &candidate);
        pick_node(view->frozen, lower, exclusive, bounds, &candidate);
        if (scratch != NULL) {
            FastDBStatus status = fdb_disk_scan_pick(db->disk, view->catalog, lower, exclusive,
                bounds->range.end, bounds->prefix,
                scratch + buffer * db->limits.max_key_bytes, &candidate);
            if (status != FASTDB_OK)
                return status;
        }
        const FDBRecord *record = &candidate.record;
        if (!matches(record->key, bounds))
            return FASTDB_OK;
        lower = record->key;
        exclusive = true;
        buffer ^= 1;
        if (!fdb_record_visible(record->kind, record->expires_at_ms, view->read_time))
            continue;
        if (layout->count == limit) {
            layout->more = true;
            return FASTDB_OK;
        }

        size_t row_bytes;
        if (!fdb_add_size(sizeof(FDBScanRow), record->key.size, &row_bytes) ||
            !fdb_add_size(row_bytes, record->value.size, &row_bytes) ||
            row_bytes > budget - layout->bytes)
            return page == NULL ? FASTDB_LIMIT_EXCEEDED : FASTDB_CORRUPTION;
        if (page != NULL) {
            if (layout->count >= page->count)
                return FASTDB_CORRUPTION;
            FastDBStatus status = copy_row(db, &candidate, &page->rows[layout->count], &payload);
            if (status != FASTDB_OK)
                return status;
        }
        layout->bytes += row_bytes;
        ++layout->count;
    }
}

static FastDBStatus build_page(FastDB *db, const ScanView *view, const ScanBounds *bounds,
                                size_t limit, FastDBIterator **out)
{
    if (db->iterator_count == db->limits.max_open_iterators)
        return FASTDB_LIMIT_EXCEEDED;
    size_t memory = db->limits.max_memory_bytes - db->reserved_bytes - fdb_memory_bytes(db);
    if (memory < sizeof(FastDBIterator))
        return FASTDB_LIMIT_EXCEEDED;
    unsigned char *scratch = NULL;
    if (has_scan_tables(view->catalog)) {
        if (db->limits.max_key_bytes > (memory - sizeof(FastDBIterator)) / 2)
            return FASTDB_LIMIT_EXCEEDED;
        size_t scratch_bytes = 2 * db->limits.max_key_bytes;
        scratch = malloc(scratch_bytes);
        if (scratch == NULL)
            return FASTDB_OUT_OF_MEMORY;
        memory -= scratch_bytes;
    }
    size_t budget = db->limits.max_scan_bytes < memory ? db->limits.max_scan_bytes : memory;
    PageLayout layout;
    FastDBStatus status = collect_page(db, view, bounds, limit, budget, scratch, NULL, &layout);
    FastDBIterator *page = NULL;
    if (status == FASTDB_OK) {
        page = malloc(layout.bytes);
        if (page == NULL)
            status = FASTDB_OUT_OF_MEMORY;
    }
    if (status == FASTDB_OK) {
        *page = (FastDBIterator){.db = db, .bytes = layout.bytes,
                                 .count = layout.count, .more = layout.more};
        PageLayout copied;
        status = collect_page(db, view, bounds, limit, page->bytes, scratch, page, &copied);
        if (status == FASTDB_OK && (copied.count != layout.count || copied.bytes != layout.bytes ||
                                    copied.more != layout.more))
            status = FASTDB_CORRUPTION;
    }
    free(scratch);
    if (status != FASTDB_OK) {
        free(page);
        return status;
    }
    db->iterator_bytes += layout.bytes;
    ++db->iterator_count;
    *out = page;
    return FASTDB_OK;
}

static FastDBStatus scan(FastDB *db, const FastDBSnapshot *snapshot, const ScanBounds *bounds,
                          size_t limit, FastDBIterator **out)
{
    if (limit == 0)
        return FASTDB_INVALID_ARGUMENT;
    if (limit > db->limits.max_scan_records)
        return FASTDB_LIMIT_EXCEEDED;
    pthread_mutex_lock(&db->mutex);
    FastDBStatus status = FASTDB_OK;
    ScanView view;
    if (snapshot != NULL) {
        view = (ScanView){&snapshot->active, &snapshot->frozen,
                          &snapshot->catalog.view, snapshot->read_time};
    } else if (fdb_disk_poisoned(db->disk)) {
        status = FASTDB_COMMIT_UNCERTAIN;
    } else {
        view = (ScanView){.active = &db->tree, .catalog = fdb_disk_catalog(db->disk)};
        if (db->flush != NULL)
            view.frozen = &db->flush->frozen;
        status = fdb_read_time(&view.read_time);
    }
    if (status == FASTDB_OK) {
        if (bounds->range.end.size != 0 && fdb_compare(bounds->range.start, bounds->range.end) == 0)
            view.catalog = NULL;
        status = build_page(db, &view, bounds, limit, out);
    }
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

static FastDBStatus scan_range(FastDB *db, const FastDBSnapshot *snapshot, const FastDBRange *range,
                                size_t limit, FastDBIterator **out)
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
    return scan(db, snapshot, &bounds, limit, out);
}

static FastDBStatus scan_prefix(FastDB *db, const FastDBSnapshot *snapshot, FastDBSlice prefix,
                                 size_t limit, FastDBIterator **out)
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
    return scan(db, snapshot, &bounds, limit, out);
}

FastDBStatus fastdb_scan_range(FastDB *db, const FastDBRange *range, size_t limit,
                                FastDBIterator **out)
{
    return scan_range(db, NULL, range, limit, out);
}

FastDBStatus fastdb_scan_prefix(FastDB *db, FastDBSlice prefix, size_t limit, FastDBIterator **out)
{
    return scan_prefix(db, NULL, prefix, limit, out);
}

FastDBStatus fastdb_snapshot_scan_range(const FastDBSnapshot *snapshot, const FastDBRange *range,
                                         size_t limit, FastDBIterator **out)
{
    return scan_range(snapshot == NULL ? NULL : snapshot->db, snapshot, range, limit, out);
}

FastDBStatus fastdb_snapshot_scan_prefix(const FastDBSnapshot *snapshot, FastDBSlice prefix,
                                          size_t limit, FastDBIterator **out)
{
    return scan_prefix(snapshot == NULL ? NULL : snapshot->db, snapshot, prefix, limit, out);
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
