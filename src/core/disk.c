#include "disk.h"
#include "compaction.h"
#include "flush.h"
#include "recovery.h"
#include "sstable_format.h"

#include <stdlib.h>
#include <string.h>

struct FDBDisk {
    size_t compaction_bytes;
    FDBDirectory dir;
    FDBWalSet log;
    FDBSSTLimits table;
    FDBManifestLimits manifest;
    FDBWalLimits wal;
    FastDBMode mode;
    FastDBDurability durability;
    size_t checkpoint_wal_segments;
    FastDBCompactionOptions compaction;
    FDBCompactionPolicy policy;
    size_t reader_bytes;
    size_t writer_bytes;
    size_t table_workspace;
    bool compaction_cleanup_needed;
    bool compaction_flush_allowed;
    uint64_t min_free_bytes;
};

static const FastDBPersistentOptions defaults = {
    .durability = FASTDB_DURABILITY_SYNC,
    .max_wal_segment_bytes = UINT64_C(64) * 1024 * 1024,
    .max_wal_frame_bytes = 32 * 1024 * 1024,
    .target_block_bytes = 16 * 1024,
    .max_index_bytes = 1024 * 1024,
    .max_filter_bytes = 256 * 1024,
    .max_catalog_files = 128,
    .max_catalog_bytes = 512 * 1024,
    .bloom_bits_per_key = 10,
    .min_free_bytes = 0,
};

void fastdb_persistent_options_default(FastDBPersistentOptions *options)
{
    if (options != NULL)
        *options = defaults;
}

void fastdb_compaction_options_default(FastDBCompactionOptions *options)
{
    if (options != NULL) {
        *options = (FastDBCompactionOptions){
            .enabled = true,
            .max_levels = 7,
            .level0_file_trigger = 4,
            .level1_bytes = UINT64_C(256) * 1024 * 1024,
            .level_multiplier = 10,
            .max_merge_inputs = 8,
            .target_file_bytes = UINT64_C(64) * 1024 * 1024,
        };
    }
}

/* Catalog edits can retain the current view, the replacement, and a checked disk image. */
static bool catalog_budget(const FDBManifestLimits *limits, size_t *bytes)
{
    size_t one;
    if (limits->max_files > SIZE_MAX / sizeof(FDBManifestEntry) ||
        !fdb_add_size(limits->max_bytes, limits->max_files * sizeof(FDBManifestEntry), &one) ||
        one > SIZE_MAX / 3)
        return false;
    *bytes = 3 * one;
    return true;
}

