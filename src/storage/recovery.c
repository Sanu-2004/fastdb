#include "recovery.h"
#include "compaction_plan.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

FastDBStatus fdb_catalog_open_table(const FDBDirectory *dir, const FDBManifestEntry *entry,
                                    const FDBSSTLimits *limits, FDBSSTReader **out)
{
    *out = NULL;
    char name[sizeof("sst-18446744073709551615")];
    bool checkpoint = entry->kind == FDB_FILE_CHECKPOINT;
    (void)snprintf(name, sizeof(name), "%s-%020" PRIu64, checkpoint ? "chk" : "sst", entry->id);
    FDBSSTReader *reader;
    FastDBStatus status = fdb_sst_reader_open(dir, name, entry->id, limits, &reader);
    if (status != FASTDB_OK)
        return status;
    const FDBSSTProperties *meta = fdb_sst_reader_properties(reader);
    bool valid_end = checkpoint ? meta->max_sequence <= entry->sequence_end && meta->tombstones == 0 :
                                  meta->max_sequence == entry->sequence_end;
    if (meta->file_bytes != entry->bytes || meta->min_sequence != entry->sequence_begin ||
        !valid_end ||
        fdb_compare(meta->min_key, entry->min_key) != 0 ||
        fdb_compare(meta->max_key, entry->max_key) != 0) {
        (void)fdb_sst_reader_close(reader);
        return FASTDB_CORRUPTION;
    }
    *out = reader;
    return FASTDB_OK;
}

static FastDBStatus verify_table(const FDBDirectory *dir, const FDBManifestEntry *entry,
                                 const FDBSSTLimits *limits, size_t *corrupt_filters,
                                 FDBSSTVisit restore, void *context)
{
    FDBSSTReader *reader;
    FastDBStatus status = fdb_catalog_open_table(dir, entry, limits, &reader);
    if (status != FASTDB_OK)
        return status;
    status = fdb_sst_reader_walk(reader, restore, context);
    if (status == FASTDB_OK &&
        !fdb_add_size(*corrupt_filters, fdb_sst_reader_corrupt_partitions(reader), corrupt_filters))
        *corrupt_filters = SIZE_MAX;
    FastDBStatus close_status = fdb_sst_reader_close(reader);
    return status == FASTDB_OK ? close_status : status;
}

static FastDBStatus covered_sequence(const FDBDirectory *dir, const FDBManifestLimits *limits,
                                      const FDBSSTLimits *sst_limits, FDBStorageMode mode,
                                      uint64_t *covered, size_t *corrupt_filters,
                                      FDBSSTVisit restore, void *context)
{
    *covered = 0;
    FDBManifestLoad manifest;
    FastDBStatus status = fdb_manifest_load(dir, limits, &manifest);
    if (status != FASTDB_OK)
        return status;
    if (manifest.view.mode != mode)
        status = FASTDB_UNSUPPORTED;
    if (status == FASTDB_OK && mode == FDB_MODE_LSM)
        status = fdb_compaction_check_levels(&manifest.view, limits->max_levels);
    for (size_t i = 0; status == FASTDB_OK && i < manifest.view.count; ++i) {
        const FDBManifestEntry *entry = &manifest.view.entries[i];
        if (entry->kind == FDB_FILE_WAL)
            continue;
        status = verify_table(dir, entry, sst_limits, corrupt_filters, restore, context);
        if (status == FASTDB_OK && entry->sequence_end > *covered)
            *covered = entry->sequence_end;
    }
    fdb_manifest_release(&manifest);
    return status;
}

typedef struct ReservedName {
    uint64_t id;
    FDBFileKind kind;
    bool temporary;
} ReservedName;

static ReservedName parse_name(const char *name)
{
    ReservedName result = {0};
    const char *digits;
    size_t size = strlen(name);
    if (size == 24 && memcmp(name, "wal-", 4) == 0) {
        digits = name + 4;
        result.kind = FDB_FILE_WAL;
    } else if (size == 24 && (memcmp(name, "sst-", 4) == 0 || memcmp(name, "chk-", 4) == 0)) {
        digits = name + 4;
        result.kind = *name == 's' ? FDB_FILE_SSTABLE : FDB_FILE_CHECKPOINT;
    } else if (size == 29 && memcmp(name + 25, ".tmp", 4) == 0 &&
               (memcmp(name, ".sst-", 5) == 0 || memcmp(name, ".chk-", 5) == 0)) {
        digits = name + 5;
        result.kind = name[1] == 's' ? FDB_FILE_SSTABLE : FDB_FILE_CHECKPOINT;
        result.temporary = true;
    } else {
        return result;
    }
    for (size_t i = 0; i < 20; ++i) {
        if (digits[i] < '0' || digits[i] > '9')
            return (ReservedName){0};
        unsigned int digit = (unsigned int)(digits[i] - '0');
        if (result.id > (UINT64_MAX - digit) / 10)
            return (ReservedName){0};
        result.id = result.id * 10 + digit;
    }
    return result;
}

