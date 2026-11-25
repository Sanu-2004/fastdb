#include "flush.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct FlushCut {
    size_t wal_boundary;
    uint64_t prior_sequence;
    uint64_t sequence;
    bool checkpoint;
} FlushCut;

static bool keep_entry(const FDBManifestEntry *entry, size_t index, const FlushCut *cut)
{
    return entry->kind == FDB_FILE_SSTABLE ||
           (!cut->checkpoint && entry->kind == FDB_FILE_WAL && index >= cut->wal_boundary);
}

static FastDBStatus make_edit(const FDBWalSet *set, const FDBSSTProperties *props,
                               const FlushCut *cut, FDBManifestLoad *next)
{
    *next = (FDBManifestLoad){0};
    const FDBManifest *current = &set->catalog.view;
    size_t entries = 1;
    size_t key_bytes;
    if (!fdb_add_size(props->min_key.size, props->max_key.size, &key_bytes))
        return FASTDB_LIMIT_EXCEEDED;
    for (size_t i = 0; i < current->count; ++i) {
        const FDBManifestEntry *old = &current->entries[i];
        if (!keep_entry(old, i, cut))
            continue;
        if (!fdb_add_size(key_bytes, old->min_key.size, &key_bytes) ||
            !fdb_add_size(key_bytes, old->max_key.size, &key_bytes))
            return FASTDB_LIMIT_EXCEEDED;
        ++entries;
    }
    if (entries > set->catalog_limits.max_files || entries > SIZE_MAX / sizeof(*next->entries) ||
        key_bytes > set->catalog_limits.max_bytes)
        return FASTDB_LIMIT_EXCEEDED;
    next->entries = malloc(entries * sizeof(*next->entries));
    next->image = malloc(key_bytes == 0 ? 1 : key_bytes);
    if (next->entries == NULL || next->image == NULL) {
        fdb_manifest_release(next);
        return FASTDB_OUT_OF_MEMORY;
    }

    next->view = *current;
    ++next->view.generation;
    if (cut->sequence > next->view.sequence)
        next->view.sequence = cut->sequence;
    next->view.count = entries;
    next->view.entries = next->entries;
    unsigned char *key_position = next->image;
    size_t count = 0, added_index = 0;
    for (size_t i = 0; i < current->count; ++i) {
        if (keep_entry(&current->entries[i], i, cut)) {
            if (current->entries[i].id < props->id)
                ++added_index;
            next->entries[count] = current->entries[i];
            fdb_manifest_copy_bounds(&next->entries[count++], &key_position);
        }
    }
    memmove(&next->entries[added_index + 1], &next->entries[added_index],
            (count - added_index) * sizeof(*next->entries));
    FDBManifestEntry *added = &next->entries[added_index];
    *added = (FDBManifestEntry){
        .id = props->id, .kind = cut->checkpoint ? FDB_FILE_CHECKPOINT : FDB_FILE_SSTABLE,
        .bytes = props->file_bytes, .sequence_begin = props->min_sequence,
        .sequence_end = cut->sequence,
        .min_key = props->min_key, .max_key = props->max_key,
    };
    fdb_manifest_copy_bounds(added, &key_position);
    return FASTDB_OK;
}

static FastDBStatus write_tree(FDBSSTWriter *writer, const FDBTree *tree, bool checkpoint,
                                FDBSSTProperties *props)
{
    const FDBNode *node = fdb_tree_bound(tree, (FastDBSlice){0}, false);
    while (node != NULL) {
        FastDBStatus status = fdb_sst_writer_add(writer, &node->record);
        if (status != FASTDB_OK)
            return status;
        node = fdb_tree_bound(tree, node->record.key, true);
    }
    return checkpoint ? fdb_sst_writer_finish_checkpoint(writer, props) :
           fdb_sst_writer_finish(writer, props);
}

static FastDBStatus install_edit(FDBWalSet *set, FDBTree *tree, bool checkpoint,
                                   FDBManifestLoad *next)
{
    FastDBStatus status = fdb_manifest_publish(set->dir, &set->catalog_limits,
                                              set->catalog.view.generation, &next->view);
    if (status != FASTDB_OK)
        return status;

    status = fdb_wal_set_install_catalog(set, next);
    if (!checkpoint)
        fdb_tree_destroy(tree);
    return status;
}

static const FDBManifestEntry *first_wal(const FDBWalSet *set)
{
    for (size_t i = 0; i < set->catalog.view.count; ++i) {
        const FDBManifestEntry *entry = &set->catalog.view.entries[i];
        if (entry->kind == FDB_FILE_WAL)
            return entry;
    }
    return NULL;
}

static FastDBStatus select_wal_boundary(const FDBWalSet *set, FlushCut *cut)
{
    const FDBManifest *view = &set->catalog.view;
    for (size_t i = 0; i < view->count; ++i) {
        const FDBManifestEntry *entry = &view->entries[i];
        if (entry->kind != FDB_FILE_WAL)
            continue;
        uint64_t end = i == set->active_index ? set->wal.sequence : entry->sequence_end;
        if (entry->sequence_begin < cut->sequence && end == cut->sequence) {
            cut->wal_boundary = i + 1;
            return FASTDB_OK;
        }
        if (end > cut->sequence)
            break;
    }
    return FASTDB_INVALID_ARGUMENT;
}