static FastDBStatus settings(const FastDBOptions *options, FDBDisk *disk, size_t *workspace)
{
    const FastDBPersistentOptions *storage = &options->storage;
    const FastDBLimits *limits = &options->limits;
    size_t record, block;
    if ((storage->durability != FASTDB_DURABILITY_SYNC && storage->durability != FASTDB_DURABILITY_ASYNC) ||
        storage->max_catalog_files < 2 ||
        storage->checkpoint_wal_segments >= storage->max_catalog_files ||
        (options->mode == FASTDB_MODE_LSM && storage->checkpoint_wal_segments != 0))
        return FASTDB_INVALID_ARGUMENT;
    if (!fdb_add_size(FDB_RECORD_HEADER_BYTES, limits->max_key_bytes, &record) ||
        !fdb_add_size(record, limits->max_value_bytes, &record) ||
        !fdb_add_size(record, SST_BLOCK_HEADER_BYTES + SST_OFFSET_BYTES, &block) ||
        storage->max_wal_frame_bytes < FDB_WAL_FRAME_BYTES ||
        record > storage->max_wal_frame_bytes - FDB_WAL_FRAME_BYTES)
        return FASTDB_INVALID_ARGUMENT;
    if (block < storage->target_block_bytes)
        block = storage->target_block_bytes;
    disk->wal = (FDBWalLimits){
        limits->max_key_bytes, limits->max_value_bytes, limits->max_batch_operations,
        storage->max_wal_frame_bytes, storage->max_wal_segment_bytes,
    };
    disk->table = (FDBSSTLimits){
        limits->max_key_bytes, limits->max_value_bytes, storage->target_block_bytes, block,
        storage->max_index_bytes, storage->max_filter_bytes, storage->bloom_bits_per_key,
    };
    disk->compaction = options->compaction;
    disk->policy = (FDBCompactionPolicy){
        disk->compaction.max_levels, disk->compaction.level0_file_trigger,
        disk->compaction.level1_bytes, disk->compaction.level_multiplier,
        disk->compaction.max_merge_inputs,
    };
    if (options->mode == FASTDB_MODE_LSM &&
        (!fdb_compaction_policy_valid(&disk->policy) || disk->compaction.target_file_bytes == 0))
        return FASTDB_INVALID_ARGUMENT;
    disk->manifest = (FDBManifestLimits){
        storage->max_catalog_files, storage->max_catalog_bytes, limits->max_key_bytes,
        options->mode == FASTDB_MODE_LSM ? disk->policy.levels : 7,
    };
    disk->mode = options->mode;
    disk->durability = storage->durability;
    disk->min_free_bytes = storage->min_free_bytes;
    size_t default_segments = storage->max_catalog_files <= 8 ? storage->max_catalog_files - 1 : 8;
    disk->checkpoint_wal_segments = storage->checkpoint_wal_segments == 0 ?
                                    default_segments : storage->checkpoint_wal_segments;
    size_t bound_keys;
    if (!fdb_wal_valid_limits(&disk->wal) || !fdb_sst_valid_limits(&disk->table) ||
        !fdb_manifest_valid_limits(&disk->manifest) ||
        !fdb_add_size(limits->max_key_bytes, limits->max_key_bytes, &bound_keys) ||
        !fdb_manifest_can_fit(&disk->manifest, 2, bound_keys) ||
        !fdb_sst_tree_fits(&disk->table, 1, limits->max_key_bytes))
        return FASTDB_INVALID_ARGUMENT;
    size_t writer, reader, catalog;
    if (!fdb_sst_writer_budget(&disk->table, &writer) ||
        !fdb_sst_reader_budget(&disk->table, &reader) ||
        !catalog_budget(&disk->manifest, &catalog) ||
        !fdb_add_size(sizeof(*disk), catalog, workspace))
        return FASTDB_INVALID_ARGUMENT;
    disk->reader_bytes = reader;
    disk->writer_bytes = writer;
    disk->table_workspace = writer > reader ? writer : reader;
    if (options->flush_threshold != 0 &&
        (options->mode != FASTDB_MODE_LSM || !fdb_add_size(reader, writer, &disk->table_workspace) ||
         !fdb_add_size(*workspace, sizeof(FDBBackgroundFlush), workspace)))
        return FASTDB_INVALID_ARGUMENT;
    if (!fdb_add_size(*workspace, disk->table_workspace, workspace))
        return FASTDB_INVALID_ARGUMENT;
    size_t smallest_node;
    size_t available = limits->max_memory_bytes - sizeof(FastDB);
    if (!fdb_node_size((FastDBSlice){.size = 1}, (FastDBSlice){0}, &smallest_node) ||
        *workspace > available || options->flush_threshold > available - *workspace ||
        smallest_node + FDB_WAL_FRAME_BYTES + FDB_RECORD_HEADER_BYTES + 1 > available - *workspace)
        return FASTDB_INVALID_ARGUMENT;
    return FASTDB_OK;
}

FastDBStatus fdb_disk_validate(const FastDBOptions *options)
{
    FDBDisk disk;
    size_t workspace;
    return settings(options, &disk, &workspace);
}

typedef struct DirectoryContents {
    bool manifest;
    bool native_files;
    bool other;
} DirectoryContents;

