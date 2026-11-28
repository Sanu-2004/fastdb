#include "block_codec.h"
#include "crc32c.h"
#include "sstable_format.h"

#ifdef FASTDB_HAVE_LZ4
#define LZ4_STATIC_LINKING_ONLY
#include <lz4.h>
#endif
#ifdef FASTDB_HAVE_ZSTD
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#endif

uint32_t fdb_block_codec_magic(uint32_t codec)
{
    switch (codec) {
#ifdef FASTDB_HAVE_LZ4
        case FASTDB_COMPRESSION_LZ4: return SST_LZ4_BLOCK_MAGIC;
#endif
#ifdef FASTDB_HAVE_ZSTD
        case FASTDB_COMPRESSION_ZSTD: return SST_ZSTD_BLOCK_MAGIC;
#endif
        default: return 0;
    }
}

bool fdb_block_codec_supported(uint32_t codec)
{
    return codec == FASTDB_COMPRESSION_NONE || fdb_block_codec_magic(codec) != 0;
}

static size_t encode_state_size(uint32_t codec)
{
    switch (codec) {
#ifdef FASTDB_HAVE_LZ4
        case FASTDB_COMPRESSION_LZ4: return (size_t)LZ4_sizeofState();
#endif
#ifdef FASTDB_HAVE_ZSTD
        case FASTDB_COMPRESSION_ZSTD: return ZSTD_estimateCCtxSize(1);
#endif
        default: return 0;
    }
}

bool fdb_block_encode_workspace(uint32_t codec, size_t max_block, size_t *out)
{
    *out = 0;
    if (codec == FASTDB_COMPRESSION_NONE)
        return true;
    return fdb_block_codec_supported(codec) &&
           fdb_add_size(max_block, encode_state_size(codec), out);
}

bool fdb_block_decode_workspace(uint32_t codec, size_t max_block, size_t *data, size_t *state)
{
    *state = 0;
#ifdef FASTDB_HAVE_ZSTD
    if (codec == FASTDB_COMPRESSION_ZSTD) {
        *state = ZSTD_estimateDCtxSize();
        return fdb_add_size(max_block, max_block, data);
    }
#else
    (void)codec;
#endif
#ifdef FASTDB_HAVE_LZ4
    return fdb_add_size(max_block, LZ4_DECOMPRESS_INPLACE_MARGIN(max_block), data);
#else
    *data = max_block;
    return true;
#endif
}

static size_t compress_payload(uint32_t codec, const unsigned char *raw, size_t size,
                               void *state, size_t state_size, void *out, size_t capacity)
{
    switch (codec) {
#ifdef FASTDB_HAVE_LZ4
        case FASTDB_COMPRESSION_LZ4: {
            if (size > LZ4_MAX_INPUT_SIZE)
                return 0;
            int length = LZ4_compress_fast_extState(state, (const char *)raw, out,
                                                    (int)size, (int)capacity, 1);
            return length <= 0 ? 0 : (size_t)length;
        }
#endif
#ifdef FASTDB_HAVE_ZSTD
        case FASTDB_COMPRESSION_ZSTD: {
            ZSTD_CCtx *context = ZSTD_initStaticCCtx(state, state_size);
            if (context == NULL)
                return 0;
            size_t length = ZSTD_compressCCtx(context, out, capacity, raw, size, 1);
            return ZSTD_isError(length) ? 0 : length;
        }
#endif
        default:
            (void)raw;
            (void)size;
            (void)state;
            (void)state_size;
            (void)out;
            (void)capacity;
            return 0;
    }
}

size_t fdb_block_encode(uint32_t codec, const unsigned char *raw, size_t size, void *workspace,
                        const unsigned char **out)
{
    size_t target = size - size / 8;
    if (target <= SST_BLOCK_HEADER_BYTES)
        return 0;
    size_t state_size = encode_state_size(codec);
    unsigned char *encoded = (unsigned char *)workspace + state_size;
    unsigned char *payload = encoded + SST_BLOCK_HEADER_BYTES;
    size_t capacity = target - SST_BLOCK_HEADER_BYTES;
    size_t length = compress_payload(codec, raw, size, workspace, state_size, payload, capacity);
    if (length == 0)
        return 0;
    fdb_encode_u32(encoded, fdb_block_codec_magic(codec));
    fdb_encode_u32(encoded + 4, (uint32_t)size);
    fdb_encode_u32(encoded + 8, (uint32_t)length);
    fdb_encode_u32(encoded + 12, fdb_crc32c(payload, length));
    *out = encoded;
    return SST_BLOCK_HEADER_BYTES + length;
}

static bool decompress_payload(uint32_t codec, const void *payload, size_t compressed,
                               void *out, size_t size, void *state, size_t state_size)
{
    switch (codec) {
#ifdef FASTDB_HAVE_LZ4
        case FASTDB_COMPRESSION_LZ4:
            return size <= LZ4_MAX_INPUT_SIZE &&
                   LZ4_decompress_safe(payload, out, (int)compressed, (int)size) == (int)size;
#endif
#ifdef FASTDB_HAVE_ZSTD
        case FASTDB_COMPRESSION_ZSTD: {
            ZSTD_frameHeader frame;
            if (ZSTD_getFrameHeader(&frame, payload, compressed) != 0 ||
                frame.frameType != ZSTD_frame || frame.dictID != 0 || frame.windowSize > size ||
                frame.frameContentSize != size ||
                ZSTD_findFrameCompressedSize(payload, compressed) != compressed)
                return false;
            ZSTD_DCtx *context = ZSTD_initStaticDCtx(state, state_size);
            return context != NULL && ZSTD_decompressDCtx(context, out, size, payload, compressed) == size;
        }
#endif
        default:
            (void)payload;
            (void)compressed;
            (void)out;
            (void)size;
            (void)state;
            (void)state_size;
            return false;
    }
}

FastDBStatus fdb_block_decode(uint32_t codec, const unsigned char *encoded, size_t stored,
                             unsigned char *out, size_t max_block, void *state, size_t state_size,
                             size_t *decoded)
{
    if (stored <= SST_BLOCK_HEADER_BYTES)
        return FASTDB_CORRUPTION;
    size_t size = fdb_decode_u32(encoded + 4);
    size_t compressed = fdb_decode_u32(encoded + 8);
    if (compressed != stored - SST_BLOCK_HEADER_BYTES || stored >= size)
        return FASTDB_CORRUPTION;
    if (size > max_block)
        return FASTDB_LIMIT_EXCEEDED;
    const unsigned char *payload = encoded + SST_BLOCK_HEADER_BYTES;
    if (fdb_decode_u32(encoded + 12) != fdb_crc32c(payload, compressed) ||
        !decompress_payload(codec, payload, compressed, out, size, state, state_size))
        return FASTDB_CORRUPTION;
    *decoded = size;
    return FASTDB_OK;
}
