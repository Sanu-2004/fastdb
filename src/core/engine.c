#include "disk.h"
#include "block_codec.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef enum OperationKind { PUT, DELETE, EXPECT } OperationKind;

typedef struct Operation {
    struct Operation *next;
    OperationKind kind;
    FastDBSlice key;
    FastDBSlice value;
    union {
        uint64_t expected;
        uint64_t ttl_ms;
    };
} Operation;

typedef struct BatchEntry {
    Operation op;
    unsigned char bytes[];
} BatchEntry;

struct FastDBBatch {
    FastDBLimits limits;
    Operation *first;
    Operation **tail;
    size_t count;
    size_t bytes;
};

typedef struct StagedWrite {
    FDBRecord record;
    size_t bytes;
    FDBNode *node;
} StagedWrite;

typedef struct StagedTree {
    FDBArenaWrite arena;
    size_t key_bytes;
} StagedTree;

typedef struct Eviction {
    FastDBEvictionPolicy policy;
    size_t limit;
    uint64_t tick;
} Eviction;

typedef struct EvictionRecord {
    uint64_t inserted;
    uint64_t touched;
    uint64_t hits;
} EvictionRecord;

_Static_assert(_Alignof(FastDB) >= _Alignof(Eviction), "cache state alignment");

static bool eviction_enabled(const FastDB *db)
{
    return db->disk == NULL && db->reserved_bytes == sizeof(Eviction);
}

static Eviction *eviction_state(FastDB *db)
{
    return eviction_enabled(db) ? (Eviction *)(db + 1) : NULL;
}

static size_t live_tree_bytes(const FastDB *db)
{
    size_t metadata = eviction_enabled(db) ? db->tree.count * sizeof(EvictionRecord) : 0;
    return fdb_tree_memory_bytes(&db->tree) + metadata;
}

static bool valid_eviction(const FastDBOptions *options)
{
    if (options->eviction_policy == FASTDB_EVICTION_NONE)
        return options->max_live_bytes == 0;
    const size_t base = sizeof(FastDB) + sizeof(Eviction);
    return options->eviction_policy >= FASTDB_EVICTION_FIFO &&
           options->eviction_policy <= FASTDB_EVICTION_LFU &&
           options->mode == FASTDB_MODE_MEMORY && options->path == NULL &&
           options->limits.max_memory_bytes >= base &&
           options->max_live_bytes >= sizeof(FDBNode) + 1 + sizeof(EvictionRecord) &&
           options->max_live_bytes <= options->limits.max_memory_bytes - base;
}

static FastDBStatus replay_frame(void *context, const unsigned char *payload, size_t size,
                                  size_t count, uint64_t first);
static size_t replay_headroom(void *context);
static FastDBStatus restore_checkpoint(void *context, const FDBRecord *record);
static size_t write_headroom(const FastDB *db);

static const FastDBLimits defaults = {
    .max_key_bytes = 64 * 1024,
    .max_value_bytes = 16 * 1024 * 1024,
    .max_memory_bytes = 64 * 1024 * 1024,
    .max_batch_operations = 10000,
    .max_batch_bytes = 32 * 1024 * 1024,
    .max_scan_records = 10000,
    .max_scan_bytes = 32 * 1024 * 1024,
    .max_open_iterators = 16,
};

const char *fastdb_version(void)
{
    return FASTDB_VERSION_STRING;
}

const char *fastdb_status_string(FastDBStatus status)
{
    switch (status) {
    case FASTDB_OK: return "ok";
    case FASTDB_NOT_FOUND: return "not found";
    case FASTDB_INVALID_ARGUMENT: return "invalid argument";
    case FASTDB_OUT_OF_MEMORY: return "out of memory";
    case FASTDB_IO_ERROR: return "I/O error";
    case FASTDB_CORRUPTION: return "corruption";
    case FASTDB_BUSY: return "busy";
    case FASTDB_CONFLICT: return "conflict";
    case FASTDB_LIMIT_EXCEEDED: return "limit exceeded";
    case FASTDB_UNSUPPORTED: return "unsupported";
    case FASTDB_CLOSED: return "closed";
    case FASTDB_COMMIT_UNCERTAIN: return "commit outcome uncertain";
    }
    return "unknown status";
}

void fastdb_free(void *allocation)
{
    free(allocation);
}

void fastdb_limits_default(FastDBLimits *limits)
{
    if (limits != NULL)
        *limits = defaults;
}

FastDBStatus fastdb_options_create(FastDBOptions **out)
{
    if (out == NULL)
        return FASTDB_INVALID_ARGUMENT;
    *out = calloc(1, sizeof(**out));
    if (*out == NULL)
        return FASTDB_OUT_OF_MEMORY;
    (*out)->limits = defaults;
    fastdb_compaction_options_default(&(*out)->compaction);
    fastdb_group_commit_options_default(&(*out)->group_commit);
    return FASTDB_OK;
}

void fastdb_options_destroy(FastDBOptions *options)
{
    if (options != NULL)
        free(options->path);
    free(options);
}

FastDBStatus fastdb_options_set_limits(FastDBOptions *options, const FastDBLimits *limits)
{
    if (options == NULL || limits == NULL ||
        limits->max_key_bytes == 0 || limits->max_key_bytes > UINT32_MAX ||
        limits->max_value_bytes > UINT32_MAX || limits->max_memory_bytes < sizeof(FastDB) ||
        limits->max_batch_operations == 0 ||
        limits->max_batch_bytes < sizeof(FastDBBatch) + sizeof(BatchEntry) + 1 ||
        limits->max_scan_records == 0 || limits->max_scan_bytes < sizeof(FastDBIterator) ||
        limits->max_open_iterators == 0)
        return FASTDB_INVALID_ARGUMENT;
    FastDBOptions next = *options;
    next.limits = *limits;
    if (!valid_eviction(&next) || (next.path != NULL && fdb_disk_validate(&next) != FASTDB_OK))
        return FASTDB_INVALID_ARGUMENT;
    options->limits = *limits;
    return FASTDB_OK;
}

FastDBStatus fastdb_options_set_eviction(FastDBOptions *options, FastDBEvictionPolicy policy,
                                          size_t max_live_bytes)
{
    if (options == NULL)
        return FASTDB_INVALID_ARGUMENT;
    FastDBOptions next = *options;
    next.eviction_policy = policy;
    next.max_live_bytes = max_live_bytes;
    if (!valid_eviction(&next))
        return FASTDB_INVALID_ARGUMENT;
    options->eviction_policy = policy;
    options->max_live_bytes = max_live_bytes;
    return FASTDB_OK;
}

