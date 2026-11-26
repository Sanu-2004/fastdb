#include "disk.h"

#include <stdlib.h>
#include <time.h>

struct FastDBSnapshot {
    FastDB *db;
    FDBTree tree;
    uint64_t revision;
    struct timespec read_time;
};

static FastDBStatus capture(FastDB *db, FastDBSnapshot **out)
{
    if (fdb_disk_poisoned(db->disk))
        return FASTDB_COMMIT_UNCERTAIN;
    if (fdb_disk_has_tables(db->disk))
        return FASTDB_UNSUPPORTED;
    size_t headroom = db->limits.max_memory_bytes - db->disk_budget - fdb_memory_bytes(db);
    size_t bytes;
    if (db->snapshot_bytes != 0 || !fdb_add_size(sizeof(FastDBSnapshot), db->tree.bytes, &bytes) ||
        bytes > headroom)
        return FASTDB_LIMIT_EXCEEDED;

    struct timespec read_time;
    if (timespec_get(&read_time, TIME_UTC) != TIME_UTC)
        return FASTDB_IO_ERROR;
    FastDBSnapshot *snapshot = malloc(sizeof(*snapshot));
    if (snapshot == NULL)
        return FASTDB_OUT_OF_MEMORY;
    *snapshot = (FastDBSnapshot){.db = db, .revision = db->sequence, .read_time = read_time};
    FastDBStatus status = fdb_tree_clone(&db->tree, &snapshot->tree);
    if (status != FASTDB_OK) {
        free(snapshot);
        return status;
    }
    db->snapshot_bytes = bytes;
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

static bool expired(uint64_t expiration, const struct timespec *read_time)
{
    if (expiration == 0 || read_time->tv_sec < 0)
        return false;
    uint64_t seconds = (uint64_t)read_time->tv_sec;
    return seconds > expiration / 1000 ||
           (seconds == expiration / 1000 &&
            (uint64_t)(read_time->tv_nsec / 1000000) >= expiration % 1000);
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
    const FDBNode *node = fdb_tree_get(&snapshot->tree, slice);
    if (node == NULL || node->record.kind == FDB_DELETE ||
        expired(node->record.expires_at_ms, &snapshot->read_time))
        return FASTDB_NOT_FOUND;

    FDBLookup found = {0};
    status = fdb_lookup_record(&node->record, true, &found);
    if (status == FASTDB_OK) {
        *value = found.value;
        *value_size = found.size;
        *revision = found.revision;
    }
    return status;
}

void fastdb_snapshot_release(FastDBSnapshot *snapshot)
{
    if (snapshot == NULL)
        return;
    FastDB *db = snapshot->db;
    pthread_mutex_lock(&db->mutex);
    fdb_tree_destroy(&snapshot->tree);
    free(snapshot);
    db->snapshot_bytes = 0;
    pthread_mutex_unlock(&db->mutex);
}
