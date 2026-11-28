#ifndef FASTDB_BLOCK_CODEC_H
#define FASTDB_BLOCK_CODEC_H

#include "fastdb/fastdb.h"

bool fdb_block_codec_supported(uint32_t codec);
uint32_t fdb_block_codec_magic(uint32_t codec);
bool fdb_block_encode_workspace(uint32_t codec, size_t max_block, size_t *out);
bool fdb_block_decode_workspace(uint32_t codec, size_t max_block, size_t *data, size_t *state);
/* Zero means store the original block. The workspace is caller-owned. */
size_t fdb_block_encode(uint32_t codec, const unsigned char *raw, size_t size, void *workspace,
                        const unsigned char **out);
/* LZ4's input ends at the end of the combined buffer. Zstd uses disjoint buffers. */
FastDBStatus fdb_block_decode(uint32_t codec, const unsigned char *encoded, size_t stored,
                             unsigned char *out, size_t max_block, void *state, size_t state_size,
                             size_t *decoded);

#endif