static FastDBStatus classify_entry(void *context, const char *name)
{
    DirectoryContents *contents = context;
    uint64_t generation;
    if (strcmp(name, "MANIFEST") == 0)
        contents->manifest = true;
    else if (fdb_manifest_temp_generation(name, &generation)) {
        if (generation != 1)
            contents->native_files = true;
    }
    else if (strncmp(name, "wal-", 4) == 0 || strncmp(name, "sst-", 4) == 0 ||
             strncmp(name, ".sst-", 5) == 0 || strncmp(name, "chk-", 4) == 0 ||
             strncmp(name, ".chk-", 5) == 0)
        contents->native_files = true;
    else
        contents->other = true;
    return FASTDB_OK;
}

static FastDBStatus check_directory(const FDBDirectory *dir)
{
    DirectoryContents contents = {0};
    FastDBStatus status = fdb_dir_visit(dir, classify_entry, &contents);
    if (status == FASTDB_OK && !contents.manifest && !contents.native_files && contents.other)
        return FASTDB_UNSUPPORTED;
    return status;
}

static FastDBStatus ensure_manifest(FDBDisk *disk)
{
    DirectoryContents contents = {0};
    FastDBStatus status = fdb_dir_visit(&disk->dir, classify_entry, &contents);
    if (status != FASTDB_OK || contents.manifest)
        return status;
    if (contents.native_files)
        return FASTDB_CORRUPTION;
    if (contents.other)
        return FASTDB_UNSUPPORTED;
    FDBManifest manifest;
    status = fdb_manifest_new(disk->mode == FASTDB_MODE_LSM ?
                              FDB_MODE_LSM : FDB_MODE_MEMORY_PERSISTENT, &manifest);
    return status == FASTDB_OK ?
           fdb_manifest_publish(&disk->dir, &disk->manifest, 0, &manifest) : status;
}

FastDBStatus fdb_disk_open(FastDB *db, const FastDBOptions *options, const FDBFs *fs,
                           const FDBWalReplay *replay, FDBSSTVisit restore)
{
    FDBDisk *disk = calloc(1, sizeof(*disk));
    if (disk == NULL)
        return FASTDB_OUT_OF_MEMORY;
    FastDBStatus status = settings(options, disk, &db->disk_budget);
    if (status == FASTDB_OK)
        status = fdb_dir_lock_checked(options->path, fs, check_directory, &disk->dir);
    if (status != FASTDB_OK) {
        free(disk);
        return status;
    }
    db->disk = disk;
    status = ensure_manifest(disk);
    if (status == FASTDB_OK) {
        status = disk->mode == FASTDB_MODE_LSM ?
                 fdb_lsm_recover(&disk->dir, &disk->manifest, &disk->wal, &disk->table,
                                  replay, &disk->log) :
                 fdb_checkpoint_recover(&disk->dir, &disk->manifest, &disk->wal, &disk->table,
                                         replay, restore, &disk->log);
    }
    if (status != FASTDB_OK) {
        db->disk = NULL;
        (void)fdb_dir_close(&disk->dir);
        free(disk);
        return status;
    }
    db->sequence = disk->log.wal.sequence;
    return FASTDB_OK;
}

FastDBStatus fdb_disk_close(FDBDisk *disk)
{
    if (disk == NULL)
        return FASTDB_OK;
    FastDBStatus status = disk->durability == FASTDB_DURABILITY_ASYNC ?
                          fdb_wal_set_sync(&disk->log) : FASTDB_OK;
    if (status == FASTDB_OK && disk->log.reclaim_pending)
        status = fdb_storage_prepare_maintenance(&disk->log);
    FastDBStatus wal_status = fdb_wal_set_close(&disk->log);
    FastDBStatus dir_status = fdb_dir_close(&disk->dir);
    free(disk);
    if (status != FASTDB_OK)
        return status;
    return wal_status != FASTDB_OK ? wal_status : dir_status;
}

bool fdb_disk_poisoned(const FDBDisk *disk)
{
    return disk != NULL && disk->log.poisoned;
}