FastDBStatus fastdb_options_set_storage(FastDBOptions *options, FastDBMode mode, const char *path,
                                          const FastDBPersistentOptions *settings)
{
    if (options == NULL)
        return FASTDB_INVALID_ARGUMENT;
    if (mode == FASTDB_MODE_MEMORY) {
        if (path != NULL || settings != NULL)
            return FASTDB_INVALID_ARGUMENT;
        free(options->path);
        options->path = NULL;
        options->mode = mode;
        options->storage = (FastDBPersistentOptions){0};
        options->flush_threshold = 0;
        options->arena_chunk_bytes = 0;
        options->block_cache_bytes = 0;
        options->partitioned_sstables = false;
        options->sstable_codec = FASTDB_COMPRESSION_NONE;
        options->sync_interval_ms = 0;
        fastdb_group_commit_options_default(&options->group_commit);
        fastdb_compaction_options_default(&options->compaction);
        return FASTDB_OK;
    }
    if ((mode != FASTDB_MODE_MEMORY_PERSISTENT && mode != FASTDB_MODE_LSM) ||
        path == NULL || *path == '\0' || options->eviction_policy != FASTDB_EVICTION_NONE)
        return FASTDB_INVALID_ARGUMENT;
    FastDBOptions next = *options;
    next.mode = mode;
    if (mode != options->mode) {
        fastdb_compaction_options_default(&next.compaction);
        next.flush_threshold = 0;
        next.arena_chunk_bytes = 0;
        next.block_cache_bytes = 0;
        next.partitioned_sstables = false;
        next.sstable_codec = FASTDB_COMPRESSION_NONE;
        next.sync_interval_ms = 0;
    }
    if (settings == NULL)
        fastdb_persistent_options_default(&next.storage);
    else
        next.storage = *settings;
    if (next.storage.durability != FASTDB_DURABILITY_INTERVAL)
        next.sync_interval_ms = 0;
    if (next.storage.durability != FASTDB_DURABILITY_GROUP_COMMIT || mode != options->mode)
        fastdb_group_commit_options_default(&next.group_commit);
    FastDBStatus status = fdb_disk_validate(&next);
    if (status != FASTDB_OK)
        return status;
    char *copy = strdup(path);
    if (copy == NULL)
        return FASTDB_OUT_OF_MEMORY;
    free(options->path);
    options->path = copy;
    options->mode = mode;
    options->storage = next.storage;
    options->compaction = next.compaction;
    options->flush_threshold = next.flush_threshold;
    options->arena_chunk_bytes = next.arena_chunk_bytes;
    options->block_cache_bytes = next.block_cache_bytes;
    options->partitioned_sstables = next.partitioned_sstables;
    options->sstable_codec = next.sstable_codec;
    options->sync_interval_ms = next.sync_interval_ms;
    options->group_commit = next.group_commit;
    return FASTDB_OK;
}

FastDBStatus fastdb_options_set_wal_interval(FastDBOptions *options, uint32_t milliseconds)
{
    if (options == NULL || options->path == NULL || milliseconds == 0 ||
        options->storage.durability != FASTDB_DURABILITY_INTERVAL)
        return FASTDB_INVALID_ARGUMENT;
    options->sync_interval_ms = milliseconds;
    return FASTDB_OK;
}

FastDBStatus fastdb_options_set_partitioned_sstables(FastDBOptions *options, bool enabled)
{
    if (options == NULL || options->path == NULL)
        return FASTDB_INVALID_ARGUMENT;
    FastDBOptions next = *options;
    next.partitioned_sstables = enabled;
    FastDBStatus status = fdb_disk_validate(&next);
    if (status == FASTDB_OK)
        options->partitioned_sstables = enabled;
    return status;
}

FastDBStatus fastdb_options_set_sstable_compression(FastDBOptions *options,
                                                     FastDBCompression compression)
{
    if (options == NULL || options->path == NULL ||
        (compression != FASTDB_COMPRESSION_NONE && compression != FASTDB_COMPRESSION_LZ4 &&
         compression != FASTDB_COMPRESSION_ZSTD))
        return FASTDB_INVALID_ARGUMENT;
    if (!fdb_block_codec_supported((uint32_t)compression))
        return FASTDB_UNSUPPORTED;
    FastDBOptions next = *options;
    next.sstable_codec = (uint8_t)compression;
    if (compression != FASTDB_COMPRESSION_NONE)
        next.partitioned_sstables = true;
    FastDBStatus status = fdb_disk_validate(&next);
    if (status == FASTDB_OK) {
        options->sstable_codec = next.sstable_codec;
        options->partitioned_sstables = next.partitioned_sstables;
    }
    return status;
}

FastDBStatus fastdb_options_set_group_commit(FastDBOptions *options,
                                              const FastDBGroupCommitOptions *settings)
{
    if (options == NULL || options->storage.durability != FASTDB_DURABILITY_GROUP_COMMIT ||
        !fdb_group_commit_valid(settings))
        return FASTDB_INVALID_ARGUMENT;
    options->group_commit = *settings;
    return FASTDB_OK;
}

FastDBStatus fastdb_options_set_compaction(FastDBOptions *options,
                                            const FastDBCompactionOptions *settings)
{
    if (options == NULL || settings == NULL || options->mode != FASTDB_MODE_LSM)
        return FASTDB_INVALID_ARGUMENT;
    FastDBOptions next = *options;
    next.compaction = *settings;
    FastDBStatus status = fdb_disk_validate(&next);
    if (status == FASTDB_OK)
        options->compaction = *settings;
    return status;
}

FastDBStatus fastdb_options_set_lsm_flush_threshold(FastDBOptions *options, size_t memtable_bytes)
{
    if (options == NULL || options->mode != FASTDB_MODE_LSM)
        return FASTDB_INVALID_ARGUMENT;
    FastDBOptions candidate = *options;
    candidate.flush_threshold = memtable_bytes;
    FastDBStatus status = fdb_disk_validate(&candidate);
    if (status == FASTDB_OK)
        options->flush_threshold = memtable_bytes;
    return status;
}

FastDBStatus fastdb_options_set_lsm_arena(FastDBOptions *options, size_t chunk_bytes)
{
    if (options == NULL || options->mode != FASTDB_MODE_LSM)
        return FASTDB_INVALID_ARGUMENT;
    FastDBOptions candidate = *options;
    candidate.arena_chunk_bytes = chunk_bytes;
    FastDBStatus status = fdb_disk_validate(&candidate);
    if (status == FASTDB_OK)
        options->arena_chunk_bytes = chunk_bytes;
    return status;
}

FastDBStatus fastdb_options_set_lsm_block_cache(FastDBOptions *options, size_t max_bytes)
{
    if (options == NULL || options->mode != FASTDB_MODE_LSM)
        return FASTDB_INVALID_ARGUMENT;
    FastDBOptions candidate = *options;
    candidate.block_cache_bytes = max_bytes;
    FastDBStatus status = fdb_disk_validate(&candidate);
    if (status == FASTDB_OK)
        options->block_cache_bytes = max_bytes;
    return status;
}

