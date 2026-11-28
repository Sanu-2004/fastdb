#ifndef FASTDB_FASTDB_H
#define FASTDB_FASTDB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define FASTDB_API __attribute__((visibility("default")))
#else
#define FASTDB_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define FASTDB_ABI_VERSION 1
#define FASTDB_EXPECT_ABSENT UINT64_C(0)

typedef struct FastDB FastDB;
typedef struct FastDBOptions FastDBOptions;
typedef struct FastDBBatch FastDBBatch;
typedef struct FastDBIterator FastDBIterator;
typedef struct FastDBSnapshot FastDBSnapshot;

typedef enum FastDBStatus {
    FASTDB_OK = 0,
    FASTDB_NOT_FOUND = 1,
    FASTDB_INVALID_ARGUMENT = 2,
    FASTDB_OUT_OF_MEMORY = 3,
    FASTDB_IO_ERROR = 4,
    FASTDB_CORRUPTION = 5,
    FASTDB_BUSY = 6,
    FASTDB_CONFLICT = 7,
    FASTDB_LIMIT_EXCEEDED = 8,
    FASTDB_UNSUPPORTED = 9,
    FASTDB_CLOSED = 10,
    FASTDB_COMMIT_UNCERTAIN = 11
} FastDBStatus;

typedef struct FastDBLimits {
    size_t max_key_bytes;
    size_t max_value_bytes;
    size_t max_memory_bytes;
    size_t max_batch_operations;
    size_t max_batch_bytes;
    size_t max_scan_records;
    size_t max_scan_bytes;
    size_t max_open_iterators;
} FastDBLimits;

typedef enum FastDBDurability {
    FASTDB_DURABILITY_SYNC = 0,
    FASTDB_DURABILITY_ASYNC = 1,
    FASTDB_DURABILITY_INTERVAL = 2,
    FASTDB_DURABILITY_GROUP_COMMIT = 3
} FastDBDurability;

typedef enum FastDBCompression {
    FASTDB_COMPRESSION_NONE = 0,
    FASTDB_COMPRESSION_LZ4 = 1,
    FASTDB_COMPRESSION_ZSTD = 2
} FastDBCompression;

typedef enum FastDBMode {
    FASTDB_MODE_MEMORY = 0,
    FASTDB_MODE_MEMORY_PERSISTENT = 1,
    FASTDB_MODE_LSM = 2
} FastDBMode;

typedef enum FastDBStorageProfile {
    FASTDB_PROFILE_CACHE = 0,
    FASTDB_PROFILE_BALANCED = 1,
    FASTDB_PROFILE_READ_OPTIMIZED = 2,
    FASTDB_PROFILE_WRITE_OPTIMIZED = 3,
    FASTDB_PROFILE_DURABLE = 4,
    FASTDB_PROFILE_LOW_MEMORY = 5
} FastDBStorageProfile;

typedef enum FastDBEvictionPolicy {
    FASTDB_EVICTION_NONE = 0,
    FASTDB_EVICTION_FIFO = 1,
    FASTDB_EVICTION_LRU = 2,
    FASTDB_EVICTION_LFU = 3
} FastDBEvictionPolicy;

typedef struct FastDBPersistentOptions {
    FastDBDurability durability;
    uint64_t max_wal_segment_bytes;
    size_t max_wal_frame_bytes;
    size_t target_block_bytes;
    size_t max_index_bytes;
    size_t max_filter_bytes;
    size_t max_catalog_files;
    size_t max_catalog_bytes;
    unsigned int bloom_bits_per_key;
    size_t checkpoint_wal_segments;
    /* Keep at least this many bytes free after each persistent write. Zero disables checks. */
    uint64_t min_free_bytes;
    /* Descriptor budget. LSM requires at least 10 and persistent memory at least 5. */
    size_t max_open_files;
} FastDBPersistentOptions;

typedef struct FastDBGroupCommitOptions {
    size_t max_pending_requests;
    size_t max_pending_bytes;
    uint32_t collection_delay_ms;
} FastDBGroupCommitOptions;

