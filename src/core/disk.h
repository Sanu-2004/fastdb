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

typedef struct FDBDiskRead {
    FDBManifestEntry *entries;
    size_t count;
    uint64_t sequence;
} FDBDiskRead;

struct FastDBSnapshot {
    FastDB *db;
    FDBTree active;
    FDBTree frozen;
    FDBManifestLoad catalog;
    uint64_t revision;
    uint64_t read_time;
};

typedef struct FDBScanCandidate {
    FDBRecord record;
    const FDBManifestEntry *file;
} FDBScanCandidate;

FastDBStatus fdb_lookup_record(const FDBRecord *record, bool copy, FDBLookup *out);
FastDBStatus fdb_disk_validate(const FastDBOptions *options);
FastDBStatus fdb_disk_open(FastDB *db, const FastDBOptions *options, const FDBFs *fs,
                           const FDBWalReplay *replay, FDBSSTVisit restore);
FastDBStatus fdb_disk_close(FDBDisk *disk);
bool fdb_disk_poisoned(const FDBDisk *disk);
bool fdb_disk_has_tables(const FDBDisk *disk);
void fdb_disk_cache_stats(const FDBDisk *disk, FastDBBlockCacheStats *out);
size_t fdb_disk_arena_chunk_bytes(const FDBDisk *disk);
size_t fdb_disk_memtable_key_bytes(const FDBDisk *disk, bool frozen);
void fdb_disk_set_active_key_bytes(FDBDisk *disk, size_t bytes);
bool fdb_disk_tree_fits(const FDBDisk *disk, size_t keys, size_t key_bytes);
FastDBStatus fdb_disk_frame_size(const FDBDisk *disk, const FDBRecord *records,
                                 size_t count, size_t *bytes);
FastDBStatus fdb_disk_append(FDBDisk *disk, const FDBRecord *records, size_t count);
FastDBStatus fdb_disk_find(const FDBDisk *disk, FastDBSlice key, uint64_t sequence,
                           bool copy, FDBLookup *out);
const FDBManifest *fdb_disk_catalog(const FDBDisk *disk);
FastDBStatus fdb_disk_copy_tables(const FDBDisk *disk, FDBManifestLoad *out, size_t *headroom);
void fdb_disk_snapshot_catalog(FDBDisk *disk, const FDBManifest *catalog);
FastDBStatus fdb_disk_find_view(const FDBDisk *disk, const FDBManifest *catalog,
                                FastDBSlice key, uint64_t sequence, bool copy, FDBLookup *out);
FastDBStatus fdb_disk_scan_pick(const FDBDisk *disk, const FDBManifest *catalog, FastDBSlice lower,
                                bool exclusive, FastDBSlice upper, FastDBSlice prefix,
                                unsigned char *key_buffer, FDBScanCandidate *candidate);
FastDBStatus fdb_disk_scan_copy(const FDBDisk *disk, const FDBScanCandidate *candidate,
                                unsigned char *value);
size_t fdb_disk_pinned_bytes(const FDBDisk *disk);
bool fdb_disk_maintenance_waiting(const FDBDisk *disk);
/* Capture/release under the engine mutex. BUSY permits a serialized lookup instead. */
FastDBStatus fdb_disk_pin_read(FDBDisk *disk, FastDBSlice key, uint64_t sequence,
                               size_t headroom, FDBDiskRead *view);
FastDBStatus fdb_disk_find_pinned(const FDBDisk *disk, const FDBDiskRead *view, FastDBSlice key,
                                  bool copy, FDBLookup *out);
void fdb_disk_unpin_read(FDBDisk *disk, FDBDiskRead *view);
FastDBStatus fdb_disk_flush(FDBDisk *disk, FDBTree *tree);
FastDBStatus fdb_disk_retry_frozen(FDBDisk *disk, FDBTree *tree, uint64_t sequence);
FastDBStatus fdb_disk_freeze_wal(FDBDisk *disk);
FastDBStatus fdb_disk_flush_reserve(FDBDisk *disk, FDBFlushOutput *output);
FastDBStatus fdb_disk_flush_build(const FDBDisk *disk, const FDBTree *tree, FDBFlushOutput *output);
FastDBStatus fdb_disk_flush_commit(FDBDisk *disk, FDBTree *tree, const FDBFlushOutput *output);
size_t fdb_disk_compaction_bytes(const FDBDisk *disk);
FastDBStatus fdb_disk_compact(FastDB *db, bool manual, bool background,
                               const FastDBRange *range);
FastDBStatus fdb_disk_checkpoint(FDBDisk *disk, FDBTree *tree);
FastDBStatus fdb_disk_maybe_checkpoint(FDBDisk *disk, FDBTree *tree, size_t frame_bytes);

#endif