static FastDBStatus flush_immutable(FastDB *db)
{
    FastDBStatus status = fdb_disk_freeze_wal(db->disk);
    if (status != FASTDB_OK)
        return status;
    FDBBackgroundFlush *flush = db->flush;
    flush->frozen = db->tree;
    flush->frozen_sequence = db->sequence;
    db->tree = (FDBTree){0};

    FDBFlushOutput output = {.sequence = flush->frozen_sequence};
    status = fdb_disk_flush_reserve(db->disk, &output);
    if (status == FASTDB_OK) {
        pthread_mutex_unlock(&db->mutex);
        status = fdb_disk_flush_build(db->disk, &flush->frozen, &output);
        pthread_mutex_lock(&db->mutex);
    }
    if (status == FASTDB_OK)
        status = fdb_disk_flush_commit(db->disk, &flush->frozen, &output);
    fdb_flush_dispose(&output);
    return status;
}

static void *flush_worker(void *arg)
{
    FastDB *db = arg;
    FDBBackgroundFlush *flush = db->flush;
    pthread_mutex_lock(&db->mutex);
    while (!flush->stopping) {
        if (flush->compaction_pending) {
            flush->compaction_pending = false;
            flush->compaction_running = true;
            flush->compaction_error = fdb_disk_compact(db, false, true, NULL);
            flush->compaction_running = false;
            pthread_cond_broadcast(&flush->condition);
            continue;
        }
        if (flush->error != FASTDB_OK || flush->waiters != 0 ||
            fdb_tree_memory_bytes(&db->tree) < flush->threshold ||
            fdb_disk_maintenance_waiting(db->disk)) {
            pthread_cond_wait(&flush->condition, &db->mutex);
            continue;
        }
        flush->error = flush_immutable(db);
        if (flush->error == FASTDB_OK && !flush->stopping) {
            flush->compaction_pending = true;
            pthread_cond_broadcast(&flush->condition);
            /* Let close cancel a queued job before it starts. */
            pthread_mutex_unlock(&db->mutex);
            pthread_mutex_lock(&db->mutex);
        }
        pthread_cond_broadcast(&flush->condition);
    }
    pthread_mutex_unlock(&db->mutex);
    return NULL;
}

static FastDBStatus start_flush_worker(FastDB *db, size_t threshold)
{
    if (threshold == 0)
        return FASTDB_OK;
    FDBBackgroundFlush *flush = calloc(1, sizeof(*flush));
    if (flush == NULL)
        return FASTDB_OUT_OF_MEMORY;
    int error = pthread_cond_init(&flush->condition, NULL);
    if (error != 0) {
        free(flush);
        return error == ENOMEM ? FASTDB_OUT_OF_MEMORY : FASTDB_IO_ERROR;
    }
    flush->threshold = threshold;
    db->flush = flush;
    error = pthread_create(&flush->worker, NULL, flush_worker, db);
    if (error != 0) {
        db->flush = NULL;
        pthread_cond_destroy(&flush->condition);
        free(flush);
        return error == ENOMEM ? FASTDB_OUT_OF_MEMORY : FASTDB_IO_ERROR;
    }
    return FASTDB_OK;
}

FastDBStatus fdb_open_with_fs(const FastDBOptions *options, const FDBFs *fs, FastDB **out)
{
    if (out == NULL)
        return FASTDB_INVALID_ARGUMENT;
    *out = NULL;
    if (options != NULL && !valid_eviction(options))
        return FASTDB_INVALID_ARGUMENT;
    size_t tail = options != NULL && options->eviction_policy != FASTDB_EVICTION_NONE ?
                  sizeof(Eviction) : 0;
    FastDB *db = calloc(1, sizeof(*db) + tail);
    if (db == NULL)
        return FASTDB_OUT_OF_MEMORY;
    db->limits = options == NULL ? defaults : options->limits;
    if (tail != 0) {
        db->reserved_bytes = tail;
        *eviction_state(db) = (Eviction){.policy = options->eviction_policy,
                                        .limit = options->max_live_bytes};
    }
    int error = pthread_mutex_init(&db->mutex, NULL);
    if (error != 0) {
        free(db);
        return error == ENOMEM ? FASTDB_OUT_OF_MEMORY : FASTDB_IO_ERROR;
    }
    if (options != NULL && options->path != NULL) {
        const FDBWalReplay replay = {replay_frame, db, replay_headroom};
        FastDBStatus status = fdb_disk_open(db, options, fs, &replay, restore_checkpoint);
        if (status == FASTDB_OK)
            status = start_flush_worker(db, options->flush_threshold);
        if (status != FASTDB_OK) {
            (void)fdb_disk_stop_sync_worker(db->disk);
            (void)fdb_disk_close(db->disk);
            fdb_tree_destroy(&db->tree);
            pthread_mutex_destroy(&db->mutex);
            free(db);
            return status;
        }
    }
    *out = db;
    return FASTDB_OK;
}

FastDBStatus fastdb_open(const FastDBOptions *options, FastDB **out)
{
    return fdb_open_with_fs(options, NULL, out);
}

FastDBStatus fastdb_close(FastDB *db)
{
    if (db == NULL)
        return FASTDB_INVALID_ARGUMENT;
    pthread_mutex_lock(&db->mutex);
    FDBBackgroundFlush *flush = db->flush;
    bool worker_compaction = flush != NULL && flush->compaction_running;
    if (db->iterator_count != 0 || db->snapshot_bytes != 0 ||
        fdb_group_commit_pending(fdb_disk_group_commit(db->disk)) != 0 ||
        fdb_disk_pinned_bytes(db->disk) != 0 ||
        (fdb_disk_compaction_bytes(db->disk) != 0 && !worker_compaction)) {
        pthread_mutex_unlock(&db->mutex);
        return FASTDB_BUSY;
    }
    FastDBStatus maintenance = FASTDB_OK;
    if (flush != NULL) {
        flush->stopping = true;
        if (flush->compaction_pending)
            flush->compaction_pending = false;
        pthread_cond_broadcast(&flush->condition);
    }
    pthread_mutex_unlock(&db->mutex);
    if (flush != NULL) {
        if (pthread_join(flush->worker, NULL) != 0)
            return FASTDB_BUSY;
        maintenance = flush->error != FASTDB_OK ? flush->error : flush->compaction_error;
        pthread_cond_destroy(&flush->condition);
        fdb_tree_destroy(&flush->frozen);
        free(flush);
        db->flush = NULL;
    }
    if (fdb_disk_stop_sync_worker(db->disk) != FASTDB_OK || pthread_mutex_destroy(&db->mutex) != 0)
        return FASTDB_BUSY;
    FastDBStatus status = fdb_disk_close(db->disk);
    fdb_tree_destroy(&db->tree);
    free(db);
    return status == FASTDB_OK ? maintenance : status;
}