typedef enum FastDBCompactionStrategy {
    FASTDB_COMPACTION_LEVELED = 0,
    FASTDB_COMPACTION_SIZE_TIERED = 1
} FastDBCompactionStrategy;

typedef struct FastDBCompactionOptions {
    bool enabled;
    uint32_t max_levels;
    size_t level0_file_trigger;
    uint64_t level1_bytes;
    uint32_t level_multiplier;
    size_t max_merge_inputs;
    size_t max_working_bytes;
    uint64_t target_file_bytes;
} FastDBCompactionOptions;

typedef struct FastDBSlice {
    const void *data;
    size_t size;
} FastDBSlice;

typedef struct FastDBRange {
    FastDBSlice start;
    FastDBSlice end;
    bool start_exclusive;
} FastDBRange;

typedef struct FastDBStats {
    uint64_t revision;
    /* Current in-memory entries, including active and frozen LSM tombstones. */
    size_t record_count;
    /* Charged bytes, including reserved persistent storage workspace. */
    size_t memory_bytes;
    size_t iterator_bytes;
    size_t open_iterators;
} FastDBStats;

typedef struct FastDBBlockCacheStats {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    /* Actual cache allocations and the reserved capacity in engine memory. */
    size_t memory_bytes;
    size_t max_bytes;
    size_t entries;
} FastDBBlockCacheStats;

enum { FASTDB_STORAGE_MAX_LEVELS = 32 };

typedef struct FastDBStorageLevelStats {
    size_t files;
    uint64_t bytes;
} FastDBStorageLevelStats;

/* Cumulative per open handle. Byte totals saturate. */
typedef struct FastDBStorageActivity {
    uint64_t wal_frame_bytes_appended;
    uint64_t flush_files_published;
    uint64_t flush_output_bytes;
    uint64_t checkpoints_published;
    uint64_t checkpoint_output_bytes;
    uint64_t compaction_jobs_published;
    uint64_t compaction_job_errors;
    uint64_t compaction_input_bytes;
    uint64_t compaction_output_bytes;
} FastDBStorageActivity;

typedef struct FastDBStorageStats {
    FastDBMode mode;
    size_t active_memtable_bytes;
    size_t frozen_memtable_bytes;
    size_t snapshot_bytes;
    size_t reserved_engine_bytes;
    size_t compaction_workspace_bytes;
    uint64_t catalog_generation;
    size_t wal_files;
    uint64_t wal_bytes;
    uint64_t active_wal_bytes;
    size_t checkpoint_files;
    uint64_t checkpoint_bytes;
    size_t sstable_files;
    uint64_t sstable_bytes;
    uint32_t level_count;
    FastDBStorageLevelStats levels[FASTDB_STORAGE_MAX_LEVELS];
    FastDBStorageActivity activity;
} FastDBStorageStats;

FASTDB_API const char *fastdb_version(void);
FASTDB_API const char *fastdb_status_string(FastDBStatus status);
FASTDB_API void fastdb_free(void *allocation);

FASTDB_API void fastdb_limits_default(FastDBLimits *limits);
FASTDB_API void fastdb_persistent_options_default(FastDBPersistentOptions *options);
FASTDB_API void fastdb_group_commit_options_default(FastDBGroupCommitOptions *options);
FASTDB_API void fastdb_compaction_options_default(FastDBCompactionOptions *options);
FASTDB_API FastDBStatus fastdb_options_create(FastDBOptions **out);
FASTDB_API void fastdb_options_destroy(FastDBOptions *options);
FASTDB_API FastDBStatus fastdb_options_set_limits(FastDBOptions *options,
                                                  const FastDBLimits *limits);
/* Memory-only live tree capacity in charged bytes. NONE requires zero. */
FASTDB_API FastDBStatus fastdb_options_set_eviction(FastDBOptions *options,
                                                    FastDBEvictionPolicy policy,
                                                    size_t max_live_bytes);
/* Persistent modes use an existing directory. Memory mode requires NULL path and settings. */
FASTDB_API FastDBStatus fastdb_options_set_storage(FastDBOptions *options, FastDBMode mode,
                                                   const char *path,
                                                   const FastDBPersistentOptions *settings);
