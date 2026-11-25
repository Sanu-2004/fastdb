#include "record.h"

#include <string.h>

int fdb_compare(FastDBSlice a, FastDBSlice b)
{
    size_t common = a.size < b.size ? a.size : b.size;
    int order = common == 0 ? 0 : memcmp(a.data, b.data, common);
    if (order != 0)
        return order;
    return (a.size > b.size) - (a.size < b.size);
}

bool fdb_add_size(size_t a, size_t b, size_t *sum)
{
    if (b > SIZE_MAX - a)
        return false;
    *sum = a + b;
    return true;
}

void fdb_encode_u32(unsigned char *out, uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i)
        out[i] = (unsigned char)(value >> (i * 8));
}

void fdb_encode_u64(unsigned char *out, uint64_t value)
{
    for (unsigned int i = 0; i < 8; ++i)
        out[i] = (unsigned char)(value >> (i * 8));
}

uint32_t fdb_decode_u32(const unsigned char *bytes)
{
    uint32_t value = 0;
    for (unsigned int i = 0; i < 4; ++i)
        value |= (uint32_t)bytes[i] << (i * 8);
    return value;
}

uint64_t fdb_decode_u64(const unsigned char *bytes)
{
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; ++i)
        value |= (uint64_t)bytes[i] << (i * 8);
    return value;
}

static bool valid_fields(FDBRecordKind kind, uint64_t sequence, uint64_t expiry,
                         size_t key_size, size_t value_size)
{
    return key_size != 0 && sequence != 0 &&
           (kind == FDB_PUT || (kind == FDB_DELETE && value_size == 0 && expiry == 0));
}

FastDBStatus fdb_record_size(const FDBRecord *record, size_t *size)
{
    if (!valid_fields(record->kind, record->sequence, record->expires_at_ms,
                      record->key.size, record->value.size) ||
        record->key.data == NULL || (record->value.data == NULL && record->value.size != 0))
        return FASTDB_INVALID_ARGUMENT;
    if (record->key.size > UINT32_MAX || record->value.size > UINT32_MAX ||
        !fdb_add_size(FDB_RECORD_HEADER_BYTES, record->key.size, size) ||
        !fdb_add_size(*size, record->value.size, size))
        return FASTDB_LIMIT_EXCEEDED;
    return FASTDB_OK;
}

FastDBStatus fdb_record_encode(const FDBRecord *record, unsigned char *out,
                               size_t capacity, size_t *written)
{
    *written = 0;
    size_t size;
    FastDBStatus status = fdb_record_size(record, &size);
    if (status != FASTDB_OK)
        return status;
    if (capacity < size)
        return FASTDB_LIMIT_EXCEEDED;
    fdb_encode_u32(out, (uint32_t)record->key.size);
    fdb_encode_u32(out + 4, (uint32_t)record->value.size);
    fdb_encode_u64(out + 8, record->sequence);
    fdb_encode_u64(out + 16, record->expires_at_ms);
    out[24] = (unsigned char)record->kind;
    memset(out + 25, 0, 3);
    memcpy(out + FDB_RECORD_HEADER_BYTES, record->key.data, record->key.size);
    if (record->value.size != 0)
        memcpy(out + FDB_RECORD_HEADER_BYTES + record->key.size,
               record->value.data, record->value.size);
    *written = size;
    return FASTDB_OK;
}

FastDBStatus fdb_record_decode(const unsigned char *bytes, size_t available,
                               size_t max_key, size_t max_value, FDBRecord *out,
                               size_t *consumed)
{
    *consumed = 0;
    *out = (FDBRecord){0};
    if (available < FDB_RECORD_HEADER_BYTES)
        return FASTDB_CORRUPTION;
    size_t key_size = fdb_decode_u32(bytes);
    size_t value_size = fdb_decode_u32(bytes + 4);
    uint64_t sequence = fdb_decode_u64(bytes + 8);
    uint64_t expiry = fdb_decode_u64(bytes + 16);
    FDBRecordKind kind = (FDBRecordKind)bytes[24];
    size_t room = available - FDB_RECORD_HEADER_BYTES;
    if (key_size > max_key || value_size > max_value || key_size > room ||
        value_size > room - key_size || bytes[25] != 0 || bytes[26] != 0 ||
        bytes[27] != 0 || !valid_fields(kind, sequence, expiry, key_size, value_size))
        return FASTDB_CORRUPTION;
    const unsigned char *key = bytes + FDB_RECORD_HEADER_BYTES;
    *out = (FDBRecord){{key, key_size}, {key + key_size, value_size}, sequence, expiry, kind};
    *consumed = FDB_RECORD_HEADER_BYTES + key_size + value_size;
    return FASTDB_OK;
}
