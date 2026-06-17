/*
 * dmr.h — DMR DSP/FEC primitives (BPTC, embedded LC, AMBE, RS(12,9), Hamming).
 *
 * Self-contained: this module depends only on the C standard library and its
 * own internal headers.  It has NO dependency on the rest of ipsc2hbpc and is
 * intended to be liftable into a standalone libdmrdsp repository unchanged.
 *
 * Ported feature-for-feature from dmr_utils3 (N0MJS / G4KLX):
 *   bptc.py, ambe_utils.py, hamming.py, crc.py, rs129.py, const.py
 *
 * Bit representation
 * ------------------
 * A "bit array" is an array of dmr_bit (one bit per byte, value 0 or 1), most
 * significant bit first — identical in ordering to Python bitarray(endian='big').
 * Use dmr_bytes_to_bits / dmr_bits_to_bytes to convert to/from packed bytes.
 *
 *   Copyright (C) 2016-2026 Cortney T. Buffington, N0MJS <n0mjs@me.com>
 *   Copyright (C) 2015 Jonathan Naylor G4KLX; (C) 2017 Mike Zingman N4IRR
 *   GNU GPLv3.
 */
#ifndef DMR_H
#define DMR_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t dmr_bit;

/* ------------------------------------------------------------------ */
/* Bit / byte conversion (big-endian bit order, matches Python bitarray) */
/* ------------------------------------------------------------------ */

/* Expand nbytes packed bytes into nbytes*8 bits, MSB first. */
void dmr_bytes_to_bits(const uint8_t *bytes, size_t nbytes, dmr_bit *bits_out);

/* Pack nbits bits (MSB first) into ceil(nbits/8) bytes; tail zero-padded. */
void dmr_bits_to_bytes(const dmr_bit *bits, size_t nbits, uint8_t *bytes_out);

/* ------------------------------------------------------------------ */
/* Reed-Solomon (12,9) — rs129.py                                      */
/* ------------------------------------------------------------------ */

/* Compute the 3 RS(12,9) parity bytes for a 9-byte LC and apply the DMR
 * header (is_term=0) or terminator (is_term=1) XOR mask. */
void dmr_rs129_lc_encode(const uint8_t lc[9], int is_term, uint8_t out3[3]);

/* ------------------------------------------------------------------ */
/* 5-bit checksum — crc.py csum5                                       */
/* ------------------------------------------------------------------ */

/* Returns the 5-bit Embedded-LC checksum as the 5 low bits of the result. */
uint8_t dmr_csum5(const uint8_t lc[9]);

/* ------------------------------------------------------------------ */
/* BPTC(196,96) — bptc.py                                              */
/* ------------------------------------------------------------------ */

/* Encode a 9-byte LC into the 196-bit interleaved BPTC codeword for a voice
 * LC header (is_term=0) or terminator (is_term=1).  Output is 196 bits. */
void dmr_bptc_encode_lc(const uint8_t lc[9], int is_term, dmr_bit out196[196]);

/* encode_19696 of (lc + rs_parity): 196-bit non-interleaved matrix.  Exposed
 * mainly for validation; the application uses dmr_bptc_encode_lc. */
void dmr_bptc_encode_19696(const uint8_t data12[12], dmr_bit out196[196]);

/* Decode the 72-bit (9-byte) LC out of a 196-bit BPTC codeword (decode_full_lc).
 * No FEC correction is performed — the bits are picked directly, matching
 * dmr_utils3.bptc.decode_full_lc. */
void dmr_bptc_decode_full_lc(const dmr_bit in196[196], uint8_t lc_out[9]);

/* ------------------------------------------------------------------ */
/* Embedded LC — bptc.py encode_emblc                                  */
/* ------------------------------------------------------------------ */

/* Encode a 9-byte LC into the four 32-bit embedded-LC fragments for bursts
 * B,C,D,E.  out[0]=burst B .. out[3]=burst E, each 4 bytes (32 bits). */
void dmr_encode_emblc(const uint8_t lc[9], uint8_t out[4][4]);

/* ------------------------------------------------------------------ */
/* AMBE 49<->72 bit conversion — ambe_utils.py                         */
/* ------------------------------------------------------------------ */

void dmr_ambe_49_to_72(const dmr_bit in49[49], dmr_bit out72[72]);
void dmr_ambe_72_to_49(const dmr_bit in72[72], dmr_bit out49[49]);

/* ------------------------------------------------------------------ */
/* Constant bit tables — const.py                                      */
/* ------------------------------------------------------------------ */

extern const dmr_bit DMR_BS_VOICE_SYNC[48];   /* b'\x75\x5F\xD7\xDF\x75\xF7' */
extern const dmr_bit DMR_BS_DATA_SYNC[48];    /* b'\xDF\xF5\x7D\x75\xDF\x5D' */

/* Embedded EMB headers, index 0..4 = BURST_B..BURST_F, 16 bits each. */
extern const dmr_bit DMR_EMB[5][16];
enum { DMR_EMB_B = 0, DMR_EMB_C, DMR_EMB_D, DMR_EMB_E, DMR_EMB_F };

extern const dmr_bit DMR_SLOT_TYPE_VHEAD[20]; /* VOICE_LC_HEAD slot type */
extern const dmr_bit DMR_SLOT_TYPE_VTERM[20]; /* VOICE_LC_TERM slot type */

extern const uint8_t DMR_LC_OPT[3];           /* group: 00 00 00 */

#endif /* DMR_H */
