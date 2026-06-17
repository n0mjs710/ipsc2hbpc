/* dmr_bits.c — bit/byte conversion (big-endian bit order). */
#include "dmr.h"

void dmr_bytes_to_bits(const uint8_t *bytes, size_t nbytes, dmr_bit *bits_out)
{
    for (size_t i = 0; i < nbytes; i++)
        for (int b = 0; b < 8; b++)
            bits_out[i * 8 + b] = (bytes[i] >> (7 - b)) & 1;
}

void dmr_bits_to_bytes(const dmr_bit *bits, size_t nbits, uint8_t *bytes_out)
{
    size_t nbytes = (nbits + 7) / 8;
    for (size_t i = 0; i < nbytes; i++)
        bytes_out[i] = 0;
    for (size_t i = 0; i < nbits; i++)
        bytes_out[i / 8] |= (uint8_t)((bits[i] & 1) << (7 - (i % 8)));
}