size_t fdb_memory_bytes(const FastDB *db)
{
    size_t frozen_bytes = db->flush == NULL ? 0 : fdb_tree_memory_bytes(&db->flush->frozen);
    return sizeof(*db) + live_tree_bytes(db) + frozen_bytes + db->iterator_bytes + db->snapshot_bytes +
           fdb_disk_pinned_bytes(db->disk) + fdb_disk_compaction_bytes(db->disk);
}

FastDBStatus fastdb_stats(FastDB *db, FastDBStats *out)
{
    if (db == NULL || out == NULL)
        return FASTDB_INVALID_ARGUMENT;
    pthread_mutex_lock(&db->mutex);
    FastDBStatus status = fdb_disk_poisoned(db->disk) ? FASTDB_COMMIT_UNCERTAIN : FASTDB_OK;
    size_t frozen_count = db->flush == NULL ? 0 : db->flush->frozen.count;
    *out = status != FASTDB_OK ? (FastDBStats){0} :
           (FastDBStats){db->sequence, db->tree.count + frozen_count,
                         fdb_memory_bytes(db) + db->reserved_bytes,
                         db->iterator_bytes, db->iterator_count};
    pthread_mutex_unlock(&db->mutex);
    return status;
}

FastDBStatus fastdb_block_cache_stats(FastDB *db, FastDBBlockCacheStats *out)
{
    if (db == NULL || out == NULL)
        return FASTDB_INVALID_ARGUMENT;
    pthread_mutex_lock(&db->mutex);
    FastDBStatus status = fdb_disk_poisoned(db->disk) ? FASTDB_COMMIT_UNCERTAIN : FASTDB_OK;
    if (status == FASTDB_OK)
        fdb_disk_cache_stats(db->disk, out);
    else
        *out = (FastDBBlockCacheStats){0};
    pthread_mutex_unlock(&db->mutex);
    return status;
}

FastDBStatus fastdb_flush(FastDB *db)
{
    if (db == NULL)
        return FASTDB_INVALID_ARGUMENT;
    pthread_mutex_lock(&db->mutex);
    FDBBackgroundFlush *flush = db->flush;
    FastDBStatus status = FASTDB_OK;
    bool background_compaction = false;
    if (flush != NULL) {
        ++flush->waiters;
        while ((flush->frozen.count != 0 || flush->compaction_pending) &&
               flush->error == FASTDB_OK)
            pthread_cond_wait(&flush->condition, &db->mutex);
        --flush->waiters;
        background_compaction = flush->compaction_running;
        if (flush->frozen.count != 0)
            status = fdb_disk_retry_frozen(db->disk, &flush->frozen, flush->frozen_sequence);
    }
    if (status == FASTDB_OK)
        status = fdb_disk_flush(db->disk, &db->tree);
    if (status == FASTDB_OK && !background_compaction)
        status = fdb_disk_compact(db, false, false, NULL);
    if (flush != NULL) {
        if (status != FASTDB_BUSY)
            flush->error = status;
        if (status == FASTDB_OK)
            flush->compaction_error = FASTDB_OK;
        pthread_cond_broadcast(&flush->condition);
    }
    pthread_mutex_unlock(&db->mutex);
    return status;
}

FastDBStatus fastdb_compact_range(FastDB *db, const FastDBRange *range)
{
    if (db == NULL)
        return FASTDB_INVALID_ARGUMENT;
    FastDBStatus status = range == NULL ? FASTDB_OK : fdb_validate_range(&db->limits, range);
    if (status != FASTDB_OK)
        return status;
    pthread_mutex_lock(&db->mutex);
    if (fdb_disk_poisoned(db->disk))
        status = FASTDB_COMMIT_UNCERTAIN;
    else if (db->flush != NULL && db->flush->frozen.count != 0 && db->flush->error == FASTDB_OK)
        status = FASTDB_BUSY;
    else
        status = fdb_disk_compact(db, true, false, range);
    if (db->flush != NULL)
        pthread_cond_broadcast(&db->flush->condition);
    pthread_mutex_unlock(&db->mutex);
    return status;
}

FastDBStatus fastdb_compact(FastDB *db)
{
    return fastdb_compact_range(db, NULL);
}

FastDBStatus fastdb_checkpoint(FastDB *db)
{
    if (db == NULL)
        return FASTDB_INVALID_ARGUMENT;
    pthread_mutex_lock(&db->mutex);
    FastDBStatus status = fdb_disk_checkpoint(db->disk, &db->tree);
    pthread_mutex_unlock(&db->mutex);
    return status;
}

FastDBStatus fdb_validate_key(const FastDBLimits *limits, FastDBSlice key)
{
    if (key.data == NULL || key.size == 0)
        return FASTDB_INVALID_ARGUMENT;
    return key.size > limits->max_key_bytes ? FASTDB_LIMIT_EXCEEDED : FASTDB_OK;
}

static FastDBStatus validate_op(const FastDBLimits *limits, const Operation *op)
{
    FastDBStatus status = fdb_validate_key(limits, op->key);
    if (status != FASTDB_OK)
        return status;
    if (op->value.data == NULL && op->value.size != 0)
        return FASTDB_INVALID_ARGUMENT;
    return op->value.size > limits->max_value_bytes ? FASTDB_LIMIT_EXCEEDED : FASTDB_OK;
}

static int stage_order(const StagedWrite *a, const StagedWrite *b)
{
    int order = fdb_compare(a->record.key, b->record.key);
    if (order != 0)
        return order;
    return (a->record.sequence > b->record.sequence) - (a->record.sequence < b->record.sequence);
}

static void swap_stages(StagedWrite *a, StagedWrite *b)
{
    StagedWrite temp = *a;
    *a = *b;
    *b = temp;
}

static void sift_down(StagedWrite *stages, size_t root, size_t count)
{
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && stage_order(&stages[child], &stages[child + 1]) < 0)
            ++child;
        if (stage_order(&stages[root], &stages[child]) >= 0)
            return;
        swap_stages(&stages[root], &stages[child]);
        root = child;
    }
}

static void sort_stages(StagedWrite *stages, size_t count)
{
    for (size_t parent = count / 2; parent != 0; --parent)
        sift_down(stages, parent - 1, count);
    for (size_t end = count; end > 1; --end) {
        swap_stages(&stages[0], &stages[end - 1]);
        sift_down(stages, 0, end - 1);
    }
}

