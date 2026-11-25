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
    FASTDB_DURABILITY_ASYNC = 1
} FastDBDurability;

typedef enum FastDBMode {
    FASTDB_MODE_MEMORY = 0,
    FASTDB_MODE_MEMORY_PERSISTENT = 1,
    FASTDB_MODE_LSM = 2
} FastDBMode;

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
} FastDBPersistentOptions;

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

FASTDB_API const char *fastdb_version(void);
FASTDB_API const char *fastdb_status_string(FastDBStatus status);
FASTDB_API void fastdb_free(void *allocation);

FASTDB_API void fastdb_limits_default(FastDBLimits *limits);
FASTDB_API void fastdb_persistent_options_default(FastDBPersistentOptions *options);
FASTDB_API void fastdb_compaction_options_default(FastDBCompactionOptions *options);
FASTDB_API FastDBStatus fastdb_options_create(FastDBOptions **out);
FASTDB_API void fastdb_options_destroy(FastDBOptions *options);
FASTDB_API FastDBStatus fastdb_options_set_limits(FastDBOptions *options,
                                                  const FastDBLimits *limits);
/* Persistent modes use an existing directory. Memory mode requires NULL path and settings. */
FASTDB_API FastDBStatus fastdb_options_set_storage(FastDBOptions *options, FastDBMode mode,
                                                   const char *path,
                                                   const FastDBPersistentOptions *settings);
/* Set after selecting LSM storage. A zero workspace cap uses available engine memory. */
FASTDB_API FastDBStatus fastdb_options_set_compaction(FastDBOptions *options,
                                                      const FastDBCompactionOptions *settings);
/* LSM only. A nonzero active-tree byte threshold enables one background flush worker. */
FASTDB_API FastDBStatus fastdb_options_set_lsm_flush_threshold(FastDBOptions *options,
                                                               size_t memtable_bytes);
FASTDB_API FastDBStatus fastdb_open(const FastDBOptions *options, FastDB **out);
/* Quiesce operations before closing. BUSY leaves the handle open. Other results consume it. */
FASTDB_API FastDBStatus fastdb_close(FastDB *db);
FASTDB_API FastDBStatus fastdb_stats(FastDB *db, FastDBStats *out);
FASTDB_API FastDBStatus fastdb_flush(FastDB *db);
FASTDB_API FastDBStatus fastdb_checkpoint(FastDB *db);
/* Promote matching SSTables to the last level. Active WAL data is left in place. */
FASTDB_API FastDBStatus fastdb_compact(FastDB *db);
/* Select whole files intersecting a scan-style range. A NULL range selects all files. */
FASTDB_API FastDBStatus fastdb_compact_range(FastDB *db, const FastDBRange *range);

FASTDB_API FastDBStatus fastdb_put(FastDB *db, const void *key, size_t key_size,
                                  const void *value, size_t value_size);
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
FASTDB_API FastDBStatus fastdb_batch_delete(FastDBBatch *batch, const void *key,
                                           size_t key_size);
FASTDB_API FastDBStatus fastdb_batch_expect(FastDBBatch *batch, const void *key,
                                           size_t key_size, uint64_t expected);
FASTDB_API FastDBStatus fastdb_batch_commit(FastDB *db, const FastDBBatch *batch);

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
