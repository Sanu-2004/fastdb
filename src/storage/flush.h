#ifndef FASTDB_FLUSH_H
#define FASTDB_FLUSH_H

#include "memtable.h"
#include "sstable.h"
#include "wal_set.h"

typedef struct FDBFlushOutput {
    uint64_t id;
    uint64_t sequence;
    bool checkpoint;
    FDBSSTProperties properties;
    FDBSSTWriter *writer;
} FDBFlushOutput;

/* Reserve and commit under the catalog lock. Build reads only an immutable tree. */
FastDBStatus fdb_flush_reserve(FDBWalSet *set, FDBFlushOutput *output);
FastDBStatus fdb_flush_build(const FDBDirectory *dir, const FDBSSTLimits *limits,
                              const FDBTree *tree, FDBFlushOutput *output);
FastDBStatus fdb_flush_commit(FDBWalSet *set, FDBTree *tree, const FDBFlushOutput *output);
void fdb_flush_dispose(FDBFlushOutput *output);

/* Caller serializes access. The tree covers the oldest outstanding WALs through sequence. */
FastDBStatus fdb_flush_l0(FDBWalSet *set, FDBTree *tree, uint64_t sequence,
                           const FDBSSTLimits *limits);
FastDBStatus fdb_checkpoint_truncate_log(FDBWalSet *set, FDBTree *tree,
                                         const FDBSSTLimits *limits);

#endif
