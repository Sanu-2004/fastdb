#include "wal.h"
#include "crc32c.h"

#include <stdlib.h>
#include <string.h>

static const unsigned char magic[8] = {'F', 'A', 'S', 'T', 'W', 'A', 'L', 0};
static const uint32_t frame_magic = UINT32_C(0x31424446);
enum { WAL_VERSION = 1, MIN_RECORD_BYTES = FDB_RECORD_HEADER_BYTES + 1 };

bool fdb_wal_valid_limits(const FDBWalLimits *limits)
{
    return limits != NULL && limits->max_key_bytes != 0 && limits->max_key_bytes <= UINT32_MAX &&
           limits->max_value_bytes <= UINT32_MAX &&
           limits->max_batch_operations != 0 && limits->max_batch_operations <= UINT32_MAX &&
           limits->max_frame_bytes >= FDB_WAL_FRAME_BYTES + MIN_RECORD_BYTES &&
           limits->max_frame_bytes - FDB_WAL_FRAME_BYTES <= UINT32_MAX &&
           limits->max_segment_bytes >= FDB_WAL_HEADER_BYTES &&
           limits->max_segment_bytes <= INT64_MAX &&
           limits->max_frame_bytes <= limits->max_segment_bytes - FDB_WAL_HEADER_BYTES;
}

static FastDBStatus read_exact(FDBWal *wal, uint64_t offset, void *buffer, size_t length)
{
    size_t read;
    FastDBStatus status = fdb_fs_read(&wal->fs, wal->fd, offset, buffer, length, &read);
    if (status == FASTDB_OK && read != length)
        return FASTDB_CORRUPTION;
    return status;
}

static FastDBStatus handle_failed_open(FDBWal *wal, FastDBStatus status)
{
    (void)fdb_fs_close(wal->fd);
    *wal = (FDBWal){.fd = -1};
    return status;
}

FastDBStatus fdb_wal_create(const FDBDirectory *dir, const char *name, uint64_t id,
                            uint64_t base_sequence, const FDBWalLimits *limits, FDBWal *out)
{
    *out = (FDBWal){.fd = -1};
    if (id == 0 || !fdb_wal_valid_limits(limits))
        return FASTDB_INVALID_ARGUMENT;
    FastDBStatus status = fdb_dir_open_file(dir, name, FDB_FILE_CREATE_NEW, &out->fd);
    if (status != FASTDB_OK)
        return status;
    out->fs = dir->fs;
    out->limits = *limits;
    unsigned char header[FDB_WAL_HEADER_BYTES] = {0};
    memcpy(header, magic, sizeof(magic));
    fdb_encode_u32(header + 8, WAL_VERSION);
    fdb_encode_u64(header + 16, id);
    fdb_encode_u64(header + 24, base_sequence);
    fdb_encode_u32(header + 36, fdb_crc32c(header, 36));

    size_t written;
    status = fdb_fs_write(&dir->fs, out->fd, 0, header, sizeof(header), &written);
    if (status == FASTDB_OK)
        status = fdb_fs_sync(&dir->fs, out->fd);
    if (status == FASTDB_OK)
        status = fdb_dir_sync(dir);
    if (status != FASTDB_OK)
        return handle_failed_open(out, status);
    out->offset = sizeof(header);
    out->sequence = base_sequence;
    return FASTDB_OK;
}

static FastDBStatus validate_header(FDBWal *wal, uint64_t id, uint64_t base_sequence)
{
    unsigned char header[FDB_WAL_HEADER_BYTES];
    FastDBStatus status = read_exact(wal, 0, header, sizeof(header));
    if (status != FASTDB_OK)
        return status;
    if (memcmp(header, magic, sizeof(magic)) != 0 ||
        fdb_decode_u32(header + 36) != fdb_crc32c(header, 36))
        return FASTDB_CORRUPTION;
    if (fdb_decode_u32(header + 8) != WAL_VERSION)
        return FASTDB_UNSUPPORTED;
    if (fdb_decode_u32(header + 12) != 0 || fdb_decode_u32(header + 32) != 0 ||
        fdb_decode_u64(header + 16) != id || fdb_decode_u64(header + 24) != base_sequence)
        return FASTDB_CORRUPTION;
    return FASTDB_OK;
}

static FastDBStatus validate_payload(FDBWal *wal, const unsigned char *payload, size_t length,
                                     size_t count, uint64_t previous_sequence)
{
    for (size_t i = 0; i < count; ++i) {
        FDBRecord record;
        size_t consumed;
        FastDBStatus status = fdb_record_decode(payload, length, wal->limits.max_key_bytes,
                                                 wal->limits.max_value_bytes, &record, &consumed);
        if (status != FASTDB_OK || record.sequence != ++previous_sequence)
            return FASTDB_CORRUPTION;
        payload += consumed;
        length -= consumed;
    }
    return length == 0 ? FASTDB_OK : FASTDB_CORRUPTION;
}