static FastDBStatus read_tables(FastDB *db, FastDBSlice key, bool copy, bool allow_pin, FDBLookup *out)
{
    if (!allow_pin)
        return fdb_disk_find(db->disk, key, db->sequence, copy, out);
    FDBDiskRead view;
    FastDBStatus status = fdb_disk_pin_read(db->disk, key, db->sequence, write_headroom(db), &view);
    if (status == FASTDB_BUSY)
        return fdb_disk_find(db->disk, key, db->sequence, copy, out);
    if (status != FASTDB_OK || view.count == 0)
        return status;

    pthread_mutex_unlock(&db->mutex);
    status = fdb_disk_find_pinned(db->disk, &view, key, copy, out);
    pthread_mutex_lock(&db->mutex);
    fdb_disk_unpin_read(db->disk, &view);
    if (db->flush != NULL)
        pthread_cond_signal(&db->flush->condition);
    return status;
}

static FastDBStatus lookup(FastDB *db, FastDBSlice key, bool copy, bool allow_pin, FDBLookup *out)
{
    *out = (FDBLookup){0};
    if (fdb_disk_poisoned(db->disk))
        return FASTDB_COMMIT_UNCERTAIN;
    const FDBNode *node = fdb_tree_get(&db->tree, key);
    if (node == NULL && db->flush != NULL)
        node = fdb_tree_get(&db->flush->frozen, key);
    FastDBStatus status = FASTDB_OK;
    /* The active tree is newer than the frozen tree, and both are newer than disk. */
    if (node != NULL)
        status = fdb_lookup_record(&node->record, copy, out);
    else if (fdb_disk_has_tables(db->disk))
        status = read_tables(db, key, copy, allow_pin, out);
    if (status != FASTDB_OK)
        return status;

    uint64_t read_time = 0;
    if (out->revision != 0 && out->kind == FDB_PUT && out->expires_at_ms != 0) {
        status = fdb_read_time(&read_time);
        if (status != FASTDB_OK)
            return status;
    }
    if (out->revision == 0 || !fdb_record_visible(out->kind, out->expires_at_ms, read_time)) {
        free(out->value);
        *out = (FDBLookup){0};
    }
    return FASTDB_OK;
}

static FastDBStatus preflight(FastDB *db, const Operation *op, size_t *mutations)
{
    *mutations = 0;
    if (fdb_disk_poisoned(db->disk))
        return FASTDB_COMMIT_UNCERTAIN;
    for (; op != NULL; op = op->next) {
        FastDBStatus status = validate_op(&db->limits, op);
        if (status != FASTDB_OK)
            return status;
        if (op->kind == EXPECT) {
            FDBLookup current;
            status = lookup(db, op->key, false, false, &current);
            if (status != FASTDB_OK)
                return status;
            if (current.revision != op->expected)
                return FASTDB_CONFLICT;
        } else {
            ++*mutations;
        }
    }
    return *mutations > UINT64_MAX - db->sequence ? FASTDB_LIMIT_EXCEEDED : FASTDB_OK;
}

static size_t eviction_offset(const FDBNode *node)
{
    return sizeof(*node) + node->record.key.size + node->record.value.size;
}

static EvictionRecord eviction_record(const FDBNode *node)
{
    EvictionRecord metadata;
    memcpy(&metadata, (const unsigned char *)node + eviction_offset(node), sizeof(metadata));
    return metadata;
}

static void store_eviction_record(FDBNode *node, EvictionRecord metadata)
{
    memcpy((unsigned char *)node + eviction_offset(node), &metadata, sizeof(metadata));
}

static uint64_t next_touch(Eviction *cache)
{
    if (cache->tick != UINT64_MAX)
        ++cache->tick;
    return cache->tick;
}

static void touch_cached_key(FastDB *db, FastDBSlice key)
{
    Eviction *cache = eviction_state(db);
    if (cache == NULL || cache->policy == FASTDB_EVICTION_FIFO)
        return;
    /* A successful memory lookup still owns the database mutex. */
    FDBNode *node = (FDBNode *)fdb_tree_get(&db->tree, key);
    EvictionRecord metadata = eviction_record(node);
    if (cache->policy == FASTDB_EVICTION_LRU)
        metadata.touched = next_touch(cache);
    else if (metadata.hits != UINT64_MAX)
        ++metadata.hits;
    store_eviction_record(node, metadata);
}

typedef struct EvictionVictim {
    const FDBNode *node;
    EvictionRecord metadata;
} EvictionVictim;

static bool preferable_victim(FastDBEvictionPolicy policy, const FDBNode *node,
                               EvictionRecord metadata, const EvictionVictim *current)
{
    if (current->node == NULL)
        return true;
    if (policy == FASTDB_EVICTION_LFU && metadata.hits != current->metadata.hits)
        return metadata.hits < current->metadata.hits;
    if (policy == FASTDB_EVICTION_LRU) {
        if (metadata.touched != current->metadata.touched)
            return metadata.touched < current->metadata.touched;
        return node->record.sequence < current->node->record.sequence;
    }
    return metadata.inserted < current->metadata.inserted;
}

static void select_victim(const FDBNode *node, FastDBEvictionPolicy policy,
                           uint64_t before, EvictionVictim *victim)
{
    if (node == NULL)
        return;
    select_victim(node->left, policy, before, victim);
    if (node->record.sequence <= before) {
        EvictionRecord metadata = eviction_record(node);
        if (preferable_victim(policy, node, metadata, victim))
            *victim = (EvictionVictim){node, metadata};
    }
    select_victim(node->right, policy, before, victim);
}

static void enforce_live_capacity(FastDB *db, const Eviction *cache)
{
    while (live_tree_bytes(db) > cache->limit) {
        EvictionVictim victim = {0};
        select_victim(db->tree.root, cache->policy, db->sequence, &victim);
        /* The new PUT set fits on its own, so any excess has a pre-commit victim. */
        fdb_tree_delete(&db->tree, victim.node->record.key);
    }
}

