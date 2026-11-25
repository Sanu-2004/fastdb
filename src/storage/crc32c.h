#ifndef FASTDB_CRC32C_H
#define FASTDB_CRC32C_H

#include <stddef.h>
#include <stdint.h>

uint32_t fdb_crc32c(const void *bytes, size_t length);

#endif
