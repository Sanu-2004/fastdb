#ifndef FASTDB_RECORD_H
#define FASTDB_RECORD_H

#include "fastdb/fastdb.h"

enum { FDB_RECORD_HEADER_BYTES = 28 };

typedef enum FDBRecordKind { FDB_PUT = 1, FDB_DELETE = 2 } FDBRecordKind;

typedef struct FDBRecord {
    FastDBSlice key;
    FastDBSlice value;
    uint64_t sequence;
    uint64_t expires_at_ms;
    FDBRecordKind kind;
} FDBRecord;

int fdb_compare(FastDBSlice a, FastDBSlice b);
bool fdb_add_size(size_t a, size_t b, size_t *sum);
uint64_t fdb_saturating_add_u64(uint64_t a, uint64_t b);
FastDBStatus fdb_read_time(uint64_t *milliseconds);
bool fdb_record_visible(FDBRecordKind kind, uint64_t expires_at_ms, uint64_t read_time);
void fdb_encode_u32(unsigned char *out, uint32_t value);
void fdb_encode_u64(unsigned char *out, uint64_t value);
uint32_t fdb_decode_u32(const unsigned char *bytes);
uint64_t fdb_decode_u64(const unsigned char *bytes);
FastDBStatus fdb_record_size(const FDBRecord *record, size_t *size);
FastDBStatus fdb_record_encode(const FDBRecord *record, unsigned char *out,
                               size_t capacity, size_t *written);
FastDBStatus fdb_record_decode(const unsigned char *bytes, size_t available,
                               size_t max_key, size_t max_value, FDBRecord *out,
                               size_t *consumed);

#endif