bool fdb_disk_has_tables(const FDBDisk *disk)
{
    return disk != NULL && disk->mode == FASTDB_MODE_LSM;
}

bool fdb_disk_tree_fits(const FDBDisk *disk, size_t keys, size_t key_bytes)
{
    return disk == NULL || fdb_sst_tree_fits(&disk->table, keys, key_bytes);
}

FastDBStatus fdb_disk_frame_size(const FDBDisk *disk, const FDBRecord *records,
                                 size_t count, size_t *bytes)
{
    return fdb_wal_frame_size(&disk->log.wal, records, count, bytes);
}

static FastDBStatus check_free_space(const FDBDisk *disk, uint64_t reservation)
{
    if (disk->min_free_bytes == 0)
        return FASTDB_OK;
    if (reservation > UINT64_MAX - disk->min_free_bytes)
        return FASTDB_LIMIT_EXCEEDED;
    uint64_t available;
    FastDBStatus status = fdb_dir_available_bytes(&disk->dir, &available);
    if (status != FASTDB_OK)
        return status;
    return available >= reservation + disk->min_free_bytes ? FASTDB_OK :
           FASTDB_LIMIT_EXCEEDED;
}

static FastDBStatus estimate_tree_file_bytes(const FDBDisk *disk, const FDBTree *tree,
                                              uint64_t *estimate)
{
    uint64_t total = 0;
    for (const FDBNode *node = fdb_tree_bound(tree, (FastDBSlice){0}, false); node != NULL;
         node = fdb_tree_bound(tree, node->record.key, true)) {
        uint64_t key = node->record.key.size;
        uint64_t value = node->record.value.size;
        uint64_t record;
        if (key > UINT64_MAX - value || key + value > UINT64_MAX - 160u)
            return FASTDB_LIMIT_EXCEEDED;
        record = key + value + 160u;
        uint64_t filter = ((uint64_t)disk->table.bloom_bits_per_key + 7u) / 8u;
        uint64_t index = (uint64_t)node->record.key.size + 32u;
        if (record > UINT64_MAX - filter || record + filter > UINT64_MAX - index ||
            total > UINT64_MAX - (record + filter + index))
            return FASTDB_LIMIT_EXCEEDED;
        total += record + filter + index;
    }
    if (total > UINT64_MAX - disk->manifest.max_bytes)
        return FASTDB_LIMIT_EXCEEDED;
    *estimate = total + disk->manifest.max_bytes;
    return FASTDB_OK;
}

FastDBStatus fdb_disk_append(FDBDisk *disk, const FDBRecord *records, size_t count)
{
    size_t frame_bytes;
    FastDBStatus status = fdb_wal_frame_size(&disk->log.wal, records, count, &frame_bytes);
    uint64_t reservation = frame_bytes;
    if (status == FASTDB_OK &&
        frame_bytes > disk->wal.max_segment_bytes - disk->log.wal.offset) {
        uint64_t overhead = FDB_WAL_HEADER_BYTES + disk->manifest.max_bytes;
        if (reservation > UINT64_MAX - overhead)
            status = FASTDB_LIMIT_EXCEEDED;
        else
            reservation += overhead;
    }
    if (status == FASTDB_OK)
        status = check_free_space(disk, reservation);
    if (status != FASTDB_OK)
        return status;
    return fdb_wal_set_append(&disk->log, records, count, disk->durability == FASTDB_DURABILITY_SYNC);
}

FastDBStatus fdb_lookup_record(const FDBRecord *record, bool copy, FDBLookup *out)
{
    free(out->value);
    *out = (FDBLookup){.revision = record->sequence, .kind = record->kind,
                       .expires_at_ms = record->expires_at_ms};
    if (copy && record->kind == FDB_PUT) {
        size_t size = record->value.size;
        out->value = malloc(size == 0 ? 1 : size);
        if (out->value == NULL)
            return FASTDB_OUT_OF_MEMORY;
        if (size != 0)
            memcpy(out->value, record->value.data, size);
        out->size = size;
    }
    return FASTDB_OK;
}