static FastDBStatus prepare_nodes(FastDB *db, StagedWrite *stages, size_t unique, size_t spare,
                                   StagedTree *tree)
{
    size_t keys = db->tree.count;
    size_t key_bytes = fdb_disk_memtable_key_bytes(db->disk, false);
    bool keep_tombstones = fdb_disk_has_tables(db->disk);
    const Eviction *cache = eviction_state(db);
    size_t write_set_budget = cache == NULL ? 0 : cache->limit;
    size_t arena_chunk_bytes = fdb_disk_arena_chunk_bytes(db->disk);
    bool use_arena = arena_chunk_bytes != 0;
    size_t arena_payload = 0;
    for (size_t i = 0; i < unique; ++i) {
        StagedWrite *write = &stages[i];
        const FDBRecord *record = &write->record;
        bool allocate = keep_tombstones || record->kind != FDB_DELETE;
        if (db->disk != NULL) {
            bool present = fdb_tree_get(&db->tree, record->key) != NULL;
            if (!allocate && present) {
                --keys;
                key_bytes -= record->key.size;
            } else if (allocate && !present) {
                if (!fdb_add_size(key_bytes, record->key.size, &key_bytes))
                    return FASTDB_LIMIT_EXCEEDED;
                ++keys;
            }
        }
        if (!allocate)
            continue;
        if (!fdb_node_size(record->key, record->value, &write->bytes))
            return FASTDB_LIMIT_EXCEEDED;
        if (cache != NULL) {
            if (!fdb_add_size(write->bytes, sizeof(EvictionRecord), &write->bytes) ||
                write->bytes > write_set_budget)
                return FASTDB_LIMIT_EXCEEDED;
            write_set_budget -= write->bytes;
        }
        if (use_arena) {
            if (!fdb_arena_count_node(write->bytes, &arena_payload))
                return FASTDB_LIMIT_EXCEEDED;
        } else {
            if (write->bytes > spare)
                return FASTDB_LIMIT_EXCEEDED;
            spare -= write->bytes;
        }
    }
    tree->key_bytes = key_bytes;
    /* Before the frozen table is durable, recovery replays both trees into one. */
    if (db->flush != NULL &&
        (!fdb_add_size(keys, db->flush->frozen.count, &keys) ||
         !fdb_add_size(key_bytes, fdb_disk_memtable_key_bytes(db->disk, true), &key_bytes)))
        return FASTDB_LIMIT_EXCEEDED;
    if (!fdb_disk_tree_fits(db->disk, keys, key_bytes))
        return FASTDB_LIMIT_EXCEEDED;
    if (use_arena) {
        FastDBStatus status = fdb_tree_reserve_arena(&db->tree, arena_payload, arena_chunk_bytes,
                                                     spare, &tree->arena);
        if (status != FASTDB_OK)
            return status;
    }

    for (size_t i = 0; i < unique; ++i) {
        StagedWrite *write = &stages[i];
        if (write->bytes != 0) {
            write->node = use_arena ? fdb_arena_make_node(&tree->arena, &write->record, write->bytes) :
                                     fdb_node_create(&write->record, write->bytes);
            if (write->node == NULL)
                return FASTDB_OUT_OF_MEMORY;
        }
    }
    return FASTDB_OK;
}

static FastDBStatus publish_stages(FastDB *db, StagedWrite *stages, size_t count, size_t spare,
                                    uint64_t sequence, const FDBRecord *journal, size_t frame_bytes,
                                    bool defer_sync)
{
    sort_stages(stages, count);
    size_t unique = 0;
    for (size_t i = 0; i < count; ++i) {
        if (i + 1 == count || fdb_compare(stages[i].record.key, stages[i + 1].record.key) != 0)
            stages[unique++] = stages[i];
    }
    StagedTree tree = {0};
    FastDBStatus status = prepare_nodes(db, stages, unique, spare, &tree);
    if (status == FASTDB_OK && journal != NULL) {
        status = fdb_disk_maybe_checkpoint(db->disk, &db->tree, frame_bytes);
        if (status == FASTDB_OK)
            status = fdb_disk_append(db->disk, journal, count, defer_sync);
    }
    Eviction *cache = eviction_state(db);
    uint64_t touch = 0;
    if (status == FASTDB_OK && cache != NULL && cache->policy == FASTDB_EVICTION_LRU)
        touch = next_touch(cache);
    bool staged_in_arena = tree.arena.chunk != NULL;
    if (staged_in_arena)
        fdb_tree_finish_arena(&db->tree, tree.arena, status == FASTDB_OK);
    for (size_t i = 0; i < unique; ++i) {
        StagedWrite *write = &stages[i];
        if (status != FASTDB_OK) {
            if (!staged_in_arena)
                free(write->node);
        } else if (write->node != NULL) {
            if (cache != NULL) {
                const FDBNode *old = fdb_tree_get(&db->tree, write->record.key);
                EvictionRecord metadata = old == NULL ?
                    (EvictionRecord){.inserted = write->record.sequence} : eviction_record(old);
                metadata.touched = touch;
                store_eviction_record(write->node, metadata);
            }
            fdb_tree_put(&db->tree, write->node);
        } else {
            fdb_tree_delete(&db->tree, write->record.key);
        }
    }
    if (status == FASTDB_OK) {
        if (cache != NULL)
            enforce_live_capacity(db, cache);
        fdb_disk_set_active_key_bytes(db->disk, tree.key_bytes);
        db->sequence = sequence;
    }
    return status;
}

static FastDBStatus allocate_stages(size_t count, size_t *spare,
                                     StagedWrite *single, StagedWrite **out)
{
    *out = single;
    if (count == 1)
        return FASTDB_OK;
    if (count > *spare / sizeof(**out))
        return FASTDB_LIMIT_EXCEEDED;
    size_t bytes = count * sizeof(**out);
    *out = malloc(bytes);
    if (*out == NULL)
        return FASTDB_OUT_OF_MEMORY;
    *spare -= bytes;
    return FASTDB_OK;
}

static size_t write_headroom(const FastDB *db)
{
    return db->limits.max_memory_bytes - db->reserved_bytes - fdb_memory_bytes(db);
}

static size_t replay_headroom(void *context)
{
    return write_headroom(context);
}

static FastDBStatus commit_writes(FastDB *db, const Operation *op, size_t count, bool defer_sync)
{
    size_t spare = write_headroom(db);
    StagedWrite single, *stages;
    FastDBStatus status = allocate_stages(count, &spare, &single, &stages);
    if (status != FASTDB_OK)
        return status;
    FDBRecord one, *journal = NULL;
    if (db->disk != NULL) {
        journal = &one;
        if (count > 1) {
            if (count > spare / sizeof(*journal)) {
                status = FASTDB_LIMIT_EXCEEDED;
                goto cleanup_stages;
            }
            size_t size = count * sizeof(*journal);
            journal = malloc(size);
            if (journal == NULL) {
                status = FASTDB_OUT_OF_MEMORY;
                goto cleanup_stages;
            }
            spare -= size;
        }
    }
    uint64_t sequence = db->sequence;
    uint64_t write_time = 0;
    bool sampled_time = false;
    size_t i = 0;
    for (; op != NULL; op = op->next) {
        if (op->kind == EXPECT)
            continue;
        uint64_t expiration = 0;
        if (op->kind == PUT && op->ttl_ms != 0) {
            if (!sampled_time) {
                status = fdb_read_time(&write_time);
                if (status != FASTDB_OK)
                    break;
                sampled_time = true;
            }
            if (op->ttl_ms > UINT64_MAX - write_time) {
                status = FASTDB_LIMIT_EXCEEDED;
                break;
            }
            expiration = write_time + op->ttl_ms;
        }
        FDBRecord record = {op->key, op->value, ++sequence, expiration,
                            op->kind == PUT ? FDB_PUT : FDB_DELETE};
        stages[i] = (StagedWrite){.record = record};
        if (journal != NULL)
            journal[i] = record;
        ++i;
    }
    size_t frame = 0;
    if (status == FASTDB_OK && journal != NULL)
        status = fdb_disk_frame_size(db->disk, journal, count, &frame);
    if (status == FASTDB_OK)
        status = frame > spare ? FASTDB_LIMIT_EXCEEDED :
                 publish_stages(db, stages, count, spare - frame, sequence, journal, frame,
                                 defer_sync);
    if (journal != &one)
        free(journal);
cleanup_stages:
    if (count > 1)
        free(stages);
    return status;
}

