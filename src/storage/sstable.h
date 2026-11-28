#ifndef FASTDB_SSTABLE_H
#define FASTDB_SSTABLE_H

#include "fs.h"
#include "record.h"
#include "block_cache.h"

typedef struct FDBSSTLimits {
    size_t max_key_bytes;
    size_t max_value_bytes;
    size_t target_block_bytes;
    size_t max_block_bytes;
    size_t max_index_bytes;
    size_t max_filter_bytes;
    unsigned int bloom_bits_per_key;
    bool partitioned;
} FDBSSTLimits;

typedef struct FDBSSTProperties {
    uint64_t id;
    uint64_t file_bytes;
    uint64_t records;
    uint64_t tombstones;
    uint64_t min_sequence;
    uint64_t max_sequence;
    FastDBSlice min_key;
    FastDBSlice max_key;
} FDBSSTProperties;

typedef struct FDBSSTWriter FDBSSTWriter;
typedef struct FDBSSTReader FDBSSTReader;
typedef struct FDBSSTIterator FDBSSTIterator;

typedef struct FDBSSTBloomStats {
    uint64_t checks;
    uint64_t negatives;
    uint64_t positives;
    uint64_t false_positives;
    size_t corrupt_partitions;
} FDBSSTBloomStats;

bool fdb_sst_valid_limits(const FDBSSTLimits *limits);
bool fdb_sst_writer_budget(const FDBSSTLimits *limits, size_t *bytes);
bool fdb_sst_reader_budget(const FDBSSTLimits *limits, size_t *bytes);
bool fdb_sst_reader_workspace(const FDBSSTReader *reader, size_t *bytes);
bool fdb_sst_tree_fits(const FDBSSTLimits *limits, size_t keys, size_t key_bytes);
FastDBStatus fdb_sst_writer_open(const FDBDirectory *dir, const char *temporary_name, uint64_t id,
                                  const FDBSSTLimits *limits, FDBSSTWriter **out);
/* User keys increase; revisions for one key decrease. Inputs are copied. */
FastDBStatus fdb_sst_writer_add(FDBSSTWriter *writer, const FDBRecord *record);
FastDBSlice fdb_sst_writer_last_key(const FDBSSTWriter *writer);
uint64_t fdb_sst_writer_file_bytes(const FDBSSTWriter *writer);
/* Finish syncs and closes the file. The caller owns publication and unlink. */
FastDBStatus fdb_sst_writer_finish(FDBSSTWriter *writer, FDBSSTProperties *out);
FastDBStatus fdb_sst_writer_finish_checkpoint(FDBSSTWriter *writer, FDBSSTProperties *out);
/* Finish properties borrow writer-owned bounds until destroy. */
FastDBStatus fdb_sst_writer_destroy(FDBSSTWriter *writer);

FastDBStatus fdb_sst_reader_open(const FDBDirectory *dir, const char *name, uint64_t id,
                                  const FDBSSTLimits *limits, FDBSSTReader **out);
/* Configure before opening iterators. The database owns the cache. */
void fdb_sst_reader_set_cache(FDBSSTReader *reader, FDBBlockCache *cache);
const FDBSSTProperties *fdb_sst_reader_properties(const FDBSSTReader *reader);
size_t fdb_sst_reader_corrupt_partitions(const FDBSSTReader *reader);
FastDBStatus fdb_sst_reader_verify(FDBSSTReader *reader);
typedef FastDBStatus (*FDBSSTVisit)(void *context, const FDBRecord *record);
FastDBStatus fdb_sst_reader_walk(FDBSSTReader *reader, FDBSSTVisit visit, void *context);
/* All iterators must close before their reader is released. */
FastDBStatus fdb_sst_reader_close(FDBSSTReader *reader);

FastDBStatus fdb_sst_iter_open(const FDBSSTReader *reader, FDBSSTIterator **out);
void fdb_sst_iter_first(FDBSSTIterator *iter);
void fdb_sst_iter_seek(FDBSSTIterator *iter, FastDBSlice key);
void fdb_sst_iter_seek_version(FDBSSTIterator *iter, FastDBSlice key, uint64_t sequence);
/* NULL denotes EOF or an error. Borrowed records expire at the next cursor operation. */
const FDBRecord *fdb_sst_iter_current(const FDBSSTIterator *iter);
void fdb_sst_iter_next(FDBSSTIterator *iter);
FastDBStatus fdb_sst_iter_status(const FDBSSTIterator *iter);
void fdb_sst_iter_bloom_stats(const FDBSSTIterator *iter, FDBSSTBloomStats *out);
void fdb_sst_iter_close(FDBSSTIterator *iter);

#endif
