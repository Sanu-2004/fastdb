#ifndef FASTDB_WAL_SET_H
#define FASTDB_WAL_SET_H

#include "manifest.h"
#include "wal.h"

typedef struct FDBWalSet {
    const FDBDirectory *dir;
    FDBManifestLimits catalog_limits;
    FDBManifestLoad catalog;
    FDBWal wal;
    size_t active_index;
    size_t corrupt_filter_partitions;
    size_t pinned_read_bytes;
    bool reclaim_pending;
    bool poisoned;
    bool compaction_outputs_reserved;
    uint64_t compaction_output_begin;
    uint64_t compaction_output_end;
} FDBWalSet;

/* covered_sequence is the verified sequence covered by durable tables or a checkpoint. */
FastDBStatus fdb_wal_set_open(const FDBDirectory *dir, const FDBManifestLimits *catalog_limits,
                              const FDBWalLimits *wal_limits, uint64_t covered_sequence,
                              const FDBWalReplay *replay, FDBWalSet *out);
FastDBStatus fdb_wal_set_append(FDBWalSet *set, const FDBRecord *records, size_t count, bool durable);
FastDBStatus fdb_wal_set_sync(FDBWalSet *set);
/* Ensure an empty active segment by sealing a nonempty predecessor if needed. */
FastDBStatus fdb_wal_set_rotate(FDBWalSet *set);
/* Consume a durable catalog edit, keeping the active WAL only if still referenced. */
FastDBStatus fdb_wal_set_install_catalog(FDBWalSet *set, FDBManifestLoad *next);
FastDBStatus fdb_wal_set_close(FDBWalSet *set);

#endif