/* Replace options with a validated preset. Apply overrides afterward. */
FASTDB_API FastDBStatus fastdb_options_set_profile(FastDBOptions *options, FastDBStorageProfile profile,
                                                   FastDBMode mode, const char *path);
/* Persistent modes only. Enable v2 partitioned output on the next open. */
FASTDB_API FastDBStatus fastdb_options_set_partitioned_sstables(FastDBOptions *options, bool enabled);
/* Optional codecs require a codec-enabled build and select v2 for new output. */
FASTDB_API FastDBStatus fastdb_options_set_sstable_compression(FastDBOptions *options,
                                                               FastDBCompression compression);
/* Interval durability only. Positive milliseconds, default 100 when unset. */
FASTDB_API FastDBStatus fastdb_options_set_wal_interval(FastDBOptions *options, uint32_t milliseconds);
/* Group commit only. Zero collection delay processes requests already queued. */
FASTDB_API FastDBStatus fastdb_options_set_group_commit(FastDBOptions *options,
                                                        const FastDBGroupCommitOptions *settings);
/* Set after selecting LSM storage. A zero workspace cap uses available engine memory. */
FASTDB_API FastDBStatus fastdb_options_set_compaction(FastDBOptions *options,
                                                      const FastDBCompactionOptions *settings);
/* LSM only. Available after selecting LSM storage and applied on open. */
FASTDB_API FastDBStatus fastdb_options_set_lsm_compaction_strategy(FastDBOptions *options,
                                                                    FastDBCompactionStrategy strategy);
/* LSM only. A nonzero active-tree byte threshold enables one background flush worker. */
FASTDB_API FastDBStatus fastdb_options_set_lsm_flush_threshold(FastDBOptions *options,
                                                               size_t memtable_bytes);
/* LSM only. Zero uses individual node allocation; otherwise sets the arena chunk payload. */
FASTDB_API FastDBStatus fastdb_options_set_lsm_arena(FastDBOptions *options, size_t chunk_bytes);
/* LSM only. Reserve max_bytes for verified data blocks. Zero disables the cache. */
FASTDB_API FastDBStatus fastdb_options_set_lsm_block_cache(FastDBOptions *options, size_t max_bytes);
FASTDB_API FastDBStatus fastdb_open(const FastDBOptions *options, FastDB **out);
/* Quiesce operations before close. BUSY leaves the handle open. Other results consume it. */
FASTDB_API FastDBStatus fastdb_close(FastDB *db);
FASTDB_API FastDBStatus fastdb_stats(FastDB *db, FastDBStats *out);
/* Capture charged storage memory, catalog inventory and maintenance activity under one lock. */
FASTDB_API FastDBStatus fastdb_storage_stats(FastDB *db, FastDBStorageStats *out);
/* Reports zero fields when the cache is disabled. Counters reset on open. */
FASTDB_API FastDBStatus fastdb_block_cache_stats(FastDB *db, FastDBBlockCacheStats *out);
FASTDB_API FastDBStatus fastdb_flush(FastDB *db);
FASTDB_API FastDBStatus fastdb_checkpoint(FastDB *db);
/* Promote matching SSTables to the last level. Active WAL data is left in place. */
FASTDB_API FastDBStatus fastdb_compact(FastDB *db);
/* Select whole files intersecting a scan-style range. A NULL range selects all files. */
FASTDB_API FastDBStatus fastdb_compact_range(FastDB *db, const FastDBRange *range);

FASTDB_API FastDBStatus fastdb_put(FastDB *db, const void *key, size_t key_size,
                                  const void *value, size_t value_size);
/* Zero TTL is permanent. Positive TTL is measured in milliseconds from commit staging. */
FASTDB_API FastDBStatus fastdb_put_with_ttl(FastDB *db, const void *key, size_t key_size,
                                           const void *value, size_t value_size, uint64_t ttl_ms);
