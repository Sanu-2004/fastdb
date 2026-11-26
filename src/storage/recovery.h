#ifndef FASTDB_RECOVERY_H
#define FASTDB_RECOVERY_H

#include "sstable.h"
#include "wal_set.h"

/* The caller owns the locked directory and a provisional replay view until success. */
FastDBStatus fdb_lsm_recover(const FDBDirectory *dir, const FDBManifestLimits *manifest_limits,
                             const FDBWalLimits *wal_limits, const FDBSSTLimits *sst_limits,
                             const FDBWalReplay *replay, FDBWalSet *out);
FastDBStatus fdb_storage_reclaim_orphans(FDBWalSet *set);
FastDBStatus fdb_storage_prepare_maintenance(FDBWalSet *set);
FastDBStatus fdb_catalog_open_table(const FDBDirectory *dir, const FDBManifestEntry *entry,
                                    const FDBSSTLimits *limits, FDBSSTReader **out);
FastDBStatus fdb_checkpoint_recover(const FDBDirectory *dir,
                                    const FDBManifestLimits *manifest_limits,
                                    const FDBWalLimits *wal_limits, const FDBSSTLimits *sst_limits,
                                    const FDBWalReplay *replay, FDBSSTVisit restore, FDBWalSet *out);

#endif