static FastDBStatus replay_frame(void *context, const unsigned char *payload, size_t size,
                                  size_t count, uint64_t first)
{
    FastDB *db = context;
    size_t spare = write_headroom(db);
    if (size > spare)
        return FASTDB_LIMIT_EXCEEDED;
    spare -= size;
    StagedWrite single, *stages;
    FastDBStatus status = allocate_stages(count, &spare, &single, &stages);
    if (status != FASTDB_OK)
        return status;
    for (size_t i = 0; status == FASTDB_OK && i < count; ++i) {
        size_t used;
        stages[i] = (StagedWrite){0};
        status = fdb_record_decode(payload, size, db->limits.max_key_bytes, db->limits.max_value_bytes,
                                   &stages[i].record, &used);
        payload += used;
        size -= used;
    }
    if (status == FASTDB_OK)
        status = publish_stages(db, stages, count, spare, first + (count - 1), NULL, 0, false);
    if (count > 1)
        free(stages);
    return status;
}

static FastDBStatus restore_checkpoint(void *context, const FDBRecord *record)
{
    FastDB *db = context;
    if (record->kind != FDB_PUT || fdb_tree_get(&db->tree, record->key) != NULL)
        return FASTDB_CORRUPTION;
    StagedWrite write = {.record = *record};
    return publish_stages(db, &write, 1, write_headroom(db), record->sequence, NULL, 0, false);
}

static FastDBStatus commit_locked(FastDB *db, const Operation *op, bool defer_sync)
{
    size_t mutations;
    FastDBStatus status = preflight(db, op, &mutations);
    if (status == FASTDB_OK && mutations != 0) {
        FDBBackgroundFlush *flush = db->flush;
        if (flush != NULL && fdb_tree_memory_bytes(&db->tree) >= flush->threshold &&
            (flush->frozen.count != 0 || flush->error != FASTDB_OK ||
             fdb_disk_maintenance_waiting(db->disk)))
            status = flush->error == FASTDB_OK ? FASTDB_BUSY : flush->error;
        else
            status = commit_writes(db, op, mutations, defer_sync);
        if (status == FASTDB_OK && flush != NULL)
            pthread_cond_signal(&flush->condition);
    }
    return status;
}

void fdb_engine_commit_group(FastDB *db, FDBGroupedWrite *list)
{
    pthread_mutex_lock(&db->mutex);
    uint64_t start = db->sequence;
    FDBGroupedWrite *first_change = NULL;
    for (FDBGroupedWrite *write = list; write != NULL; write = write->next) {
        write->status = commit_locked(db, write->operation, true);
        if (first_change == NULL && db->sequence != start)
            first_change = write;
    }
    /* Later checks may depend on an earlier provisional write in this group. */
    if (first_change != NULL && fdb_disk_finish_group(db->disk) != FASTDB_OK) {
        for (FDBGroupedWrite *write = first_change; write != NULL; write = write->next)
            write->status = FASTDB_COMMIT_UNCERTAIN;
    }
    pthread_mutex_unlock(&db->mutex);
}

bool fdb_group_commit_valid(const FastDBGroupCommitOptions *options)
{
    const size_t smallest = sizeof(FDBGroupedWrite) + sizeof(Operation) + 1;
    return options != NULL && options->max_pending_requests != 0 &&
           options->max_pending_bytes >= smallest;
}

static FastDBStatus commit(FastDB *db, const Operation *op)
{
    FDBGroupCommit *group = fdb_disk_group_commit(db->disk);
    if (group != NULL) {
        size_t charge = sizeof(FDBGroupedWrite);
        for (const Operation *entry = op; entry != NULL; entry = entry->next) {
            if (!fdb_add_size(charge, sizeof(*entry), &charge) ||
                !fdb_add_size(charge, entry->key.size, &charge) ||
                !fdb_add_size(charge, entry->value.size, &charge))
                return FASTDB_LIMIT_EXCEEDED;
        }
        return fdb_group_commit_submit(group, op, charge);
    }
    pthread_mutex_lock(&db->mutex);
    FastDBStatus status = commit_locked(db, op, false);
    pthread_mutex_unlock(&db->mutex);
    return status;
}

FastDBStatus fastdb_put(FastDB *db, const void *key, size_t key_size,
                         const void *value, size_t value_size)
{
    return fastdb_put_with_ttl(db, key, key_size, value, value_size, 0);
}

FastDBStatus fastdb_put_with_ttl(FastDB *db, const void *key, size_t key_size,
                                  const void *value, size_t value_size, uint64_t ttl_ms)
{
    if (db == NULL)
        return FASTDB_INVALID_ARGUMENT;
    const Operation op = {.kind = PUT, .key = {key, key_size},
                          .value = {value, value_size}, .ttl_ms = ttl_ms};
    return commit(db, &op);
}

FastDBStatus fastdb_purge_expired(FastDB *db, size_t limit, size_t *removed)
{
    if (removed == NULL)
        return FASTDB_INVALID_ARGUMENT;
    *removed = 0;
    if (db == NULL || limit == 0)
        return FASTDB_INVALID_ARGUMENT;
    if (limit > db->limits.max_batch_operations)
        return FASTDB_LIMIT_EXCEEDED;
    pthread_mutex_lock(&db->mutex);
    FastDBStatus status;
    uint64_t now = 0;
    if (fdb_disk_poisoned(db->disk))
        status = FASTDB_COMMIT_UNCERTAIN;
    else if (fdb_disk_has_tables(db->disk))
        status = FASTDB_UNSUPPORTED;
    else
        status = fdb_read_time(&now);
    const FDBNode *node = fdb_tree_bound(&db->tree, (FastDBSlice){0}, false);
    while (status == FASTDB_OK && node != NULL && *removed < limit) {
        const FDBNode *next = fdb_tree_bound(&db->tree, node->record.key, true);
        if (node->record.expires_at_ms != 0 && node->record.expires_at_ms <= now) {
            if (db->sequence == UINT64_MAX) {
                status = FASTDB_LIMIT_EXCEEDED;
                break;
            }
            const Operation op = {.kind = DELETE, .key = node->record.key};
            status = commit_writes(db, &op, 1, false);
            if (status == FASTDB_OK)
                ++*removed;
        }
        node = next;
    }
    pthread_mutex_unlock(&db->mutex);
    return status;
}

