#ifndef FASTDB_COMPACTION_PLAN_H
#define FASTDB_COMPACTION_PLAN_H

#include "manifest.h"

typedef struct FDBCompactionPolicy {
    uint32_t levels;
    size_t level0_file_trigger;
    uint64_t level1_bytes;
    uint32_t level_multiplier;
    size_t max_inputs;
    FastDBCompactionStrategy strategy;
} FDBCompactionPolicy;

typedef struct FDBCompactionPlan {
    uint32_t source_level;
    uint32_t destination_level;
    size_t count;
    /* In a staged job, keys above this inclusive boundary stay at source_level. */
    FastDBSlice split_after;
    /* Size-tiered soft output target uses the saturated sum of stored input bytes. */
    uint64_t input_bytes;
} FDBCompactionPlan;

bool fdb_compaction_policy_valid(const FDBCompactionPolicy *policy);
FastDBStatus fdb_compaction_check_levels(const FDBManifest *manifest, uint32_t levels);
/* Writes at most policy->max_inputs manifest indexes, with the source first. */
FastDBStatus fdb_compaction_plan(const FDBManifest *manifest, const FDBCompactionPolicy *policy,
                                  bool manual, const FastDBRange *range,
                                  size_t *inputs, FDBCompactionPlan *out);

#endif
