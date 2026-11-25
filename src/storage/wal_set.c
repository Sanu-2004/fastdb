#include "wal_set.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WAL_NAME_BYTES = sizeof("wal-18446744073709551615") };

static void wal_name(uint64_t id, char name[WAL_NAME_BYTES])
{
    (void)snprintf(name, WAL_NAME_BYTES, "wal-%020" PRIu64, id);
}

static FastDBStatus replay_segments(FDBWalSet *set, const FDBWalReplay *replay)
{
    const FDBManifest *view = &set->catalog.view;
    size_t last_wal = SIZE_MAX;
    for (size_t i = 0; i < view->count; ++i) {
        if (view->entries[i].kind == FDB_FILE_WAL)
            last_wal = i;
    }
    for (size_t i = 0; i < view->count; ++i) {
        const FDBManifestEntry *entry = &view->entries[i];
        if (entry->kind != FDB_FILE_WAL)
            continue;
        if (entry->sequence_begin != set->wal.sequence || entry->bytes < FDB_WAL_HEADER_BYTES)
            return FASTDB_CORRUPTION;
        bool active = entry->sequence_end == 0;
        if (active && (i != last_wal || entry->bytes != FDB_WAL_HEADER_BYTES))
            return FASTDB_CORRUPTION;

        char name[WAL_NAME_BYTES];
        wal_name(entry->id, name);
        FDBWal wal;
        FastDBStatus status = fdb_wal_open(
            set->dir, name, entry->id, entry->sequence_begin, &set->wal.limits,
            active ? FDB_WAL_REPAIR_TAIL : FDB_WAL_SEALED, replay, &wal);
        if (status != FASTDB_OK)
            return status;
        if (active) {
            set->wal = wal;
            set->active_index = i;
        } else {
            bool correct = wal.offset == entry->bytes && wal.sequence == entry->sequence_end;
            set->wal.sequence = wal.sequence;
            status = fdb_wal_close(&wal);
            if (status != FASTDB_OK)
                return status;
            if (!correct)
                return FASTDB_CORRUPTION;
        }
    }
    return set->wal.sequence >= view->sequence ? FASTDB_OK : FASTDB_CORRUPTION;
}

FastDBStatus fdb_wal_set_open(const FDBDirectory *dir, const FDBManifestLimits *catalog_limits,
                              const FDBWalLimits *wal_limits, uint64_t covered_sequence,
                              const FDBWalReplay *replay, FDBWalSet *out)
{
    *out = (FDBWalSet){.wal.fd = -1, .active_index = SIZE_MAX};
    if (!fdb_wal_valid_limits(wal_limits))
        return FASTDB_INVALID_ARGUMENT;
    FastDBStatus status = fdb_manifest_load(dir, catalog_limits, &out->catalog);
    if (status != FASTDB_OK)
        return status;
    out->dir = dir;
    out->catalog_limits = *catalog_limits;
    out->wal.limits = *wal_limits;
    out->wal.sequence = covered_sequence;
    out->wal.offset = FDB_WAL_HEADER_BYTES;
    status = covered_sequence > out->catalog.view.sequence ?
             FASTDB_CORRUPTION : replay_segments(out, replay);
    if (status != FASTDB_OK)
        (void)fdb_wal_set_close(out);
    return status;
}

FastDBStatus fdb_wal_set_sync(FDBWalSet *set)
{
    if (set->poisoned)
        return FASTDB_COMMIT_UNCERTAIN;
    if (set->active_index == SIZE_MAX)
        return FASTDB_OK;
    FastDBStatus status = fdb_wal_sync(&set->wal);
    if (status != FASTDB_OK)
        set->poisoned = true;
    return status;
}

