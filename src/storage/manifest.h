#ifndef FASTDB_MANIFEST_H
#define FASTDB_MANIFEST_H

#include "fs.h"

typedef enum FDBStorageMode {
    FDB_MODE_MEMORY_PERSISTENT = 1, FDB_MODE_LSM = 2
} FDBStorageMode;

typedef enum FDBFileKind {
    FDB_FILE_WAL = 1, FDB_FILE_SSTABLE = 2, FDB_FILE_CHECKPOINT = 3
} FDBFileKind;

typedef struct FDBManifestEntry {
    uint64_t id;
    FDBFileKind kind;
    uint32_t level;
    uint64_t bytes;
    uint64_t sequence_begin;
    uint64_t sequence_end;
    FastDBSlice min_key;
    FastDBSlice max_key;
} FDBManifestEntry;

typedef struct FDBManifest {
    FDBStorageMode mode;
    unsigned char database_id[16];
    uint64_t generation;
    uint64_t next_file_id;
    uint64_t sequence;
    size_t count;
    const FDBManifestEntry *entries;
} FDBManifest;

typedef struct FDBManifestLoad {
    FDBManifest view;
    unsigned char *image;
    FDBManifestEntry *entries;
} FDBManifestLoad;

typedef struct FDBManifestLimits {
    size_t max_files;
    size_t max_bytes;
    size_t max_key_bytes;
    uint32_t max_levels;
} FDBManifestLimits;

FastDBStatus fdb_manifest_new(FDBStorageMode mode, FDBManifest *out);
bool fdb_manifest_valid_limits(const FDBManifestLimits *limits);
bool fdb_manifest_can_fit(const FDBManifestLimits *limits, size_t files, size_t key_bytes);
bool fdb_manifest_temp_generation(const char *name, uint64_t *generation);
void fdb_manifest_copy_bounds(FDBManifestEntry *entry, unsigned char **next);
/* A loaded view borrows the image and entries until release. */
FastDBStatus fdb_manifest_load(const FDBDirectory *dir, const FDBManifestLimits *limits,
                                FDBManifestLoad *out);
void fdb_manifest_release(FDBManifestLoad *loaded);
/* Serialize commits under the directory's owner. expected_generation=0 initializes. */
FastDBStatus fdb_manifest_publish(const FDBDirectory *dir, const FDBManifestLimits *limits,
                                   uint64_t expected_generation, const FDBManifest *candidate);
/* A successful reservation consumes the ID even if no database file is created. */
FastDBStatus fdb_manifest_reserve_file(const FDBDirectory *dir, const FDBManifestLimits *limits,
                                        FDBManifest *current, uint64_t *id);

#endif
