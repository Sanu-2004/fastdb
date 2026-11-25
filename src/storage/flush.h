#ifndef FASTDB_FLUSH_H
#define FASTDB_FLUSH_H

#include "memtable.h"
#include "sstable.h"
#include "wal_set.h"

/* Caller serializes access. The tree covers the oldest outstanding WALs through sequence. */
FastDBStatus fdb_flush_l0(FDBWalSet *set, FDBTree *tree, uint64_t sequence,
                           const FDBSSTLimits *limits);
FastDBStatus fdb_checkpoint_truncate_log(FDBWalSet *set, FDBTree *tree,
                                         const FDBSSTLimits *limits);

#endif