/* Remove up to limit expired keys in the memory modes. Reports completed deletions on error. */
FASTDB_API FastDBStatus fastdb_purge_expired(FastDB *db, size_t limit, size_t *removed);
FASTDB_API FastDBStatus fastdb_delete(FastDB *db, const void *key, size_t key_size);
/* A successful GET returns a caller-owned allocation, even for an empty value. */
FASTDB_API FastDBStatus fastdb_get(FastDB *db, const void *key, size_t key_size,
                                  void **value, size_t *value_size);
FASTDB_API FastDBStatus fastdb_get_with_revision(FastDB *db, const void *key,
                                                size_t key_size, void **value,
                                                size_t *value_size, uint64_t *revision);
FASTDB_API FastDBStatus fastdb_exists(FastDB *db, const void *key, size_t key_size,
                                     bool *exists);
FASTDB_API FastDBStatus fastdb_compare_and_swap(FastDB *db, const void *key,
                                               size_t key_size, uint64_t expected,
                                               const void *value, size_t value_size);

/* Batches own copies of inputs. All checks observe the state before any mutation. */
FASTDB_API FastDBStatus fastdb_batch_create(const FastDBOptions *options, FastDBBatch **out);
FASTDB_API void fastdb_batch_destroy(FastDBBatch *batch);
FASTDB_API FastDBStatus fastdb_batch_put(FastDBBatch *batch, const void *key, size_t key_size,
                                        const void *value, size_t value_size);
FASTDB_API FastDBStatus fastdb_batch_put_with_ttl(FastDBBatch *batch, const void *key, size_t key_size,
                                                 const void *value, size_t value_size, uint64_t ttl_ms);
FASTDB_API FastDBStatus fastdb_batch_delete(FastDBBatch *batch, const void *key,
                                           size_t key_size);
FASTDB_API FastDBStatus fastdb_batch_expect(FastDBBatch *batch, const void *key,
                                           size_t key_size, uint64_t expected);
FASTDB_API FastDBStatus fastdb_batch_commit(FastDB *db, const FastDBBatch *batch);

/* One immutable snapshot per database, charged to max_memory_bytes.
 * Release it before closing the database. */
FASTDB_API FastDBStatus fastdb_snapshot_create(FastDB *db, FastDBSnapshot **out);
/* The capture revision, or zero for a NULL snapshot. */
FASTDB_API uint64_t fastdb_snapshot_revision(const FastDBSnapshot *snapshot);
/* Returns a caller-owned value and the record's original revision. */
FASTDB_API FastDBStatus fastdb_snapshot_get(const FastDBSnapshot *snapshot, const void *key,
                                           size_t key_size, void **value, size_t *value_size,
                                           uint64_t *revision);
/* Quiesce readers of this snapshot before releasing it. */
FASTDB_API void fastdb_snapshot_release(FastDBSnapshot *snapshot);
FASTDB_API FastDBStatus fastdb_snapshot_scan_range(const FastDBSnapshot *snapshot,
                                                  const FastDBRange *range, size_t limit,
                                                  FastDBIterator **out);
FASTDB_API FastDBStatus fastdb_snapshot_scan_prefix(const FastDBSnapshot *snapshot,
                                                   FastDBSlice prefix, size_t limit,
                                                   FastDBIterator **out);

/* A NULL/zero slice leaves a range bound open. The upper bound is exclusive. */
FASTDB_API FastDBStatus fastdb_scan_range(FastDB *db, const FastDBRange *range,
                                         size_t limit, FastDBIterator **out);
FASTDB_API FastDBStatus fastdb_scan_prefix(FastDB *db, FastDBSlice prefix,
                                          size_t limit, FastDBIterator **out);
/* Record slices stay valid until iterator close. NOT_FOUND indicates end of page. */
FASTDB_API FastDBStatus fastdb_iterator_next(FastDBIterator *iterator, FastDBSlice *key,
                                            FastDBSlice *value, uint64_t *revision);
FASTDB_API bool fastdb_iterator_has_more(const FastDBIterator *iterator);
FASTDB_API void fastdb_iterator_close(FastDBIterator *iterator);

#ifdef __cplusplus
}
#endif

#endif
