#include "crc32c.h"

uint32_t fdb_crc32c(const void *bytes, size_t length)
{
    /* Reflected Castagnoli polynomial, four input bits per step. */
    static const uint32_t lookup[16] = {
        UINT32_C(0x00000000), UINT32_C(0x105ec76f), UINT32_C(0x20bd8ede), UINT32_C(0x30e349b1),
        UINT32_C(0x417b1dbc), UINT32_C(0x5125dad3), UINT32_C(0x61c69362), UINT32_C(0x7198540d),
        UINT32_C(0x82f63b78), UINT32_C(0x92a8fc17), UINT32_C(0xa24bb5a6), UINT32_C(0xb21572c9),
        UINT32_C(0xc38d26c4), UINT32_C(0xd3d3e1ab), UINT32_C(0xe330a81a), UINT32_C(0xf36e6f75),
    };
    const unsigned char *source = bytes;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= source[i];
        crc = (crc >> 4) ^ lookup[crc & 15U];
        crc = (crc >> 4) ^ lookup[crc & 15U];
    }
    return ~crc;
}