FastDBStatus fastdb_delete(FastDB *db, const void *key, size_t key_size)
{
    if (db == NULL)
        return FASTDB_INVALID_ARGUMENT;
    const Operation op = {.kind = DELETE, .key = {key, key_size}};
    return commit(db, &op);
}

FastDBStatus fastdb_compare_and_swap(FastDB *db, const void *key, size_t key_size,
                                      uint64_t expected, const void *value, size_t value_size)
{
    if (db == NULL)
        return FASTDB_INVALID_ARGUMENT;
    Operation put = {.kind = PUT, .key = {key, key_size}, .value = {value, value_size}};
    const Operation check = {.next = &put, .kind = EXPECT, .key = put.key, .expected = expected};
    return commit(db, &check);
}

FastDBStatus fastdb_get_with_revision(FastDB *db, const void *key, size_t key_size,
                                       void **value, size_t *value_size, uint64_t *revision)
{
    if (value != NULL)
        *value = NULL;
    if (value_size != NULL)
        *value_size = 0;
    if (revision != NULL)
        *revision = 0;
    if (db == NULL || value == NULL || value_size == NULL || revision == NULL)
        return FASTDB_INVALID_ARGUMENT;
    FastDBSlice slice = {key, key_size};
    FastDBStatus status = fdb_validate_key(&db->limits, slice);
    if (status != FASTDB_OK)
        return status;

    pthread_mutex_lock(&db->mutex);
    FDBLookup found;
    status = lookup(db, slice, true, true, &found);
    if (status == FASTDB_OK && found.revision != 0) {
        touch_cached_key(db, slice);
        *value = found.value;
        *value_size = found.size;
        *revision = found.revision;
    } else {
        free(found.value);
        if (status == FASTDB_OK)
            status = FASTDB_NOT_FOUND;
    }
    pthread_mutex_unlock(&db->mutex);
    return status;
}

FastDBStatus fastdb_get(FastDB *db, const void *key, size_t key_size, void **value,
                         size_t *value_size)
{
    uint64_t revision;
    return fastdb_get_with_revision(db, key, key_size, value, value_size, &revision);
}

FastDBStatus fastdb_exists(FastDB *db, const void *key, size_t key_size, bool *exists)
{
    if (exists != NULL)
        *exists = false;
    if (db == NULL || exists == NULL)
        return FASTDB_INVALID_ARGUMENT;
    FastDBSlice slice = {key, key_size};
    FastDBStatus status = fdb_validate_key(&db->limits, slice);
    if (status != FASTDB_OK)
        return status;
    pthread_mutex_lock(&db->mutex);
    FDBLookup found;
    status = lookup(db, slice, false, true, &found);
    *exists = status == FASTDB_OK && found.revision != 0;
    if (*exists)
        touch_cached_key(db, slice);
    pthread_mutex_unlock(&db->mutex);
    return status;
}

FastDBStatus fastdb_batch_create(const FastDBOptions *options, FastDBBatch **out)
{
    if (out == NULL)
        return FASTDB_INVALID_ARGUMENT;
    *out = NULL;
    FastDBBatch *batch = calloc(1, sizeof(*batch));
    if (batch == NULL)
        return FASTDB_OUT_OF_MEMORY;
    batch->limits = options == NULL ? defaults : options->limits;
    batch->tail = &batch->first;
    batch->bytes = sizeof(*batch);
    *out = batch;
    return FASTDB_OK;
}

void fastdb_batch_destroy(FastDBBatch *batch)
{
    if (batch == NULL)
        return;
    Operation *op = batch->first;
    while (op != NULL) {
        Operation *next = op->next;
        free((BatchEntry *)op);
        op = next;
    }
    free(batch);
}

static FastDBStatus batch_add(FastDBBatch *batch, Operation op)
{
    if (batch == NULL)
        return FASTDB_INVALID_ARGUMENT;
    FastDBStatus status = validate_op(&batch->limits, &op);
    if (status != FASTDB_OK)
        return status;
    size_t bytes;
    if (batch->count == batch->limits.max_batch_operations ||
        !fdb_add_size(sizeof(BatchEntry), op.key.size, &bytes) ||
        !fdb_add_size(bytes, op.value.size, &bytes) ||
        bytes > batch->limits.max_batch_bytes - batch->bytes)
        return FASTDB_LIMIT_EXCEEDED;
    BatchEntry *entry = malloc(bytes);
    if (entry == NULL)
        return FASTDB_OUT_OF_MEMORY;
    memcpy(entry->bytes, op.key.data, op.key.size);
    if (op.value.size != 0)
        memcpy(entry->bytes + op.key.size, op.value.data, op.value.size);
    op.key.data = entry->bytes;
    op.value.data = entry->bytes + op.key.size;
    entry->op = op;
    *batch->tail = &entry->op;
    batch->tail = &entry->op.next;
    ++batch->count;
    batch->bytes += bytes;
    return FASTDB_OK;
}

FastDBStatus fastdb_batch_put(FastDBBatch *batch, const void *key, size_t key_size,
                               const void *value, size_t value_size)
{
    return fastdb_batch_put_with_ttl(batch, key, key_size, value, value_size, 0);
}

FastDBStatus fastdb_batch_put_with_ttl(FastDBBatch *batch, const void *key, size_t key_size,
                                        const void *value, size_t value_size, uint64_t ttl_ms)
{
    return batch_add(batch, (Operation){.kind = PUT, .key = {key, key_size},
                                       .value = {value, value_size}, .ttl_ms = ttl_ms});
}

FastDBStatus fastdb_batch_delete(FastDBBatch *batch, const void *key, size_t key_size)
{
    return batch_add(batch, (Operation){.kind = DELETE, .key = {key, key_size}});
}

FastDBStatus fastdb_batch_expect(FastDBBatch *batch, const void *key, size_t key_size,
                                  uint64_t expected)
{
    return batch_add(batch, (Operation){.kind = EXPECT, .key = {key, key_size}, .expected = expected});
}

FastDBStatus fastdb_batch_commit(FastDB *db, const FastDBBatch *batch)
{
    if (db == NULL || batch == NULL)
        return FASTDB_INVALID_ARGUMENT;
    if (batch->count > db->limits.max_batch_operations || batch->bytes > db->limits.max_batch_bytes)
        return FASTDB_LIMIT_EXCEEDED;
    return commit(db, batch->first);
}
