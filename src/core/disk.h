#ifndef FASTDB_DISK_H
#define FASTDB_DISK_H

#include "engine.h"
#include "flush.h"
#include "storage/sstable.h"
#include "wal.h"

typedef struct FDBLookup {
    void *value;
    size_t size;
    uint64_t revision;
    uint64_t expires_at_ms;
    FDBRecordKind kind;
} FDBLookup;

FastDBStatus fdb_lookup_record(const FDBRecord *record, bool copy, FDBLookup *out);
FastDBStatus fdb_disk_validate(const FastDBOptions *options);
FastDBStatus fdb_disk_open(FastDB *db, const FastDBOptions *options, const FDBFs *fs,
                           const FDBWalReplay *replay, FDBSSTVisit restore);
FastDBStatus fdb_disk_close(FDBDisk *disk);
bool fdb_disk_poisoned(const FDBDisk *disk);
bool fdb_disk_has_tables(const FDBDisk *disk);
bool fdb_disk_tree_fits(const FDBDisk *disk, size_t keys, size_t key_bytes);
FastDBStatus fdb_disk_frame_size(const FDBDisk *disk, const FDBRecord *records,
                                 size_t count, size_t *bytes);
FastDBStatus fdb_disk_append(FDBDisk *disk, const FDBRecord *records, size_t count);
FastDBStatus fdb_disk_find(const FDBDisk *disk, FastDBSlice key, uint64_t sequence,
                           bool copy, FDBLookup *out);
FastDBStatus fdb_disk_flush(FDBDisk *disk, FDBTree *tree);
FastDBStatus fdb_disk_retry_frozen(FDBDisk *disk, FDBTree *tree, uint64_t sequence);
FastDBStatus fdb_disk_rotate_wal(FDBDisk *disk);
FastDBStatus fdb_disk_flush_reserve(FDBDisk *disk, FDBFlushOutput *output);
FastDBStatus fdb_disk_flush_build(const FDBDisk *disk, const FDBTree *tree, FDBFlushOutput *output);
FastDBStatus fdb_disk_flush_commit(FDBDisk *disk, FDBTree *tree, const FDBFlushOutput *output);
FastDBStatus fdb_disk_compact(FDBDisk *disk, bool manual, const FastDBRange *range, size_t headroom);
FastDBStatus fdb_disk_checkpoint(FDBDisk *disk, FDBTree *tree);
FastDBStatus fdb_disk_maybe_checkpoint(FDBDisk *disk, FDBTree *tree, size_t frame_bytes);

#endif
