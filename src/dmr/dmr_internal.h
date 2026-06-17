/*
 * dmr_internal.h — private declarations shared between the dmr source files.
 * Not part of the public API.
 */
#ifndef DMR_INTERNAL_H
#define DMR_INTERNAL_H

#include "dmr.h"

/* Hamming encoders (hamming.py).  _data points to bit arrays; csum filled. */
void dmr_ham_15113(const dmr_bit data[11], dmr_bit csum[4]);
void dmr_ham_1393(const dmr_bit data[9], dmr_bit csum[4]);
void dmr_ham_16114(const dmr_bit data[11], dmr_bit csum[5]);

/* RS(12,9) raw encode (no mask): returns parity bytes [p2,p1,p0]. */
void dmr_rs129_encode(const uint8_t msg[9], uint8_t parity_out[3]);

/* Golay(23,12) used by AMBE conversion (ambe_utils.py golay2312/parity). */
uint32_t dmr_golay2312(uint32_t cw);
int dmr_parity(uint32_t cw);

#endif /* DMR_INTERNAL_H */
