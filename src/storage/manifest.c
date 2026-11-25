#include "manifest.h"
#include "crc32c.h"
#include "record.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

static const unsigned char magic[8] = {'F', 'A', 'S', 'T', 'M', 'A', 'N', 0};
static const char manifest_name[] = "MANIFEST";
enum { VERSION = 1, HEADER_BYTES = 68, ENTRY_BYTES = 48, CRC_BYTES = 4, TEMP_NAME_BYTES = 64 };

void fdb_manifest_copy_bounds(FDBManifestEntry *entry, unsigned char **next)
{
    FastDBSlice *bounds[] = {&entry->min_key, &entry->max_key};
    for (size_t i = 0; i < sizeof(bounds) / sizeof(*bounds); ++i) {
        FastDBSlice *bound = bounds[i];
        if (bound->size == 0) {
            bound->data = NULL;
            continue;
        }
        memcpy(*next, bound->data, bound->size);
        bound->data = *next;
        *next += bound->size;
    }
}

bool fdb_manifest_temp_generation(const char *name, uint64_t *generation)
{
    static const char prefix[] = ".manifest-";
    if (strncmp(name, prefix, sizeof(prefix) - 1) != 0)
        return false;
    const char *rest = name + sizeof(prefix) - 1;
    if (*rest < '1' || *rest > '9')
        return false;
    uint64_t value = 0;
    for (; *rest >= '0' && *rest <= '9'; ++rest) {
        unsigned int digit = (unsigned int)(*rest - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    if (*rest != '-' || strlen(rest) != 1 + 16 + 4)
        return false;
    ++rest;
    for (size_t i = 0; i < 16; ++i) {
        char ch = rest[i];
        if ((ch < '0' || ch > '9') && (ch < 'a' || ch > 'f'))
            return false;
    }
    if (strcmp(rest + 16, ".tmp") != 0)
        return false;
    *generation = value;
    return true;
}

static bool valid_mode(FDBStorageMode mode)
{
    return mode == FDB_MODE_MEMORY_PERSISTENT || mode == FDB_MODE_LSM;
}

bool fdb_manifest_valid_limits(const FDBManifestLimits *limits)
{
    return limits != NULL && limits->max_files > 0 && limits->max_files <= UINT32_MAX &&
           limits->max_key_bytes > 0 && limits->max_key_bytes <= UINT32_MAX &&
           limits->max_levels > 0 && limits->max_bytes >= HEADER_BYTES + CRC_BYTES &&
           limits->max_bytes - HEADER_BYTES - CRC_BYTES <= UINT32_MAX;
}

bool fdb_manifest_can_fit(const FDBManifestLimits *limits, size_t files, size_t key_bytes)
{
    size_t available = limits->max_bytes - HEADER_BYTES - CRC_BYTES;
    return files <= limits->max_files && key_bytes <= available &&
           files <= (available - key_bytes) / ENTRY_BYTES;
}

static bool valid_identity(const unsigned char id[16])
{
    static const unsigned char zero[16] = {0};
    return memcmp(id, zero, sizeof(zero)) != 0;
}

static FastDBStatus random_bytes(void *out, size_t size)
{
    unsigned char *bytes = out;
    size_t done = 0;
    while (done < size) {
        ssize_t result = getrandom(bytes + done, size - done, 0);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return FASTDB_IO_ERROR;
        done += (size_t)result;
    }
    return FASTDB_OK;
}

FastDBStatus fdb_manifest_new(FDBStorageMode mode, FDBManifest *out)
{
    *out = (FDBManifest){0};
    if (!valid_mode(mode))
        return FASTDB_INVALID_ARGUMENT;
    FDBManifest initial = {.mode = mode, .generation = 1, .next_file_id = 1};
    FastDBStatus status = random_bytes(initial.database_id, sizeof(initial.database_id));
    if (status != FASTDB_OK || !valid_identity(initial.database_id))
        return FASTDB_IO_ERROR;
    *out = initial;
    return FASTDB_OK;
}

static bool valid_entry(const FDBManifest *view, const FDBManifestLimits *limits,
                        const FDBManifestEntry *entry)
{
    if (entry->id == 0 || entry->id >= view->next_file_id || entry->bytes == 0 ||
        entry->bytes > INT64_MAX || entry->sequence_begin > view->sequence ||
        entry->sequence_end > view->sequence ||
        entry->min_key.size > limits->max_key_bytes || entry->max_key.size > limits->max_key_bytes)
        return false;
    if (entry->kind == FDB_FILE_WAL)
        return entry->level == 0 && entry->min_key.size == 0 && entry->max_key.size == 0 &&
               (entry->sequence_end == 0 || entry->sequence_begin <= entry->sequence_end);

    if (entry->sequence_begin > entry->sequence_end)
        return false;
    if (entry->kind == FDB_FILE_CHECKPOINT) {
        if (view->mode != FDB_MODE_MEMORY_PERSISTENT || entry->level != 0)
            return false;
        if (entry->min_key.size == 0 && entry->max_key.size == 0)
            return true;
    } else if (entry->kind != FDB_FILE_SSTABLE || view->mode != FDB_MODE_LSM ||
               entry->level >= limits->max_levels || entry->sequence_begin == 0) {
        return false;
    }
    return entry->min_key.size != 0 && entry->max_key.size != 0 &&
           entry->min_key.data != NULL && entry->max_key.data != NULL &&
           fdb_compare(entry->min_key, entry->max_key) <= 0;
}

static FastDBStatus image_size(const FDBManifest *view, const FDBManifestLimits *limits,
                               size_t *size)
{
    if (!valid_mode(view->mode) || view->generation == 0 || view->next_file_id == 0 ||
        !valid_identity(view->database_id) || (view->count != 0 && view->entries == NULL))
        return FASTDB_INVALID_ARGUMENT;
    if (view->count > limits->max_files)
        return FASTDB_LIMIT_EXCEEDED;
    size_t bytes = HEADER_BYTES + CRC_BYTES;
    uint64_t previous_id = 0;
    bool checkpoint = false;
    for (size_t i = 0; i < view->count; ++i) {
        const FDBManifestEntry *entry = &view->entries[i];
        if (!valid_entry(view, limits, entry) || entry->id <= previous_id)
            return FASTDB_INVALID_ARGUMENT;
        if (entry->kind == FDB_FILE_CHECKPOINT) {
            if (checkpoint)
                return FASTDB_INVALID_ARGUMENT;
            checkpoint = true;
        }
        size_t length;
        if (!fdb_add_size(ENTRY_BYTES, entry->min_key.size, &length) ||
            !fdb_add_size(length, entry->max_key.size, &length) ||
            length > limits->max_bytes - bytes)
            return FASTDB_LIMIT_EXCEEDED;
        bytes += length;
        previous_id = entry->id;
    }
    *size = bytes;
    return FASTDB_OK;
}

static void encode_entry(unsigned char *bytes, const FDBManifestEntry *entry)
{
    fdb_encode_u64(bytes, entry->id);
    fdb_encode_u32(bytes + 8, (uint32_t)entry->kind);
    fdb_encode_u32(bytes + 12, entry->level);
    fdb_encode_u64(bytes + 16, entry->bytes);
    fdb_encode_u64(bytes + 24, entry->sequence_begin);
    fdb_encode_u64(bytes + 32, entry->sequence_end);
    fdb_encode_u32(bytes + 40, (uint32_t)entry->min_key.size);
    fdb_encode_u32(bytes + 44, (uint32_t)entry->max_key.size);
    if (entry->min_key.size != 0)
        memcpy(bytes + ENTRY_BYTES, entry->min_key.data, entry->min_key.size);
    if (entry->max_key.size != 0)
        memcpy(bytes + ENTRY_BYTES + entry->min_key.size, entry->max_key.data, entry->max_key.size);
}

static void encode(unsigned char *image, size_t size, const FDBManifest *view)
{
    memcpy(image, magic, sizeof(magic));
    fdb_encode_u32(image + 8, VERSION);
    fdb_encode_u32(image + 12, (uint32_t)view->mode);
    memcpy(image + 16, view->database_id, sizeof(view->database_id));
    fdb_encode_u64(image + 32, view->generation);
    fdb_encode_u64(image + 40, view->next_file_id);
    fdb_encode_u64(image + 48, view->sequence);
    fdb_encode_u32(image + 56, (uint32_t)view->count);
    size_t payload_size = size - HEADER_BYTES - CRC_BYTES;
    fdb_encode_u32(image + 60, (uint32_t)payload_size);
    fdb_encode_u32(image + 64, fdb_crc32c(image, 64));
    unsigned char *bytes = image + HEADER_BYTES;
    for (size_t i = 0; i < view->count; ++i) {
        const FDBManifestEntry *entry = &view->entries[i];
        encode_entry(bytes, entry);
        bytes += ENTRY_BYTES + entry->min_key.size + entry->max_key.size;
    }
    fdb_encode_u32(bytes, fdb_crc32c(image + HEADER_BYTES, payload_size));
}

static FastDBStatus decode_header(const unsigned char *bytes, FDBManifest *view)
{
    if (memcmp(bytes, magic, sizeof(magic)) != 0 ||
        fdb_decode_u32(bytes + 64) != fdb_crc32c(bytes, 64))
        return FASTDB_CORRUPTION;
    if (fdb_decode_u32(bytes + 8) != VERSION)
        return FASTDB_UNSUPPORTED;
    view->mode = (FDBStorageMode)fdb_decode_u32(bytes + 12);
    memcpy(view->database_id, bytes + 16, sizeof(view->database_id));
    view->generation = fdb_decode_u64(bytes + 32);
    view->next_file_id = fdb_decode_u64(bytes + 40);
    view->sequence = fdb_decode_u64(bytes + 48);
    view->count = fdb_decode_u32(bytes + 56);
    return FASTDB_OK;
}

static size_t decode_entry(const unsigned char *bytes, size_t available, FDBManifestEntry *entry)
{
    if (available < ENTRY_BYTES)
        return 0;
    size_t min_size = fdb_decode_u32(bytes + 40);
    size_t max_size = fdb_decode_u32(bytes + 44);
    if (min_size > available - ENTRY_BYTES || max_size > available - ENTRY_BYTES - min_size)
        return 0;
    const unsigned char *keys = bytes + ENTRY_BYTES;
    *entry = (FDBManifestEntry){
        .id = fdb_decode_u64(bytes), .kind = (FDBFileKind)fdb_decode_u32(bytes + 8),
        .level = fdb_decode_u32(bytes + 12), .bytes = fdb_decode_u64(bytes + 16),
        .sequence_begin = fdb_decode_u64(bytes + 24), .sequence_end = fdb_decode_u64(bytes + 32),
        .min_key = {keys, min_size}, .max_key = {keys + min_size, max_size},
    };
    return ENTRY_BYTES + min_size + max_size;
}

static FastDBStatus decode(FDBManifestLoad *load, size_t size, const FDBManifestLimits *limits)
{
    FDBManifest *view = &load->view;
    const unsigned char *image = load->image;
    FastDBStatus status = decode_header(image, view);
    if (status != FASTDB_OK)
        return status;
    size_t payload = size - HEADER_BYTES - CRC_BYTES;
    const unsigned char *bytes = image + HEADER_BYTES;
    if (payload != fdb_decode_u32(image + 60) || view->count > payload / ENTRY_BYTES ||
        fdb_decode_u32(bytes + payload) != fdb_crc32c(bytes, payload))
        return FASTDB_CORRUPTION;
    if (view->count > limits->max_files || view->count > SIZE_MAX / sizeof(*load->entries))
        return FASTDB_LIMIT_EXCEEDED;
    if (view->count != 0) {
        load->entries = malloc(view->count * sizeof(*load->entries));
        if (load->entries == NULL)
            return FASTDB_OUT_OF_MEMORY;
    }
    view->entries = load->entries;
    for (size_t i = 0; i < view->count; ++i) {
        size_t used = decode_entry(bytes, payload, &load->entries[i]);
        if (used == 0)
            return FASTDB_CORRUPTION;
        bytes += used;
        payload -= used;
    }
    size_t checked_size;
    if (payload != 0 || image_size(view, limits, &checked_size) != FASTDB_OK || checked_size != size)
        return FASTDB_CORRUPTION;
    return FASTDB_OK;
}

void fdb_manifest_release(FDBManifestLoad *loaded)
{
    free(loaded->entries);
    free(loaded->image);
    *loaded = (FDBManifestLoad){0};
}

static FastDBStatus read_image(const FDBDirectory *dir, int fd, const FDBManifestLimits *limits,
                                FDBManifestLoad *out)
{
    uint64_t size;
    FastDBStatus status = fdb_fs_size(fd, &size);
    if (status != FASTDB_OK)
        return status;
    if (size < HEADER_BYTES + CRC_BYTES)
        return FASTDB_CORRUPTION;
    if (size > limits->max_bytes)
        return FASTDB_LIMIT_EXCEEDED;
    out->image = malloc((size_t)size);
    if (out->image == NULL)
        return FASTDB_OUT_OF_MEMORY;
    size_t read;
    status = fdb_fs_read(&dir->fs, fd, 0, out->image, (size_t)size, &read);
    if (status == FASTDB_OK && read != size)
        return FASTDB_CORRUPTION;
    return status == FASTDB_OK ? decode(out, (size_t)size, limits) : status;
}

FastDBStatus fdb_manifest_load(const FDBDirectory *dir, const FDBManifestLimits *limits,
                                FDBManifestLoad *out)
{
    *out = (FDBManifestLoad){0};
    if (!fdb_manifest_valid_limits(limits))
        return FASTDB_INVALID_ARGUMENT;
    int fd;
    FastDBStatus status = fdb_dir_open_file(dir, manifest_name, FDB_FILE_READ, &fd);
    if (status != FASTDB_OK)
        return status;
    status = read_image(dir, fd, limits, out);
    FastDBStatus close_status = fdb_fs_close(fd);
    if (status == FASTDB_OK)
        status = close_status;
    if (status != FASTDB_OK)
        fdb_manifest_release(out);
    return status;
}

static FastDBStatus check_current(const FDBDirectory *dir, const FDBManifestLimits *limits,
                                   uint64_t expected, const FDBManifest *next)
{
    if (expected == UINT64_MAX || next->generation != expected + 1)
        return FASTDB_CONFLICT;
    FDBManifestLoad current;
    FastDBStatus status = fdb_manifest_load(dir, limits, &current);
    if (status == FASTDB_NOT_FOUND)
        return expected == 0 && next->count == 0 && next->next_file_id == 1 &&
               next->sequence == 0 ? FASTDB_OK : FASTDB_CONFLICT;
    if (status != FASTDB_OK)
        return status;
    if (expected == 0 || current.view.generation != expected ||
        current.view.mode != next->mode ||
        memcmp(current.view.database_id, next->database_id, sizeof(next->database_id)) != 0 ||
        current.view.next_file_id > next->next_file_id || current.view.sequence > next->sequence)
        status = FASTDB_CONFLICT;
    fdb_manifest_release(&current);
    return status;
}

static FastDBStatus publish_image(const FDBDirectory *dir, const unsigned char *image,
                                   size_t size, uint64_t generation)
{
    uint64_t nonce;
    FastDBStatus status = random_bytes(&nonce, sizeof(nonce));
    if (status != FASTDB_OK)
        return status;
    char name[TEMP_NAME_BYTES];
    int length = snprintf(name, sizeof(name), ".manifest-%" PRIu64 "-%016" PRIx64 ".tmp",
                          generation, nonce);
    if (length < 0 || (size_t)length >= sizeof(name))
        return FASTDB_IO_ERROR;
    int fd;
    status = fdb_dir_open_file(dir, name, FDB_FILE_CREATE_NEW, &fd);
    if (status != FASTDB_OK)
        return status;
    size_t written;
    status = fdb_fs_write(&dir->fs, fd, 0, image, size, &written);
    if (status == FASTDB_OK)
        status = fdb_fs_sync(&dir->fs, fd);
    FastDBStatus close_status = fdb_fs_close(fd);
    if (status == FASTDB_OK)
        status = close_status;
    if (status == FASTDB_OK)
        status = fdb_dir_rename(dir, name, manifest_name);
    if (status != FASTDB_OK) {
        /* A failed cleanup leaves an unauthoritative, uniquely named temporary file. */
        (void)fdb_dir_unlink(dir, name);
        return status;
    }
    return fdb_dir_sync(dir) == FASTDB_OK ? FASTDB_OK : FASTDB_COMMIT_UNCERTAIN;
}

FastDBStatus fdb_manifest_publish(const FDBDirectory *dir, const FDBManifestLimits *limits,
                                   uint64_t expected_generation, const FDBManifest *candidate)
{
    if (!fdb_manifest_valid_limits(limits))
        return FASTDB_INVALID_ARGUMENT;
    size_t size;
    FastDBStatus status = image_size(candidate, limits, &size);
    if (status != FASTDB_OK)
        return status;
    status = check_current(dir, limits, expected_generation, candidate);
    if (status != FASTDB_OK)
        return status;
    unsigned char *image = malloc(size);
    if (image == NULL)
        return FASTDB_OUT_OF_MEMORY;
    encode(image, size, candidate);
    status = publish_image(dir, image, size, candidate->generation);
    free(image);
    return status;
}

FastDBStatus fdb_manifest_reserve_file(const FDBDirectory *dir, const FDBManifestLimits *limits,
                                        FDBManifest *current, uint64_t *id)
{
    *id = 0;
    if (current->next_file_id == UINT64_MAX || current->generation == UINT64_MAX)
        return FASTDB_LIMIT_EXCEEDED;
    FDBManifest next = *current;
    ++next.next_file_id;
    ++next.generation;
    FastDBStatus status = fdb_manifest_publish(dir, limits, current->generation, &next);
    if (status == FASTDB_OK) {
        *id = current->next_file_id;
        *current = next;
    }
    return status;
}
