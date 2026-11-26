#include "disk.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum OperationKind { PUT, DELETE, EXPECT } OperationKind;

typedef struct Operation {
    struct Operation *next;
    OperationKind kind;
    FastDBSlice key;
    FastDBSlice value;
    uint64_t expected;
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
    if (next.path != NULL && fdb_disk_validate(&next) != FASTDB_OK)
        return FASTDB_INVALID_ARGUMENT;
    options->limits = *limits;
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
        fastdb_compaction_options_default(&options->compaction);
        return FASTDB_OK;
    }
    if ((mode != FASTDB_MODE_MEMORY_PERSISTENT && mode != FASTDB_MODE_LSM) ||
        path == NULL || *path == '\0')
        return FASTDB_INVALID_ARGUMENT;
    FastDBOptions next = *options;
    next.mode = mode;
    if (mode != options->mode) {
        fastdb_compaction_options_default(&next.compaction);
        next.flush_threshold = 0;
    }
    if (settings == NULL)
        fastdb_persistent_options_default(&next.storage);
    else
        next.storage = *settings;
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

static FastDBStatus flush_immutable(FastDB *db)
{
    FastDBStatus status = fdb_disk_rotate_wal(db->disk);
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
        if (flush->error != FASTDB_OK || flush->waiters != 0 || db->tree.bytes < flush->threshold ||
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
    FastDB *db = calloc(1, sizeof(*db));
    if (db == NULL)
        return FASTDB_OUT_OF_MEMORY;
    db->limits = options == NULL ? defaults : options->limits;
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
    if (pthread_mutex_destroy(&db->mutex) != 0)
        return FASTDB_BUSY;
    FastDBStatus status = fdb_disk_close(db->disk);
    fdb_tree_destroy(&db->tree);
    free(db);
    return status == FASTDB_OK ? maintenance : status;
}

size_t fdb_memory_bytes(const FastDB *db)
{
    size_t frozen_bytes = db->flush == NULL ? 0 : db->flush->frozen.bytes;
    return sizeof(*db) + db->tree.bytes + frozen_bytes + db->iterator_bytes + db->snapshot_bytes +
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
                         fdb_memory_bytes(db) + db->disk_budget,
                         db->iterator_bytes, db->iterator_count};
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

    bool visible = out->revision != 0 && out->kind != FDB_DELETE;
    if (visible && out->expires_at_ms != 0) {
        struct timespec now;
        if (timespec_get(&now, TIME_UTC) != TIME_UTC)
            return FASTDB_IO_ERROR;
        if (now.tv_sec >= 0) {
            uint64_t seconds = (uint64_t)now.tv_sec;
            visible = seconds <= (UINT64_MAX - 999) / 1000 &&
                      out->expires_at_ms > seconds * 1000 + (uint64_t)(now.tv_nsec / 1000000);
        }
    }
    if (!visible) {
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

static FastDBStatus prepare_nodes(FastDB *db, StagedWrite *stages, size_t unique, size_t spare)
{
    size_t keys = db->tree.count;
    size_t key_bytes = db->tree.key_bytes;
    bool keep_tombstones = fdb_disk_has_tables(db->disk);
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
        if (!fdb_node_size(record->key, record->value, &write->bytes) || write->bytes > spare)
            return FASTDB_LIMIT_EXCEEDED;
        spare -= write->bytes;
    }
    /* Before the frozen table is durable, recovery replays both trees into one. */
    if (db->flush != NULL &&
        (!fdb_add_size(keys, db->flush->frozen.count, &keys) ||
         !fdb_add_size(key_bytes, db->flush->frozen.key_bytes, &key_bytes)))
        return FASTDB_LIMIT_EXCEEDED;
    if (!fdb_disk_tree_fits(db->disk, keys, key_bytes))
        return FASTDB_LIMIT_EXCEEDED;

    for (size_t i = 0; i < unique; ++i) {
        StagedWrite *write = &stages[i];
        if (write->bytes != 0) {
            write->node = fdb_node_create(&write->record, write->bytes);
            if (write->node == NULL)
                return FASTDB_OUT_OF_MEMORY;
        }
    }
    return FASTDB_OK;
}

static FastDBStatus publish_stages(FastDB *db, StagedWrite *stages, size_t count, size_t spare,
                                    uint64_t sequence, const FDBRecord *journal, size_t frame_bytes)
{
    sort_stages(stages, count);
    size_t unique = 0;
    for (size_t i = 0; i < count; ++i) {
        if (i + 1 == count || fdb_compare(stages[i].record.key, stages[i + 1].record.key) != 0)
            stages[unique++] = stages[i];
    }
    FastDBStatus status = prepare_nodes(db, stages, unique, spare);
    if (status == FASTDB_OK && journal != NULL) {
        status = fdb_disk_maybe_checkpoint(db->disk, &db->tree, frame_bytes);
        if (status == FASTDB_OK)
            status = fdb_disk_append(db->disk, journal, count);
    }
    for (size_t i = 0; i < unique; ++i) {
        StagedWrite *write = &stages[i];
        if (status != FASTDB_OK)
            free(write->node);
        else if (write->node != NULL)
            fdb_tree_put(&db->tree, write->node);
        else
            fdb_tree_delete(&db->tree, write->record.key);
    }
    if (status == FASTDB_OK)
        db->sequence = sequence;
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
    return db->limits.max_memory_bytes - db->disk_budget - fdb_memory_bytes(db);
}

static size_t replay_headroom(void *context)
{
    return write_headroom(context);
}

static FastDBStatus commit_writes(FastDB *db, const Operation *op, size_t count)
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
    size_t i = 0;
    for (; op != NULL; op = op->next) {
        if (op->kind == EXPECT)
            continue;
        FDBRecord record = {op->key, op->value, ++sequence, 0, op->kind == PUT ? FDB_PUT : FDB_DELETE};
        stages[i] = (StagedWrite){.record = record};
        if (journal != NULL)
            journal[i] = record;
        ++i;
    }
    size_t frame = 0;
    if (journal != NULL)
        status = fdb_disk_frame_size(db->disk, journal, count, &frame);
    if (status == FASTDB_OK)
        status = frame > spare ? FASTDB_LIMIT_EXCEEDED :
                 publish_stages(db, stages, count, spare - frame, sequence, journal, frame);
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
        status = publish_stages(db, stages, count, spare, first + (count - 1), NULL, 0);
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
    return publish_stages(db, &write, 1, write_headroom(db), record->sequence, NULL, 0);
}