static FastDBStatus publish_rotation(FDBWalSet *set, FDBManifestEntry *entries)
{
    FDBManifest *current = &set->catalog.view;
    uint64_t id;
    FastDBStatus status = fdb_manifest_reserve_file(set->dir, &set->catalog_limits, current, &id);
    if (status != FASTDB_OK)
        return status;

    char name[WAL_NAME_BYTES];
    wal_name(id, name);
    FDBWal new_wal;
    status = fdb_wal_create(set->dir, name, id, set->wal.sequence, &set->wal.limits, &new_wal);
    if (status != FASTDB_OK)
        return status;

    if (set->active_index != SIZE_MAX) {
        FDBManifestEntry *old_entry = &entries[set->active_index];
        old_entry->bytes = set->wal.offset;
        old_entry->sequence_end = set->wal.sequence;
    }
    entries[current->count] = (FDBManifestEntry){
        .id = id, .kind = FDB_FILE_WAL, .bytes = FDB_WAL_HEADER_BYTES,
        .sequence_begin = set->wal.sequence,
    };
    FDBManifest next = *current;
    ++next.generation;
    next.sequence = new_wal.sequence;
    next.entries = entries;
    ++next.count;
    status = fdb_manifest_publish(set->dir, &set->catalog_limits, current->generation, &next);
    if (status != FASTDB_OK) {
        (void)fdb_wal_close(&new_wal);
        return status;
    }

    FastDBStatus close_status = set->wal.fd < 0 ? FASTDB_OK : fdb_wal_close(&set->wal);
    set->wal = new_wal;
    set->active_index = current->count;
    free(set->catalog.entries);
    set->catalog.entries = entries;
    *current = next;
    if (close_status != FASTDB_OK)
        set->poisoned = true;
    return close_status;
}

FastDBStatus fdb_wal_set_rotate(FDBWalSet *set)
{
    if (set->poisoned)
        return FASTDB_COMMIT_UNCERTAIN;
    if (set->active_index != SIZE_MAX && set->wal.offset == FDB_WAL_HEADER_BYTES)
        return FASTDB_OK;
    const FDBManifest *view = &set->catalog.view;
    if (view->count >= set->catalog_limits.max_files ||
        view->count >= SIZE_MAX / sizeof(FDBManifestEntry) ||
        view->generation >= UINT64_MAX - 1)
        return FASTDB_LIMIT_EXCEEDED;
    FDBManifestEntry *entries = malloc((view->count + 1) * sizeof(*entries));
    if (entries == NULL)
        return FASTDB_OUT_OF_MEMORY;
    if (view->count != 0)
        memcpy(entries, view->entries, view->count * sizeof(*entries));
    FastDBStatus status = fdb_wal_set_sync(set);
    if (status == FASTDB_OK)
        status = publish_rotation(set, entries);
    if (set->catalog.entries != entries)
        free(entries);
    if (status == FASTDB_COMMIT_UNCERTAIN)
        set->poisoned = true;
    return status;
}

FastDBStatus fdb_wal_set_append(FDBWalSet *set, const FDBRecord *records, size_t count, bool durable)
{
    if (set->poisoned)
        return FASTDB_COMMIT_UNCERTAIN;
    size_t frame_bytes;
    FastDBStatus status = fdb_wal_frame_size(&set->wal, records, count, &frame_bytes);
    if (status != FASTDB_OK)
        return status;
    if (set->active_index == SIZE_MAX ||
        frame_bytes > set->wal.limits.max_segment_bytes - set->wal.offset) {
        status = fdb_wal_set_rotate(set);
        if (status != FASTDB_OK)
            return status;
    }
    status = fdb_wal_append(&set->wal, records, count, durable);
    if (status == FASTDB_COMMIT_UNCERTAIN)
        set->poisoned = true;
    return status;
}

FastDBStatus fdb_wal_set_install_catalog(FDBWalSet *set, FDBManifestLoad *next)
{
    FastDBStatus status = FASTDB_OK;
    if (set->active_index != SIZE_MAX) {
        uint64_t id = set->catalog.view.entries[set->active_index].id;
        set->active_index = SIZE_MAX;
        for (size_t i = 0; i < next->view.count; ++i) {
            if (next->view.entries[i].id == id) {
                set->active_index = i;
                break;
            }
        }
        if (set->active_index == SIZE_MAX) {
            FDBWal previous = set->wal;
            status = fdb_wal_close(&set->wal);
            set->wal = (FDBWal){.fd = -1, .limits = previous.limits, .sequence = previous.sequence,
                                .offset = FDB_WAL_HEADER_BYTES};
        }
    }
    fdb_manifest_release(&set->catalog);
    set->catalog = *next;
    *next = (FDBManifestLoad){0};
    return status;
}

FastDBStatus fdb_wal_set_close(FDBWalSet *set)
{
    FastDBStatus status = set->wal.fd < 0 ? FASTDB_OK : fdb_wal_close(&set->wal);
    if (status == FASTDB_OK && set->poisoned)
        status = FASTDB_COMMIT_UNCERTAIN;
    fdb_manifest_release(&set->catalog);
    *set = (FDBWalSet){.wal.fd = -1, .active_index = SIZE_MAX};
    return status;
}
