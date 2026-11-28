#include "block_codec.h"
#include "crc32c.h"
#include "sstable_format.h"

#ifdef FASTDB_HAVE_LZ4
#define LZ4_STATIC_LINKING_ONLY
#include <lz4.h>
#endif

bool fdb_block_codec_supported(uint32_t codec)
{
    if (codec == FASTDB_COMPRESSION_NONE)
        return true;
#ifdef FASTDB_HAVE_LZ4
    return codec == FASTDB_COMPRESSION_LZ4;
#else
    return false;
#endif
}

bool fdb_block_encode_workspace(uint32_t codec, size_t max_block, size_t *out)
{
    *out = 0;
    if (codec == FASTDB_COMPRESSION_NONE)
        return true;
#ifdef FASTDB_HAVE_LZ4
    return codec == FASTDB_COMPRESSION_LZ4 && fdb_add_size(max_block, (size_t)LZ4_sizeofState(), out);
#else
    (void)max_block;
    return false;
#endif
}

bool fdb_block_decode_workspace(size_t max_block, size_t *out)
{
#ifdef FASTDB_HAVE_LZ4
    return fdb_add_size(max_block, LZ4_DECOMPRESS_INPLACE_MARGIN(max_block), out);
#else
    *out = max_block;
    return true;
#endif
}

size_t fdb_block_lz4_encode(const unsigned char *raw, size_t size, void *workspace,
                             const unsigned char **out)
{
#ifdef FASTDB_HAVE_LZ4
    if (size > LZ4_MAX_INPUT_SIZE || size - size / 8 <= SST_BLOCK_HEADER_BYTES)
        return 0;
    unsigned char *encoded = (unsigned char *)workspace + LZ4_sizeofState();
    unsigned char *payload = encoded + SST_BLOCK_HEADER_BYTES;
    size_t capacity = size - size / 8 - SST_BLOCK_HEADER_BYTES;
    int length = LZ4_compress_fast_extState(workspace, (const char *)raw, (char *)payload,
                                            (int)size, (int)capacity, 1);
    if (length <= 0)
        return 0;
    fdb_encode_u32(encoded, SST_LZ4_BLOCK_MAGIC);
    fdb_encode_u32(encoded + 4, (uint32_t)size);
    fdb_encode_u32(encoded + 8, (uint32_t)length);
    fdb_encode_u32(encoded + 12, fdb_crc32c(payload, (size_t)length));
    *out = encoded;
    return SST_BLOCK_HEADER_BYTES + (size_t)length;
#else
    (void)raw;
    (void)size;
    (void)workspace;
    (void)out;
    return 0;
#endif
}

FastDBStatus fdb_block_lz4_decode(const unsigned char *encoded, size_t stored,
                                  unsigned char *out, size_t max_block, size_t *decoded)
{
#ifdef FASTDB_HAVE_LZ4
    if (stored <= SST_BLOCK_HEADER_BYTES)
        return FASTDB_CORRUPTION;
    size_t size = fdb_decode_u32(encoded + 4);
    size_t compressed = fdb_decode_u32(encoded + 8);
    if (compressed != stored - SST_BLOCK_HEADER_BYTES || stored >= size || size > LZ4_MAX_INPUT_SIZE)
        return FASTDB_CORRUPTION;
    if (size > max_block)
        return FASTDB_LIMIT_EXCEEDED;
    const unsigned char *payload = encoded + SST_BLOCK_HEADER_BYTES;
    if (fdb_decode_u32(encoded + 12) != fdb_crc32c(payload, compressed) ||
        LZ4_decompress_safe((const char *)payload, (char *)out, (int)compressed, (int)size) != (int)size)
        return FASTDB_CORRUPTION;
    *decoded = size;
    return FASTDB_OK;
#else
    (void)encoded;
    (void)stored;
    (void)out;
    (void)max_block;
    (void)decoded;
    return FASTDB_UNSUPPORTED;
#endif
}