static FastDBStatus find_in_table(const FDBDisk *disk, const FDBManifestEntry *entry,
                                  FastDBSlice key, uint64_t sequence, bool copy, FDBLookup *out)
{
    FDBSSTReader *reader;
    FastDBStatus status = fdb_catalog_open_table(&disk->dir, entry, &disk->table, &reader);
    if (status != FASTDB_OK)
        return status;
    FDBSSTIterator *iter;
    status = fdb_sst_iter_open(reader, &iter);
    if (status == FASTDB_OK) {
        fdb_sst_iter_seek_version(iter, key, sequence);
        status = fdb_sst_iter_status(iter);
        const FDBRecord *record = fdb_sst_iter_current(iter);
        if (status == FASTDB_OK && record != NULL && record->sequence > out->revision)
            status = fdb_lookup_record(record, copy, out);
        fdb_sst_iter_close(iter);
    }
    FastDBStatus close_status = fdb_sst_reader_close(reader);
    return status == FASTDB_OK ? close_status : status;
}

static bool lookup_candidate(const FDBManifestEntry *entry, FastDBSlice key,
                              uint64_t sequence, uint64_t found)
{
    return entry->kind == FDB_FILE_SSTABLE && entry->sequence_end > found &&
           entry->sequence_begin <= sequence && fdb_compare(entry->min_key, key) <= 0 &&
           fdb_compare(entry->max_key, key) >= 0;
}

static FastDBStatus find_entries(const FDBDisk *disk, const FDBManifestEntry *entries, size_t count,
                                  FastDBSlice key, uint64_t sequence, bool copy, FDBLookup *out)
{
    for (size_t i = count; i != 0; --i) {
        const FDBManifestEntry *entry = &entries[i - 1];
        if (!lookup_candidate(entry, key, sequence, out->revision))
            continue;
        FastDBStatus status = find_in_table(disk, entry, key, sequence, copy, out);
        if (status != FASTDB_OK)
            return status;
    }
    return FASTDB_OK;
}

FastDBStatus fdb_disk_find(const FDBDisk *disk, FastDBSlice key, uint64_t sequence,
                           bool copy, FDBLookup *out)
{
    const FDBManifest *catalog = &disk->log.catalog.view;
    return find_entries(disk, catalog->entries, catalog->count, key, sequence, copy, out);
}

size_t fdb_disk_pinned_bytes(const FDBDisk *disk)
{
    return disk == NULL ? 0 : disk->log.pinned_read_bytes;
}

bool fdb_disk_maintenance_waiting(const FDBDisk *disk)
{
    return disk->compaction_bytes != 0 ||
           (disk->log.reclaim_pending && disk->log.pinned_read_bytes != 0);
}

size_t fdb_disk_compaction_bytes(const FDBDisk *disk)
{
    return disk == NULL ? 0 : disk->compaction_bytes;
}