static bool referenced(const FDBManifest *manifest, ReservedName file)
{
    if (file.temporary)
        return false;
    size_t low = 0, high = manifest->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        const FDBManifestEntry *entry = &manifest->entries[mid];
        if (entry->id == file.id)
            return entry->kind == file.kind;
        if (entry->id < file.id)
            low = mid + 1;
        else
            high = mid;
    }
    return false;
}

typedef struct Reclaim {
    FDBWalSet *set;
    bool removed;
} Reclaim;

static FastDBStatus reclaim_file(void *context, const char *name)
{
    Reclaim *reclaim = context;
    const FDBManifest *manifest = &reclaim->set->catalog.view;
    ReservedName file = parse_name(name);
    uint64_t generation;
    bool manifest_temp = fdb_manifest_temp_generation(name, &generation) &&
                         generation - 1 <= manifest->generation;
    bool compaction_output = reclaim->set->compaction_outputs_reserved && file.id != 0 &&
                             file.kind == FDB_FILE_SSTABLE && file.temporary &&
                             file.id >= reclaim->set->compaction_output_begin &&
                             file.id < reclaim->set->compaction_output_end;
    if (compaction_output)
        return FASTDB_OK;
    if (!manifest_temp &&
        (file.id == 0 || file.id >= manifest->next_file_id || referenced(manifest, file)))
        return FASTDB_OK;
    bool regular;
    FastDBStatus status = fdb_dir_is_regular(reclaim->set->dir, name, &regular);
    if (status != FASTDB_OK || !regular)
        return status;
    status = fdb_dir_unlink(reclaim->set->dir, name);
    if (status == FASTDB_OK)
        reclaim->removed = true;
    return status;
}

FastDBStatus fdb_storage_reclaim_orphans(FDBWalSet *set)
{
    if (set->poisoned)
        return FASTDB_COMMIT_UNCERTAIN;
    set->reclaim_pending = true;
    if (set->pinned_read_bytes != 0)
        return FASTDB_OK;
    /* Select a durable catalog before deleting files an older name could still reference. */
    FastDBStatus status = fdb_dir_sync(set->dir);
    if (status != FASTDB_OK)
        return status;
    Reclaim reclaim = {.set = set};
    do {
        reclaim.removed = false;
        status = fdb_dir_visit(set->dir, reclaim_file, &reclaim);
        if (reclaim.removed) {
            FastDBStatus sync_status = fdb_dir_sync(set->dir);
            if (status == FASTDB_OK)
                status = sync_status;
        }
    } while (status == FASTDB_OK && reclaim.removed);
    if (status == FASTDB_OK)
        set->reclaim_pending = false;
    return status;
}

FastDBStatus fdb_storage_prepare_maintenance(FDBWalSet *set)
{
    if (set->poisoned)
        return FASTDB_COMMIT_UNCERTAIN;
    if (!set->reclaim_pending)
        return FASTDB_OK;
    if (set->pinned_read_bytes != 0)
        return FASTDB_BUSY;
    return fdb_storage_reclaim_orphans(set);
}

static FastDBStatus recover(const FDBDirectory *dir, const FDBManifestLimits *manifest_limits,
                             const FDBWalLimits *wal_limits, const FDBSSTLimits *sst_limits,
                             FDBStorageMode mode, const FDBWalReplay *replay, FDBSSTVisit restore,
                             FDBWalSet *out)
{
    *out = (FDBWalSet){.wal.fd = -1, .active_index = SIZE_MAX};
    if (!fdb_sst_valid_limits(sst_limits) || !fdb_wal_valid_limits(wal_limits))
        return FASTDB_INVALID_ARGUMENT;
    uint64_t covered;
    size_t corrupt_filters = 0;
    FastDBStatus status = covered_sequence(dir, manifest_limits, sst_limits, mode, &covered,
                                            &corrupt_filters, restore,
                                            replay == NULL ? NULL : replay->context);
    if (status != FASTDB_OK)
        return status;
    status = fdb_wal_set_open(dir, manifest_limits, wal_limits, covered, replay, out);
    if (status != FASTDB_OK)
        return status;
    out->corrupt_filter_partitions = corrupt_filters;
    status = fdb_storage_reclaim_orphans(out);
    if (status != FASTDB_OK)
        (void)fdb_wal_set_close(out);
    return status;
}

FastDBStatus fdb_lsm_recover(const FDBDirectory *dir, const FDBManifestLimits *manifest_limits,
                             const FDBWalLimits *wal_limits, const FDBSSTLimits *sst_limits,
                             const FDBWalReplay *replay, FDBWalSet *out)
{
    return recover(dir, manifest_limits, wal_limits, sst_limits, FDB_MODE_LSM, replay, NULL, out);
}

FastDBStatus fdb_checkpoint_recover(const FDBDirectory *dir,
                                    const FDBManifestLimits *manifest_limits,
                                    const FDBWalLimits *wal_limits, const FDBSSTLimits *sst_limits,
                                    const FDBWalReplay *replay, FDBSSTVisit restore, FDBWalSet *out)
{
    return recover(dir, manifest_limits, wal_limits, sst_limits,
                   FDB_MODE_MEMORY_PERSISTENT, replay, restore, out);
}
