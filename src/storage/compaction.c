#include "compaction.h"
#include "recovery.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Build {
    FDBWalSet *set;
    const FDBCompactionPlan *plan;
    FDBManifestLoad candidate;
    size_t key_bytes;
    uint64_t next_output_id;
    uint64_t end_output_id;
} Build;

static FastDBStatus add_entry(Build *build, FDBManifestEntry entry)
{
    FDBManifestLoad *candidate = &build->candidate;
    size_t size;
    if (!fdb_add_size(build->key_bytes, entry.min_key.size, &size) ||
        !fdb_add_size(size, entry.max_key.size, &size) ||
        !fdb_manifest_can_fit(&build->set->catalog_limits, candidate->view.count + 1, size))
        return FASTDB_LIMIT_EXCEEDED;
    unsigned char *next = candidate->image + build->key_bytes;
    fdb_manifest_copy_bounds(&entry, &next);
    candidate->entries[candidate->view.count++] = entry;
    build->key_bytes = size;
    return FASTDB_OK;
}

static FastDBStatus take_output_id(void *context, uint64_t *id)
{
    Build *build = context;
    if (build->next_output_id == build->end_output_id)
        return FASTDB_LIMIT_EXCEEDED;
    *id = build->next_output_id++;
    return FASTDB_OK;
}

static FastDBStatus finished(void *context, const char *temporary_name,
                              const FDBSSTProperties *props, bool suffix)
{
    (void)temporary_name;
    Build *build = context;
    return add_entry(build, (FDBManifestEntry){
        .id = props->id, .kind = FDB_FILE_SSTABLE, .bytes = props->file_bytes,
        .level = suffix ? build->plan->source_level : build->plan->destination_level,
        .sequence_begin = props->min_sequence, .sequence_end = props->max_sequence,
        .min_key = props->min_key, .max_key = props->max_key,
    });
}

static bool selected(size_t index, const size_t *inputs, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (inputs[i] == index)
            return true;
    }
    return false;
}

static FastDBStatus allocate_candidate(Build *build)
{
    FDBWalSet *set = build->set;
    FDBManifestLoad *candidate = &build->candidate;
    const FDBManifestLimits *limits = &set->catalog_limits;
    if (limits->max_files > SIZE_MAX / sizeof(*candidate->entries))
        return FASTDB_LIMIT_EXCEEDED;
    candidate->entries = malloc(limits->max_files * sizeof(*candidate->entries));
    candidate->image = malloc(limits->max_bytes);
    if (candidate->image == NULL || candidate->entries == NULL)
        return FASTDB_OUT_OF_MEMORY;
    candidate->view = set->catalog.view;
    candidate->view.entries = candidate->entries;
    candidate->view.count = 0;
    return FASTDB_OK;
}

static FastDBStatus copy_retained(Build *build, const size_t *inputs)
{
    FDBWalSet *set = build->set;
    for (size_t i = 0; i < set->catalog.view.count; ++i) {
        if (selected(i, inputs, build->plan->count))
            continue;
        FastDBStatus status = add_entry(build, set->catalog.view.entries[i]);
        if (status != FASTDB_OK)
            return status;
    }
    return FASTDB_OK;
}

