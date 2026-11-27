#include "disk.h"

#include <stdlib.h>

static FastDBStatus capture(FastDB *db, FastDBSnapshot **out)
{
    if (fdb_disk_poisoned(db->disk))
        return FASTDB_COMMIT_UNCERTAIN;
    size_t headroom = db->limits.max_memory_bytes - db->reserved_bytes - fdb_memory_bytes(db);
    size_t frozen_bytes = db->flush == NULL ? 0 : db->flush->frozen.bytes;
    size_t bytes;
    if (db->snapshot_bytes != 0 || !fdb_add_size(sizeof(FastDBSnapshot), db->tree.bytes, &bytes) ||
        !fdb_add_size(bytes, frozen_bytes, &bytes) || bytes > headroom)
        return FASTDB_LIMIT_EXCEEDED;

    uint64_t read_time;
    FastDBStatus status = fdb_read_time(&read_time);
    if (status != FASTDB_OK)
        return status;
    const size_t catalog_budget = headroom - bytes;
    size_t remaining = catalog_budget;
    FDBManifestLoad catalog = {0};
    status = fdb_disk_copy_tables(db->disk, &catalog, &remaining);
    if (status != FASTDB_OK)
        return status;
    FastDBSnapshot *snapshot = malloc(sizeof(*snapshot));
    if (snapshot == NULL) {
        fdb_manifest_release(&catalog);
        return FASTDB_OUT_OF_MEMORY;
    }
    *snapshot = (FastDBSnapshot){.db = db, .catalog = catalog,
                                 .revision = db->sequence, .read_time = read_time};
    status = fdb_tree_clone(&db->tree, &snapshot->active);
    if (status == FASTDB_OK && db->flush != NULL)
        status = fdb_tree_clone(&db->flush->frozen, &snapshot->frozen);
    if (status != FASTDB_OK) {
        fdb_tree_destroy(&snapshot->active);
        fdb_manifest_release(&snapshot->catalog);
        free(snapshot);
        return status;
    }
    if (catalog.view.count != 0)
        fdb_disk_snapshot_catalog(db->disk, &snapshot->catalog.view);
    db->snapshot_bytes = bytes + (catalog_budget - remaining);
    *out = snapshot;
    return FASTDB_OK;
}

FastDBStatus fastdb_snapshot_create(FastDB *db, FastDBSnapshot **out)
{
    if (out == NULL)
        return FASTDB_INVALID_ARGUMENT;
    *out = NULL;
    if (db == NULL)
        return FASTDB_INVALID_ARGUMENT;
    pthread_mutex_lock(&db->mutex);
    FastDBStatus status = capture(db, out);
    pthread_mutex_unlock(&db->mutex);
    return status;
}

uint64_t fastdb_snapshot_revision(const FastDBSnapshot *snapshot)
{
    return snapshot == NULL ? 0 : snapshot->revision;
}

FastDBStatus fastdb_snapshot_get(const FastDBSnapshot *snapshot, const void *key, size_t key_size,
                                  void **value, size_t *value_size, uint64_t *revision)
{
    if (value != NULL)
        *value = NULL;
    if (value_size != NULL)
        *value_size = 0;
    if (revision != NULL)
        *revision = 0;
    if (snapshot == NULL || value == NULL || value_size == NULL || revision == NULL)
        return FASTDB_INVALID_ARGUMENT;
    FastDBSlice slice = {key, key_size};
    FastDBStatus status = fdb_validate_key(&snapshot->db->limits, slice);
    if (status != FASTDB_OK)
        return status;
    const FDBNode *node = fdb_tree_get(&snapshot->active, slice);
    if (node == NULL)
        node = fdb_tree_get(&snapshot->frozen, slice);
    FDBLookup found = {0};
    if (node != NULL) {
        if (!fdb_record_visible(node->record.kind, node->record.expires_at_ms, snapshot->read_time))
            return FASTDB_NOT_FOUND;
        status = fdb_lookup_record(&node->record, true, &found);
    } else if (snapshot->catalog.view.count != 0) {
        FastDB *db = snapshot->db;
        pthread_mutex_lock(&db->mutex);
        status = fdb_disk_find_view(db->disk, &snapshot->catalog.view, slice,
                                     snapshot->revision, true, &found);
        pthread_mutex_unlock(&db->mutex);
    }
    if (status != FASTDB_OK || found.revision == 0 ||
        !fdb_record_visible(found.kind, found.expires_at_ms, snapshot->read_time)) {
        free(found.value);
        return status == FASTDB_OK ? FASTDB_NOT_FOUND : status;
    }
    *value = found.value;
    *value_size = found.size;
    *revision = found.revision;
    return FASTDB_OK;
}

void fastdb_snapshot_release(FastDBSnapshot *snapshot)
{
    if (snapshot == NULL)
        return;
    FastDB *db = snapshot->db;
    pthread_mutex_lock(&db->mutex);
    fdb_disk_snapshot_catalog(db->disk, NULL);
    fdb_manifest_release(&snapshot->catalog);
    fdb_tree_destroy(&snapshot->active);
    fdb_tree_destroy(&snapshot->frozen);
    free(snapshot);
    db->snapshot_bytes = 0;
    if (db->flush != NULL)
        pthread_cond_signal(&db->flush->condition);
    pthread_mutex_unlock(&db->mutex);
}
