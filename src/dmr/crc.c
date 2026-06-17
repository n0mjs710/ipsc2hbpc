/* crc.c — 5-bit Embedded-LC checksum (crc.py csum5).
 *
 * Python: accum = sum(bytes) % 31; take that one byte as 8 bits big-endian,
 * drop the top 3 bits, leaving the low 5 bits.  We return those 5 bits in the
 * low 5 bits of a uint8_t. */
#include "dmr.h"

uint8_t dmr_csum5(const uint8_t lc[9])
{
    unsigned accum = 0;
    for (int i = 0; i < 9; i++)
        accum += lc[i];
    return (uint8_t)(accum % 31) & 0x1F;
}
