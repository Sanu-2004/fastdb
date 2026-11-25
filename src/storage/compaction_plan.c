#include "compaction_plan.h"
#include "record.h"

bool fdb_compaction_policy_valid(const FDBCompactionPolicy *policy)
{
    return policy != NULL && policy->levels >= 2 &&
           policy->levels <= FDB_MAX_COMPACTION_LEVELS && policy->level0_file_trigger != 0 &&
           policy->level1_bytes != 0 && policy->level_multiplier >= 2 && policy->max_inputs >= 2;
}

static bool overlap(const FDBManifestEntry *a, const FDBManifestEntry *b)
{
    return fdb_compare(a->min_key, b->max_key) <= 0 &&
           fdb_compare(b->min_key, a->max_key) <= 0;
}

FastDBStatus fdb_compaction_check_levels(const FDBManifest *manifest, uint32_t levels)
{
    for (size_t i = 0; i < manifest->count; ++i) {
        const FDBManifestEntry *file = &manifest->entries[i];
        if (file->kind != FDB_FILE_SSTABLE)
            continue;
        if (file->level >= levels || file->min_key.data == NULL || file->min_key.size == 0 ||
            file->max_key.data == NULL || file->max_key.size == 0 ||
            fdb_compare(file->min_key, file->max_key) > 0)
            return FASTDB_CORRUPTION;
        if (file->level == 0)
            continue;
        for (size_t j = 0; j < i; ++j) {
            const FDBManifestEntry *previous = &manifest->entries[j];
            if (previous->kind == FDB_FILE_SSTABLE && previous->level == file->level &&
                overlap(previous, file))
                return FASTDB_CORRUPTION;
        }
    }
    return FASTDB_OK;
}

static bool in_range(const FDBManifestEntry *file, const FastDBRange *range)
{
    if (range == NULL)
        return true;
    if (range->start.size != 0) {
        int order = fdb_compare(file->max_key, range->start);
        if (order < 0 || (order == 0 && range->start_exclusive))
            return false;
    }
    return range->end.size == 0 || fdb_compare(file->min_key, range->end) < 0;
}

static uint64_t saturating_add(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

static size_t choose_source(const FDBManifest *manifest, const FDBCompactionPolicy *policy,
                             bool manual, const FastDBRange *range)
{
    uint64_t bytes[FDB_MAX_COMPACTION_LEVELS] = {0};
    uint64_t max_bytes[FDB_MAX_COMPACTION_LEVELS] = {0};
    size_t candidate[FDB_MAX_COMPACTION_LEVELS] = {0};
    size_t level0_count = 0;
    max_bytes[1] = policy->level1_bytes;
    for (uint32_t i = 2; i < policy->levels; ++i) {
        uint64_t previous = max_bytes[i - 1];
        max_bytes[i] = previous > UINT64_MAX / policy->level_multiplier ?
                       UINT64_MAX : previous * policy->level_multiplier;
    }
    for (size_t i = manifest->count; i != 0; --i) {
        const FDBManifestEntry *file = &manifest->entries[i - 1];
        if (file->kind != FDB_FILE_SSTABLE || file->level == policy->levels - 1)
            continue;
        bytes[file->level] = saturating_add(bytes[file->level], file->bytes);
        if (file->level == 0)
            ++level0_count;
        if (in_range(file, range))
            candidate[file->level] = i;
    }
    /* Relieve deep destinations first. The last level never auto-promotes. */
    for (uint32_t i = policy->levels - 1; i != 0; --i) {
        uint32_t level = i - 1;
        bool over = level == 0 ? level0_count >= policy->level0_file_trigger :
                                 bytes[level] > max_bytes[level];
        if ((manual || over) && candidate[level] != 0)
            return candidate[level] - 1;
    }
    return SIZE_MAX;
}

FastDBStatus fdb_compaction_plan(const FDBManifest *manifest, const FDBCompactionPolicy *policy,
                                  bool manual, const FastDBRange *range,
                                  size_t *inputs, FDBCompactionPlan *out)
{
    *out = (FDBCompactionPlan){0};
    if (manifest == NULL || inputs == NULL || !fdb_compaction_policy_valid(policy) ||
        (range != NULL && (!manual ||
         (range->start.size != 0 && range->start.data == NULL) ||
         (range->end.size != 0 && range->end.data == NULL))))
        return FASTDB_INVALID_ARGUMENT;
    if (manifest->mode != FDB_MODE_LSM)
        return FASTDB_UNSUPPORTED;
    FastDBStatus status = fdb_compaction_check_levels(manifest, policy->levels);
    if (status != FASTDB_OK)
        return status;
    if (range != NULL && range->start.size != 0 && range->end.size != 0 &&
        fdb_compare(range->start, range->end) >= 0)
        return FASTDB_OK;

    size_t source_index = choose_source(manifest, policy, manual, range);
    if (source_index == SIZE_MAX)
        return FASTDB_OK;
    const FDBManifestEntry *source = &manifest->entries[source_index];
    uint32_t destination = source->level + 1;
    size_t count = 1;
    size_t first = SIZE_MAX;
    bool staged = false;
    inputs[0] = source_index;
    for (size_t i = 0; i < manifest->count; ++i) {
        const FDBManifestEntry *file = &manifest->entries[i];
        if (file->kind != FDB_FILE_SSTABLE || file->level != destination || !overlap(source, file))
            continue;
        if (first == SIZE_MAX || fdb_compare(file->min_key, manifest->entries[first].min_key) < 0)
            first = i;
        if (count < policy->max_inputs)
            inputs[count++] = i;
        else
            staged = true;
    }
    *out = (FDBCompactionPlan){.source_level = source->level, .destination_level = destination,
                               .count = count};
    if (staged) {
        inputs[1] = first;
        out->count = 2;
        out->split_after = manifest->entries[first].max_key;
    }
    return FASTDB_OK;
}