FastDBStatus fdb_disk_pin_read(FDBDisk *disk, FastDBSlice key, uint64_t sequence,
                               size_t headroom, FDBDiskRead *view)
{
    *view = (FDBDiskRead){.sequence = sequence};
    if (disk->log.pinned_read_bytes != 0 || disk->log.reclaim_pending)
        return FASTDB_BUSY;
    const FDBManifest *catalog = &disk->log.catalog.view;
    size_t bytes = 0, count = 0;
    for (size_t i = 0; i < catalog->count; ++i) {
        const FDBManifestEntry *entry = &catalog->entries[i];
        if (lookup_candidate(entry, key, sequence, 0)) {
            if (!fdb_add_size(bytes, sizeof(*entry), &bytes) ||
                !fdb_add_size(bytes, entry->min_key.size, &bytes) ||
                !fdb_add_size(bytes, entry->max_key.size, &bytes))
                return FASTDB_BUSY;
            ++count;
        }
    }
    if (count == 0)
        return FASTDB_OK;
    size_t charge;
    if (!fdb_add_size(bytes, disk->reader_bytes, &charge) || charge > headroom)
        return FASTDB_BUSY;
    FDBManifestEntry *entries = malloc(bytes);
    if (entries == NULL)
        return FASTDB_BUSY;
    unsigned char *bounds = (unsigned char *)(entries + count);
    size_t next = 0;
    for (size_t i = 0; i < catalog->count; ++i) {
        if (lookup_candidate(&catalog->entries[i], key, sequence, 0)) {
            entries[next] = catalog->entries[i];
            fdb_manifest_copy_bounds(&entries[next++], &bounds);
        }
    }
    view->entries = entries;
    view->count = count;
    disk->log.pinned_read_bytes = charge;
    return FASTDB_OK;
}

FastDBStatus fdb_disk_find_pinned(const FDBDisk *disk, const FDBDiskRead *view, FastDBSlice key,
                                  bool copy, FDBLookup *out)
{
    return find_entries(disk, view->entries, view->count, key, view->sequence, copy, out);
}

void fdb_disk_unpin_read(FDBDisk *disk, FDBDiskRead *view)
{
    free(view->entries);
    disk->log.pinned_read_bytes = 0;
}

FastDBStatus fdb_disk_flush(FDBDisk *disk, FDBTree *tree)
{
    if (!fdb_disk_has_tables(disk))
        return FASTDB_UNSUPPORTED;
    if (disk->compaction_bytes != 0 && !disk->compaction_flush_allowed)
        return FASTDB_BUSY;
    uint64_t estimate;
    FastDBStatus status = estimate_tree_file_bytes(disk, tree, &estimate);
    if (status == FASTDB_OK)
        status = check_free_space(disk, estimate);
    if (status == FASTDB_OK)
        status = fdb_storage_prepare_maintenance(&disk->log);
    if (status == FASTDB_OK)
        status = fdb_flush_l0(&disk->log, tree, disk->log.wal.sequence, &disk->table);
    return status == FASTDB_OK ? fdb_storage_reclaim_orphans(&disk->log) : status;
}

FastDBStatus fdb_disk_retry_frozen(FDBDisk *disk, FDBTree *tree, uint64_t sequence)
{
    if (disk->compaction_bytes != 0)
        return FASTDB_BUSY;
    uint64_t estimate;
    FastDBStatus ready = estimate_tree_file_bytes(disk, tree, &estimate);
    if (ready == FASTDB_OK)
        ready = check_free_space(disk, estimate);
    if (ready == FASTDB_OK)
        ready = fdb_storage_prepare_maintenance(&disk->log);
    if (ready != FASTDB_OK)
        return ready;
    if (disk->log.pinned_read_bytes != 0)
        return FASTDB_BUSY;
    FastDBStatus status = fdb_storage_reclaim_orphans(&disk->log);
    if (status == FASTDB_OK)
        status = fdb_flush_l0(&disk->log, tree, sequence, &disk->table);
    return status == FASTDB_OK ? fdb_storage_reclaim_orphans(&disk->log) : status;
}

FastDBStatus fdb_disk_rotate_wal(FDBDisk *disk)
{
    FastDBStatus status = fdb_storage_prepare_maintenance(&disk->log);
    return status == FASTDB_OK ? fdb_wal_set_rotate(&disk->log) : status;
}

FastDBStatus fdb_disk_flush_reserve(FDBDisk *disk, FDBFlushOutput *output)
{
    return fdb_flush_reserve(&disk->log, output);
}

FastDBStatus fdb_disk_flush_build(const FDBDisk *disk, const FDBTree *tree, FDBFlushOutput *output)
{
    return fdb_flush_build(&disk->dir, &disk->table, tree, output);
}

