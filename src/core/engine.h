#ifndef FASTDB_ENGINE_H
#define FASTDB_ENGINE_H

#include "memtable.h"
#include "fs.h"
#include <pthread.h>

struct FastDBOptions {
    FastDBLimits limits;
    FastDBMode mode;
    char *path;
    FastDBPersistentOptions storage;
    FastDBCompactionOptions compaction;
};

typedef struct FDBDisk FDBDisk;

struct FastDB {
    pthread_mutex_t mutex;
    FastDBLimits limits;
    FDBTree tree;
    uint64_t sequence;
    size_t iterator_bytes;
    size_t iterator_count;
    FDBDisk *disk;
    size_t disk_budget;
};

typedef struct FDBScanRow {
    FastDBSlice key;
    FastDBSlice value;
    uint64_t sequence;
} FDBScanRow;

struct FastDBIterator {
    FastDB *db;
    size_t bytes;
    size_t count;
    size_t cursor;
    bool more;
    FDBScanRow rows[];
};

FastDBStatus fdb_validate_key(const FastDBLimits *limits, FastDBSlice key);
FastDBStatus fdb_validate_range(const FastDBLimits *limits, const FastDBRange *range);
size_t fdb_memory_bytes(const FastDB *db);
FastDBStatus fdb_open_with_fs(const FastDBOptions *options, const FDBFs *fs, FastDB **out);

#endif