FastDBStatus fdb_flush_build(const FDBDirectory *dir, const FDBSSTLimits *limits,
                              const FDBTree *tree, FDBFlushOutput *output)
{
    char temp[sizeof(".sst-18446744073709551615.tmp")];
    const char *prefix = output->checkpoint ? "chk" : "sst";
    (void)snprintf(temp, sizeof(temp), ".%s-%020" PRIu64 ".tmp", prefix, output->id);
    FastDBStatus status = fdb_sst_writer_open(dir, temp, output->id, limits, &output->writer);
    return status == FASTDB_OK ? write_tree(output->writer, tree, output->checkpoint,
                                            &output->properties) : status;
}

void fdb_flush_dispose(FDBFlushOutput *output)
{
    /* A finished writer is already closed, and build reports any error. */
    (void)fdb_sst_writer_destroy(output->writer);
    *output = (FDBFlushOutput){0};
}

FastDBStatus fdb_flush_commit(FDBWalSet *set, FDBTree *tree, const FDBFlushOutput *output)
{
    if (set->poisoned)
        return FASTDB_COMMIT_UNCERTAIN;
    FlushCut cut = {.sequence = output->sequence, .checkpoint = output->checkpoint};
    const FDBManifestEntry *first = first_wal(set);
    if (first == NULL || set->catalog.view.generation == UINT64_MAX)
        return first == NULL ? FASTDB_CORRUPTION : FASTDB_LIMIT_EXCEEDED;
    cut.prior_sequence = first->sequence_begin;
    FastDBStatus status = cut.checkpoint ? FASTDB_OK : select_wal_boundary(set, &cut);
    if (status == FASTDB_OK) {
        const FDBSSTProperties *props = &output->properties;
        bool covered = cut.checkpoint ? props->max_sequence <= cut.sequence && props->tombstones == 0 :
                       props->max_sequence == cut.sequence && props->min_sequence > cut.prior_sequence;
        if (!covered)
            status = FASTDB_CORRUPTION;
    }
    FDBManifestLoad next = {0};
    if (status == FASTDB_OK)
        status = make_edit(set, &output->properties, &cut, &next);
    if (status == FASTDB_OK) {
        char target[sizeof("sst-18446744073709551615")];
        char temp[sizeof(".sst-18446744073709551615.tmp")];
        const char *prefix = cut.checkpoint ? "chk" : "sst";
        (void)snprintf(target, sizeof(target), "%s-%020" PRIu64, prefix, output->id);
        (void)snprintf(temp, sizeof(temp), ".%s-%020" PRIu64 ".tmp", prefix, output->id);
        status = fdb_dir_rename_new(set->dir, temp, target);
    }
    if (status == FASTDB_OK)
        status = fdb_dir_sync(set->dir);
    if (status == FASTDB_OK)
        status = install_edit(set, tree, cut.checkpoint, &next);
    fdb_manifest_release(&next);
    if (status == FASTDB_COMMIT_UNCERTAIN)
        set->poisoned = true;
    return status;
}

FastDBStatus fdb_flush_reserve(FDBWalSet *set, FDBFlushOutput *output)
{
    if (set->poisoned)
        return FASTDB_COMMIT_UNCERTAIN;
    if (set->catalog.view.generation >= UINT64_MAX - 1)
        return FASTDB_LIMIT_EXCEEDED;
    FastDBStatus status = fdb_manifest_reserve_file(set->dir, &set->catalog_limits,
                                                   &set->catalog.view, &output->id);
    if (status == FASTDB_COMMIT_UNCERTAIN)
        set->poisoned = true;
    return status;
}

static FastDBStatus flush(FDBWalSet *set, FDBTree *tree, const FDBSSTLimits *limits,
                            FlushCut cut)
{
    if (set->poisoned)
        return FASTDB_COMMIT_UNCERTAIN;
    FDBStorageMode mode = cut.checkpoint ? FDB_MODE_MEMORY_PERSISTENT : FDB_MODE_LSM;
    if (set->catalog.view.mode != mode || cut.sequence > set->wal.sequence)
        return FASTDB_INVALID_ARGUMENT;
    const FDBManifestEntry *entry = first_wal(set);
    if (!cut.checkpoint && tree->count == 0) {
        bool empty = entry == NULL || entry->sequence_begin == cut.sequence;
        return cut.sequence == set->wal.sequence && empty ? FASTDB_OK : FASTDB_CORRUPTION;
    }
    if (cut.checkpoint && (entry == NULL || entry->sequence_begin == cut.sequence))
        return FASTDB_OK;
    if (entry == NULL || !fdb_sst_valid_limits(limits))
        return FASTDB_INVALID_ARGUMENT;
    if (!cut.checkpoint) {
        FastDBStatus selected = select_wal_boundary(set, &cut);
        if (selected != FASTDB_OK)
            return selected;
    }
    FDBFlushOutput output = {.sequence = cut.sequence, .checkpoint = cut.checkpoint};
    FastDBStatus status = fdb_flush_reserve(set, &output);
    if (status == FASTDB_OK)
        status = fdb_flush_build(set->dir, limits, tree, &output);
    if (status == FASTDB_OK)
        status = fdb_flush_commit(set, tree, &output);
    fdb_flush_dispose(&output);
    return status;
}

FastDBStatus fdb_flush_l0(FDBWalSet *set, FDBTree *tree, uint64_t sequence,
                           const FDBSSTLimits *limits)
{
    return flush(set, tree, limits, (FlushCut){.sequence = sequence});
}

FastDBStatus fdb_checkpoint_truncate_log(FDBWalSet *set, FDBTree *tree,
                                         const FDBSSTLimits *limits)
{
    return flush(set, tree, limits, (FlushCut){.sequence = set->wal.sequence, .checkpoint = true});
}