FastDBStatus fdb_disk_flush_commit(FDBDisk *disk, FDBTree *tree, const FDBFlushOutput *output)
{
    FastDBStatus status = fdb_flush_commit(&disk->log, tree, output);
    return status == FASTDB_OK ? fdb_storage_reclaim_orphans(&disk->log) : status;
}

FastDBStatus fdb_disk_compact(FastDB *db, bool manual, bool background,
                               const FastDBRange *range)
{
    FDBDisk *disk = db->disk;
    if (!fdb_disk_has_tables(disk) || !disk->compaction.enabled)
        return manual ? FASTDB_UNSUPPORTED : FASTDB_OK;
    if (disk->log.poisoned)
        return FASTDB_COMMIT_UNCERTAIN;
    if (disk->compaction_bytes != 0)
        return FASTDB_BUSY;
    FastDBStatus ready = fdb_storage_prepare_maintenance(&disk->log);
    if (ready != FASTDB_OK)
        return ready == FASTDB_BUSY && !manual ? FASTDB_OK : ready;
    if (disk->compaction_cleanup_needed) {
        FastDBStatus cleanup = fdb_storage_reclaim_orphans(&disk->log);
        if (cleanup != FASTDB_OK)
            return cleanup;
        disk->compaction_cleanup_needed = false;
    }

    size_t headroom = db->limits.max_memory_bytes - db->disk_budget - fdb_memory_bytes(db);
    size_t available = disk->table_workspace + headroom;
    size_t cap = disk->compaction.max_working_bytes;
    if (cap != 0 && cap < available)
        available = cap;
    /* Charge one metadata and cursor bound per input, plus all reader/heap pointers. */
    size_t input_bytes;
    if (!fdb_add_size(disk->reader_bytes,
                     sizeof(FDBSSTReader *) + 2 * sizeof(FDBSSTIterator *) + sizeof(size_t),
                     &input_bytes))
        return FASTDB_LIMIT_EXCEEDED;
    size_t affordable = available > disk->writer_bytes ?
                        (available - disk->writer_bytes) / input_bytes : 0;
    FDBCompactionPolicy policy = disk->policy;
    if (policy.max_inputs > disk->log.catalog.view.count)
        policy.max_inputs = disk->log.catalog.view.count;
    if (policy.max_inputs > affordable)
        policy.max_inputs = affordable;
    if (policy.max_inputs < 2)
        policy.max_inputs = 2;
    size_t inline_inputs[2];
    uint64_t inline_input_ids[2];
    size_t input_indexes = policy.max_inputs * sizeof(size_t);
    size_t input_id_bytes = policy.max_inputs * sizeof(uint64_t);
    size_t selection_bytes;
    if (!fdb_add_size(input_indexes, input_id_bytes, &selection_bytes))
        return FASTDB_LIMIT_EXCEEDED;
    size_t *inputs = policy.max_inputs == 2 ? inline_inputs : malloc(input_indexes);
    uint64_t *input_ids = policy.max_inputs == 2 ? inline_input_ids : malloc(input_id_bytes);
    if (inputs == NULL || input_ids == NULL) {
        if (inputs != inline_inputs)
            free(inputs);
        if (input_ids != inline_input_ids)
            free(input_ids);
        return FASTDB_OUT_OF_MEMORY;
    }
    FDBMergeLimits limits = {
        .output_table = disk->table,
        .max_inputs = policy.max_inputs,
        .max_outputs = disk->manifest.max_files,
        .target_output_bytes = disk->compaction.target_file_bytes,
    };
    FastDBStatus status;
    for (;;) {
        FDBCompactionPlan plan;
        status = fdb_compaction_plan(&disk->log.catalog.view, &policy, manual, range, inputs, &plan);
        if (status != FASTDB_OK || plan.count == 0)
            break;
        status = fdb_storage_prepare_maintenance(&disk->log);
        if (status != FASTDB_OK)
            break;
        headroom = db->limits.max_memory_bytes - db->disk_budget - fdb_memory_bytes(db);
        available = headroom + disk->table_workspace;
        if (cap != 0 && cap < available)
            available = cap;
        if (selection_bytes > available || disk->writer_bytes > available - selection_bytes ||
            plan.count > (available - selection_bytes - disk->writer_bytes) / input_bytes) {
            status = FASTDB_LIMIT_EXCEEDED;
            break;
        }
        for (size_t i = 0; i < plan.count; ++i)
            input_ids[i] = disk->log.catalog.view.entries[inputs[i]].id;
        uint64_t output_estimate = 0;
        for (size_t i = 0; i < plan.count; ++i) {
            uint64_t bytes = disk->log.catalog.view.entries[inputs[i]].bytes;
            if (bytes > UINT64_MAX / 2 || output_estimate > UINT64_MAX - bytes * 2) {
                status = FASTDB_LIMIT_EXCEEDED;
                break;
            }
            output_estimate += bytes * 2;
        }
        if (status == FASTDB_OK) {
            if (output_estimate > UINT64_MAX - disk->manifest.max_bytes)
                status = FASTDB_LIMIT_EXCEEDED;
            else
                output_estimate += disk->manifest.max_bytes;
        }
        if (status == FASTDB_OK)
            status = check_free_space(disk, output_estimate);
        if (status != FASTDB_OK)
            break;
        limits.max_working_bytes = available - selection_bytes;
        size_t catalogs, charge = disk->writer_bytes + plan.count * input_bytes;
        pthread_mutex_t *mutex = NULL;
        if ((manual || background) && catalog_budget(&disk->manifest, &catalogs) &&
            fdb_add_size(charge, catalogs / 3, &charge) &&
            fdb_add_size(charge, selection_bytes, &charge) &&
            fdb_add_size(charge, plan.split_after.size, &charge) && charge <= headroom) {
            disk->compaction_bytes = charge;
            disk->compaction_flush_allowed = background;
            mutex = &db->mutex;
        }
        status = fdb_compact_once(&disk->log, &policy, &plan, input_ids, &limits, mutex);
        disk->compaction_bytes = 0;
        disk->compaction_flush_allowed = false;
        if (status != FASTDB_OK) {
            disk->compaction_cleanup_needed = true;
            break;
        }
    }
    if (inputs != inline_inputs)
        free(inputs);
    if (input_ids != inline_input_ids)
        free(input_ids);
    return status == FASTDB_BUSY && !manual ? FASTDB_OK : status;
}