static FastDBStatus recover_payload(FDBWal *wal, size_t length, size_t count, uint32_t checksum,
                                    const FDBWalReplay *replay)
{
    if (replay != NULL && replay->payload_headroom != NULL &&
        length > replay->payload_headroom(replay->context))
        return FASTDB_LIMIT_EXCEEDED;
    unsigned char *payload = malloc(length);
    if (payload == NULL)
        return FASTDB_OUT_OF_MEMORY;
    FastDBStatus status = read_exact(wal, wal->offset + FDB_WAL_FRAME_BYTES, payload, length);
    if (status == FASTDB_OK && fdb_crc32c(payload, length) != checksum)
        status = FASTDB_CORRUPTION;
    if (status == FASTDB_OK)
        status = validate_payload(wal, payload, length, count, wal->sequence);
    if (status == FASTDB_OK && replay != NULL && replay->visit != NULL)
        status = replay->visit(replay->context, payload, length, count, wal->sequence + 1);
    free(payload);
    return status;
}

static FastDBStatus repair_tail(FDBWal *wal)
{
    FastDBStatus status = fdb_fs_truncate(&wal->fs, wal->fd, wal->offset);
    if (status == FASTDB_OK)
        status = fdb_fs_sync(&wal->fs, wal->fd);
    if (status == FASTDB_OK)
        wal->dirty = false;
    return status;
}

static FastDBStatus scan_wal(FDBWal *wal, uint64_t file_size, FDBWalTail tail,
                             const FDBWalReplay *replay)
{
    unsigned char header[FDB_WAL_FRAME_BYTES];
    while (wal->offset < file_size) {
        uint64_t left = file_size - wal->offset;
        size_t head_size = left < sizeof(header) ? (size_t)left : sizeof(header);
        FastDBStatus status = read_exact(wal, wal->offset, header, head_size);
        if (status != FASTDB_OK)
            return status;
        unsigned char expected_magic[4];
        fdb_encode_u32(expected_magic, frame_magic);
        size_t comparable = head_size < sizeof(expected_magic) ? head_size : sizeof(expected_magic);
        if (memcmp(header, expected_magic, comparable) != 0)
            return FASTDB_CORRUPTION;
        if (head_size < sizeof(header))
            return tail == FDB_WAL_REPAIR_TAIL ? repair_tail(wal) : FASTDB_CORRUPTION;

        size_t length = fdb_decode_u32(header + 4);
        size_t count = fdb_decode_u32(header + 8);
        if (fdb_decode_u32(header + 28) != fdb_crc32c(header, 28) ||
            fdb_decode_u32(header + 24) != 0 || count == 0 || count > length / MIN_RECORD_BYTES ||
            count > wal->limits.max_batch_operations ||
            length > wal->limits.max_frame_bytes - FDB_WAL_FRAME_BYTES ||
            count > UINT64_MAX - wal->sequence || fdb_decode_u64(header + 16) != wal->sequence + 1)
            return FASTDB_CORRUPTION;
        if (length > left - sizeof(header)) {
            if (length > wal->limits.max_segment_bytes - wal->offset - sizeof(header))
                return FASTDB_CORRUPTION;
            return tail == FDB_WAL_REPAIR_TAIL ? repair_tail(wal) : FASTDB_CORRUPTION;
        }
        status = recover_payload(wal, length, count, fdb_decode_u32(header + 12), replay);
        if (status != FASTDB_OK)
            return status;
        wal->offset += sizeof(header) + length;
        wal->sequence += count;
        wal->dirty = true;
    }
    return FASTDB_OK;
}

FastDBStatus fdb_wal_open(const FDBDirectory *dir, const char *name, uint64_t id,
                          uint64_t base_sequence, const FDBWalLimits *limits,
                          FDBWalTail tail, const FDBWalReplay *replay, FDBWal *out)
{
    *out = (FDBWal){.fd = -1};
    if (id == 0 || !fdb_wal_valid_limits(limits))
        return FASTDB_INVALID_ARGUMENT;
    FastDBStatus status = fdb_dir_open_file(dir, name, FDB_FILE_READ_WRITE, &out->fd);
    if (status != FASTDB_OK)
        return status == FASTDB_NOT_FOUND ? FASTDB_CORRUPTION : status;
    out->fs = dir->fs;
    out->limits = *limits;
    uint64_t file_size;
    status = fdb_fs_size(out->fd, &file_size);
    if (status == FASTDB_OK && file_size > limits->max_segment_bytes)
        status = FASTDB_LIMIT_EXCEEDED;
    if (status == FASTDB_OK)
        status = validate_header(out, id, base_sequence);
    if (status == FASTDB_OK) {
        out->offset = FDB_WAL_HEADER_BYTES;
        out->sequence = base_sequence;
        status = scan_wal(out, file_size, tail, replay);
    }
    return status == FASTDB_OK ? status : handle_failed_open(out, status);
}

