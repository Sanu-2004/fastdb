#ifndef FASTDB_BLOCK_CODEC_H
#define FASTDB_BLOCK_CODEC_H

#include "fastdb/fastdb.h"

bool fdb_block_codec_supported(uint32_t codec);
bool fdb_block_encode_workspace(uint32_t codec, size_t max_block, size_t *out);
bool fdb_block_decode_workspace(size_t max_block, size_t *out);
/* Zero means store the original block. The workspace is caller-owned. */
size_t fdb_block_lz4_encode(const unsigned char *raw, size_t size, void *workspace,
                             const unsigned char **out);
/* The encoded bytes end at the end of the decode workspace. */
FastDBStatus fdb_block_lz4_decode(const unsigned char *encoded, size_t stored,
                                  unsigned char *out, size_t max_block, size_t *decoded);

#endif