static FastDBStatus commit(FastDB *db, const Operation *op)
{
    pthread_mutex_lock(&db->mutex);
    size_t mutations;
    FastDBStatus status = preflight(db, op, &mutations);
    if (status == FASTDB_OK && mutations != 0) {
        FDBBackgroundFlush *flush = db->flush;
        if (flush != NULL && db->tree.bytes >= flush->threshold &&
            (flush->frozen.count != 0 || flush->error != FASTDB_OK ||
             fdb_disk_maintenance_waiting(db->disk)))
            status = flush->error == FASTDB_OK ? FASTDB_BUSY : flush->error;
        else
            status = commit_writes(db, op, mutations);
        if (status == FASTDB_OK && flush != NULL)
            pthread_cond_signal(&flush->condition);
    }
    pthread_mutex_unlock(&db->mutex);
    return status;
}

FastDBStatus fastdb_put(FastDB *db, const void *key, size_t key_size,
                         const void *value, size_t value_size)
{
    if (db == NULL)
        return FASTDB_INVALID_ARGUMENT;
    const Operation op = {.kind = PUT, .key = {key, key_size}, .value = {value, value_size}};
    return commit(db, &op);
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
    return batch_add(batch, (Operation){.kind = PUT, .key = {key, key_size},
                                       .value = {value, value_size}});
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