static FastDBStatus write_outputs(Build *build, const size_t *inputs, FDBMergeLimits limits,
                                   pthread_mutex_t *mutex)
{
    FDBWalSet *set = build->set;
    size_t count = build->plan->count;
    size_t outputs = set->catalog_limits.max_files - (set->catalog.view.count - count);
    if (limits.max_outputs > outputs)
        limits.max_outputs = outputs;
    uint64_t ids_left = UINT64_MAX - set->catalog.view.next_file_id;
    if (limits.max_outputs > ids_left)
        limits.max_outputs = (size_t)ids_left;
    if (limits.max_outputs == 0 || count > limits.max_working_bytes / sizeof(FDBSSTReader *) ||
        set->catalog.view.generation >= UINT64_MAX - 1)
        return FASTDB_LIMIT_EXCEEDED;
    limits.max_working_bytes -= count * sizeof(FDBSSTReader *);
    limits.split_after = build->plan->split_after;
    void *split_key = NULL;
    if (mutex != NULL && limits.split_after.size != 0) {
        split_key = malloc(limits.split_after.size);
        if (split_key == NULL)
            return FASTDB_OUT_OF_MEMORY;
        memcpy(split_key, limits.split_after.data, limits.split_after.size);
        limits.split_after.data = split_key;
    }
    const FDBSSTReader **readers = calloc(count, sizeof(*readers));
    if (readers == NULL) {
        free(split_key);
        return FASTDB_OUT_OF_MEMORY;
    }
    FastDBStatus status = fdb_manifest_reserve_files(set->dir, &set->catalog_limits, &set->catalog.view,
                                                     limits.max_outputs, &build->next_output_id);
    build->end_output_id = set->catalog.view.next_file_id;
    for (size_t i = 0; status == FASTDB_OK && i < count; ++i) {
        FDBSSTReader *reader;
        status = fdb_catalog_open_table(set->dir, &set->catalog.view.entries[inputs[i]],
                                         &limits.output_table, &reader);
        readers[i] = reader;
    }
    if (status == FASTDB_OK) {
        const FDBMergeOutput output = {take_output_id, finished, build};
        if (mutex != NULL)
            pthread_mutex_unlock(mutex);
        status = fdb_sst_merge_split(set->dir, readers, count, &limits, &output);
        if (mutex != NULL)
            pthread_mutex_lock(mutex);
        if (set->poisoned)
            status = FASTDB_COMMIT_UNCERTAIN;
        for (size_t i = 0; status == FASTDB_OK && i < count; ++i) {
            if (inputs[i] >= set->catalog.view.count ||
                set->catalog.view.entries[inputs[i]].id != fdb_sst_reader_properties(readers[i])->id)
                status = FASTDB_CONFLICT;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        FastDBStatus closed = fdb_sst_reader_close((FDBSSTReader *)readers[i]);
        if (status == FASTDB_OK)
            status = closed;
    }
    free(readers);
    free(split_key);
    return status;
}

static FastDBStatus rename_outputs(const Build *build, size_t count)
{
    const FDBManifest *candidate = &build->candidate.view;
    for (size_t i = 0; i < count; ++i) {
        char temporary[sizeof(".sst-18446744073709551615.tmp")];
        char final_name[sizeof("sst-18446744073709551615")];
        uint64_t id = candidate->entries[i].id;
        (void)snprintf(temporary, sizeof(temporary), ".sst-%020" PRIu64 ".tmp", id);
        (void)snprintf(final_name, sizeof(final_name), "sst-%020" PRIu64, id);
        FastDBStatus status = fdb_dir_rename_new(build->set->dir, temporary, final_name);
        if (status != FASTDB_OK)
            return status;
    }
    return fdb_dir_sync(build->set->dir);
}

static int order_by_id(const void *a, const void *b)
{
    uint64_t left = ((const FDBManifestEntry *)a)->id;
    uint64_t right = ((const FDBManifestEntry *)b)->id;
    return (left > right) - (left < right);
}

static FastDBStatus publish(Build *build, uint32_t levels, size_t outputs)
{
    FDBWalSet *set = build->set;
    FDBManifest *next = &build->candidate.view;
    uint64_t generation = set->catalog.view.generation;
    if (outputs == 0)
        return FASTDB_CORRUPTION;
    if (generation == UINT64_MAX)
        return FASTDB_LIMIT_EXCEEDED;
    next->generation = generation + 1;
    next->next_file_id = set->catalog.view.next_file_id;
    next->sequence = set->catalog.view.sequence;
    FastDBStatus status = fdb_compaction_check_levels(next, levels);
    if (status == FASTDB_OK)
        status = rename_outputs(build, outputs);
    if (status == FASTDB_OK)
        qsort(build->candidate.entries, next->count, sizeof(*next->entries), order_by_id);
    if (status == FASTDB_OK)
        status = fdb_manifest_publish(set->dir, &set->catalog_limits, generation, next);
    if (status != FASTDB_OK)
        return status;
    status = fdb_wal_set_install_catalog(set, &build->candidate);
    return status == FASTDB_OK ? fdb_storage_reclaim_orphans(set) : status;
}

FastDBStatus fdb_compact_once(FDBWalSet *set, const FDBCompactionPolicy *policy,
                               const FDBCompactionPlan *plan, const size_t *inputs,
                               const FDBMergeLimits *limits, pthread_mutex_t *mutex)
{
    if (set->poisoned)
        return FASTDB_COMMIT_UNCERTAIN;
    Build build = {.set = set, .plan = plan};
    FastDBStatus status = allocate_candidate(&build);
    if (status == FASTDB_OK)
        status = write_outputs(&build, inputs, *limits, mutex);
    size_t outputs = build.candidate.view.count;
    if (status == FASTDB_OK)
        status = copy_retained(&build, inputs);
    if (status == FASTDB_OK)
        status = publish(&build, policy->levels, outputs);
    if (status == FASTDB_COMMIT_UNCERTAIN)
        set->poisoned = true;
    fdb_manifest_release(&build.candidate);
    return status;
}