FastDBStatus fdb_wal_frame_size(const FDBWal *wal, const FDBRecord *records,
                                size_t count, size_t *size)
{
    *size = 0;
    if (count == 0 || records == NULL)
        return FASTDB_INVALID_ARGUMENT;
    if (count > wal->limits.max_batch_operations || count > UINT64_MAX - wal->sequence)
        return FASTDB_LIMIT_EXCEEDED;
    size_t total = FDB_WAL_FRAME_BYTES;
    for (size_t i = 0; i < count; ++i) {
        const FDBRecord *record = &records[i];
        if (record->sequence != wal->sequence + i + 1)
            return FASTDB_INVALID_ARGUMENT;
        if (record->key.size > wal->limits.max_key_bytes ||
            record->value.size > wal->limits.max_value_bytes)
            return FASTDB_LIMIT_EXCEEDED;
        size_t record_size;
        FastDBStatus status = fdb_record_size(record, &record_size);
        if (status != FASTDB_OK)
            return status;
        if (record_size > wal->limits.max_frame_bytes - total)
            return FASTDB_LIMIT_EXCEEDED;
        total += record_size;
    }
    *size = total;
    return FASTDB_OK;
}

FastDBStatus fdb_wal_sync(FDBWal *wal)
{
    if (wal->poisoned)
        return FASTDB_COMMIT_UNCERTAIN;
    if (fdb_fs_sync(&wal->fs, wal->fd) == FASTDB_OK) {
        wal->dirty = false;
        return FASTDB_OK;
    }
    wal->poisoned = true;
    return FASTDB_COMMIT_UNCERTAIN;
}

FastDBStatus fdb_wal_append(FDBWal *wal, const FDBRecord *records, size_t count, bool durable)
{
    if (wal->poisoned)
        return FASTDB_COMMIT_UNCERTAIN;
    size_t length;
    FastDBStatus status = fdb_wal_frame_size(wal, records, count, &length);
    if (status != FASTDB_OK)
        return status;
    if (length > wal->limits.max_segment_bytes - wal->offset)
        return FASTDB_LIMIT_EXCEEDED;
    unsigned char *frame = malloc(length);
    if (frame == NULL)
        return FASTDB_OUT_OF_MEMORY;
    size_t position = FDB_WAL_FRAME_BYTES;
    for (size_t i = 0; i < count; ++i) {
        size_t encoded;
        status = fdb_record_encode(&records[i], frame + position, length - position, &encoded);
        if (status != FASTDB_OK) {
            free(frame);
            return status;
        }
        position += encoded;
    }
    size_t payload_size = length - FDB_WAL_FRAME_BYTES;
    fdb_encode_u32(frame, frame_magic);
    fdb_encode_u32(frame + 4, (uint32_t)payload_size);
    fdb_encode_u32(frame + 8, (uint32_t)count);
    fdb_encode_u32(frame + 12, fdb_crc32c(frame + FDB_WAL_FRAME_BYTES, payload_size));
    fdb_encode_u64(frame + 16, records[0].sequence);
    fdb_encode_u32(frame + 24, 0);
    fdb_encode_u32(frame + 28, fdb_crc32c(frame, 28));
    size_t written;
    status = fdb_fs_write(&wal->fs, wal->fd, wal->offset, frame, length, &written);
    free(frame);
    if (status != FASTDB_OK) {
        wal->poisoned = true;
        return FASTDB_COMMIT_UNCERTAIN;
    }
    wal->dirty = true;
    if (durable && fdb_wal_sync(wal) != FASTDB_OK)
        return FASTDB_COMMIT_UNCERTAIN;
    wal->offset += length;
    wal->sequence += count;
    return FASTDB_OK;
}

FastDBStatus fdb_wal_close(FDBWal *wal)
{
    FastDBStatus status = fdb_fs_close(wal->fd);
    if (wal->poisoned && status == FASTDB_OK)
        status = FASTDB_COMMIT_UNCERTAIN;
    *wal = (FDBWal){.fd = -1};
    return status;
}
