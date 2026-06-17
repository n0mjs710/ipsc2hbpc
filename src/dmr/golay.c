/* golay.c — Golay(23,12) codeword + parity used by AMBE conversion
 * (ambe_utils.py golay2312 / parity). */
#include "dmr_internal.h"

uint32_t dmr_golay2312(uint32_t cw)
{
    const uint32_t POLY = 0xAE3;
    cw &= 0xFFF;             /* data bits only */
    uint32_t c = cw;         /* save original codeword */
    for (int i = 1; i <= 12; i++) {
        if (cw & 1)
            cw ^= POLY;
        cw >>= 1;
    }
    return (cw << 12) | c;
}

int dmr_parity(uint32_t cw)
{
    uint32_t p = cw & 0xFF;
    p ^= (cw >> 8) & 0xFF;
    p ^= (cw >> 16) & 0xFF;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    return (int)(p & 1);
}
