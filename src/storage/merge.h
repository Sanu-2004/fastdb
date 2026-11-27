#ifndef FASTDB_MERGE_H
#define FASTDB_MERGE_H

#include "sstable.h"

/*
 * Streams distinct key/revision pairs. The visitor may keep copies only.
 * The workspace limit includes readers, their cursors, and the merge heap.
 * The caller retains all readers and owns any unpublished output.
 */
FastDBStatus fdb_sst_merge(const FDBSSTReader *const *inputs, size_t count,
                           size_t max_working_bytes, FDBSSTVisit emit, void *context);

typedef struct FDBMergeLimits {
    FDBSSTLimits output_table;
    size_t max_inputs;
    size_t max_outputs;
    size_t max_working_bytes;
    uint64_t target_output_bytes;
    FastDBSlice split_after;
    bool discard_shadowed_puts;
} FDBMergeLimits;

typedef struct FDBMergeOutput {
    FastDBStatus (*reserve)(void *context, uint64_t *id);
    FastDBStatus (*finished)(void *context, const char *temporary_name,
                             const FDBSSTProperties *properties, bool suffix);
    void *context;
} FDBMergeOutput;

/*
 * Produces synced temporary tables. Callbacks reserve fresh IDs and may copy
 * metadata. The caller owns the outputs and publishes the complete job.
 */
FastDBStatus fdb_sst_merge_split(const FDBDirectory *dir, const FDBSSTReader *const *inputs,
                                 size_t count, const FDBMergeLimits *limits,
                                 const FDBMergeOutput *output);

#endif