FastDBStatus fdb_disk_checkpoint(FDBDisk *disk, FDBTree *tree)
{
    if (disk == NULL || disk->mode != FASTDB_MODE_MEMORY_PERSISTENT)
        return FASTDB_UNSUPPORTED;
    uint64_t estimate;
    FastDBStatus status = estimate_tree_file_bytes(disk, tree, &estimate);
    if (status == FASTDB_OK)
        status = check_free_space(disk, estimate);
    if (status == FASTDB_OK)
        status = fdb_checkpoint_truncate_log(&disk->log, tree, &disk->table);
    return status == FASTDB_OK ? fdb_storage_reclaim_orphans(&disk->log) : status;
}

FastDBStatus fdb_disk_maybe_checkpoint(FDBDisk *disk, FDBTree *tree, size_t frame_bytes)
{
    if (disk->mode != FASTDB_MODE_MEMORY_PERSISTENT ||
        frame_bytes <= disk->wal.max_segment_bytes - disk->log.wal.offset)
        return FASTDB_OK;
    const FDBManifest *manifest = &disk->log.catalog.view;
    size_t segments = 0;
    for (size_t i = 0; i < manifest->count; ++i) {
        if (manifest->entries[i].kind == FDB_FILE_WAL)
            ++segments;
    }
    return segments < disk->checkpoint_wal_segments ? FASTDB_OK : fdb_disk_checkpoint(disk, tree);
}
