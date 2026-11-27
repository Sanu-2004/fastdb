#ifndef FASTDB_WAL_H
#define FASTDB_WAL_H

#include "fs.h"
#include "record.h"

enum { FDB_WAL_HEADER_BYTES = 40, FDB_WAL_FRAME_BYTES = 32 };

typedef struct FDBWalLimits {
    size_t max_key_bytes;
    size_t max_value_bytes;
    size_t max_batch_operations;
    size_t max_frame_bytes;
    uint64_t max_segment_bytes;
} FDBWalLimits;

typedef struct FDBWal {
    int fd;
    FDBFs fs;
    FDBWalLimits limits;
    uint64_t offset;
    uint64_t sequence;
    bool poisoned;
    bool dirty;
} FDBWal;

typedef enum FDBWalTail { FDB_WAL_SEALED, FDB_WAL_REPAIR_TAIL } FDBWalTail;

/* Only whole, validated batches are passed to the visitor. Payload lifetime is the call. */
typedef FastDBStatus (*FDBWalVisit)(void *context, const unsigned char *payload,
                                    size_t payload_size, size_t count, uint64_t first_sequence);
typedef struct FDBWalReplay {
    FDBWalVisit visit;
    void *context;
    size_t (*payload_headroom)(void *context);
} FDBWalReplay;

bool fdb_wal_valid_limits(const FDBWalLimits *limits);
FastDBStatus fdb_wal_create(const FDBDirectory *dir, const char *name, uint64_t id,
                            uint64_t base_sequence, const FDBWalLimits *limits, FDBWal *out);
/* The caller selects an authoritative segment. A verified incomplete tail may be truncated. */
FastDBStatus fdb_wal_open(const FDBDirectory *dir, const char *name, uint64_t id,
                          uint64_t base_sequence, const FDBWalLimits *limits,
                          FDBWalTail tail, const FDBWalReplay *replay, FDBWal *out);
FastDBStatus fdb_wal_frame_size(const FDBWal *wal, const FDBRecord *records,
                                size_t count, size_t *size);
FastDBStatus fdb_wal_append(FDBWal *wal, const FDBRecord *records, size_t count, bool durable);
FastDBStatus fdb_wal_sync(FDBWal *wal);
FastDBStatus fdb_wal_close(FDBWal *wal);

#endif
