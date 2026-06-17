/* bptc.c — BPTC(196,96) and Embedded LC encode/decode (bptc.py).
 *
 * The Python reference builds the matrix with bitarray.insert() (positional
 * shifts).  We replicate that exactly with a small growable bit buffer so the
 * output is bit-identical, including the few index quirks present in the
 * reference embedded-LC layout. */
#include "dmr_internal.h"
#include <string.h>

/* Interleaver index (bptc.py INDEX_181). */
static const int INDEX_181[196] = {
    0,181,166,151,136,121,106,91,76,61,46,31,16,1,182,167,152,137,
    122,107,92,77,62,47,32,17,2,183,168,153,138,123,108,93,78,63,
    48,33,18,3,184,169,154,139,124,109,94,79,64,49,34,19,4,185,170,
    155,140,125,110,95,80,65,50,35,20,5,186,171,156,141,126,111,96,
    81,66,51,36,21,6,187,172,157,142,127,112,97,82,67,52,37,22,7,
    188,173,158,143,128,113,98,83,68,53,38,23,8,189,174,159,144,129,
    114,99,84,69,54,39,24,9,190,175,160,145,130,115,100,85,70,55,40,
    25,10,191,176,161,146,131,116,101,86,71,56,41,26,11,192,177,162,
    147,132,117,102,87,72,57,42,27,12,193,178,163,148,133,118,103,88,
    73,58,43,28,13,194,179,164,149,134,119,104,89,74,59,44,29,14,
    195,180,165,150,135,120,105,90,75,60,45,30,15
};

/* decode_full_lc bit-pick order (bptc.py decode_full_lc), 72 entries. */
static const int DECODE_LC_IDX[72] = {
    136,121,106,91,76,61,46,31,
    152,137,122,107,92,77,62,47,32,17,2,
    123,108,93,78,63,48,33,18,3,184,169,
    94,79,64,49,34,19,4,185,170,155,140,
    65,50,35,20,5,186,171,156,141,126,111,
    36,21,6,187,172,157,142,127,112,97,82,
    7,188,173,158,143,128,113,98,83
};

/* insert val at pos, shifting [pos..len) right by one. */
static void bit_insert(dmr_bit *buf, int *len, int pos, dmr_bit val)
{
    for (int i = *len; i > pos; i--)
        buf[i] = buf[i - 1];
    buf[pos] = val;
    (*len)++;
}

void dmr_bptc_encode_19696(const uint8_t data12[12], dmr_bit out196[196])
{
    dmr_bit buf[210];
    int len = 0;
    dmr_bytes_to_bits(data12, 12, buf);   /* 96 bits */
    len = 96;

    /* insert R0-R3 (4 zero bits) at front */
    for (int i = 0; i < 4; i++)
        bit_insert(buf, &len, 0, 0);

    /* row Hamming(15,11,3) */
    for (int index = 0; index < 9; index++) {
        int spos = index * 15 + 1;
        int epos = spos + 11;
        dmr_bit row[11], csum[4];
        for (int k = 0; k < 11; k++) row[k] = buf[spos + k];
        dmr_ham_15113(row, csum);
        for (int p = 0; p < 4; p++)
            bit_insert(buf, &len, epos + p, csum[p]);
    }

    /* pad to 196 */
    while (len < 196) buf[len++] = 0;

    /* column Hamming(13,9,3) written in place */
    for (int col = 0; col < 15; col++) {
        dmr_bit column[9], csum[4];
        int spos = col + 1;
        for (int index = 0; index < 9; index++) { column[index] = buf[spos]; spos += 15; }
        dmr_ham_1393(column, csum);
        int cpar = 136 + col;
        for (int p = 0; p < 4; p++) { buf[cpar] = csum[p]; cpar += 15; }
    }

    memcpy(out196, buf, 196);
}

static void interleave_19696(const dmr_bit in196[196], dmr_bit out196[196])
{
    for (int index = 0; index < 196; index++)
        out196[INDEX_181[index]] = in196[index];
}

void dmr_bptc_encode_lc(const uint8_t lc[9], int is_term, dmr_bit out196[196])
{
    uint8_t data12[12];
    memcpy(data12, lc, 9);
    dmr_rs129_lc_encode(lc, is_term, data12 + 9);
    dmr_bit tmp[196];
    dmr_bptc_encode_19696(data12, tmp);
    interleave_19696(tmp, out196);
}

void dmr_bptc_decode_full_lc(const dmr_bit in196[196], uint8_t lc_out[9])
{
    dmr_bit bits[72];
    for (int i = 0; i < 72; i++)
        bits[i] = in196[DECODE_LC_IDX[i]];
    dmr_bits_to_bytes(bits, 72, lc_out);
}

/* Embedded LC segment bit-pick indices (bptc.py encode_emblc).
 * Note: emblc_d row 1 reads index 24 (matches the reference exactly). */
static const int EMBLC_IDX[4][32] = {
    { 0,16,32,48,64,80,96,112,  1,17,33,49,65,81,97,113,
      2,18,34,50,66,82,98,114,  3,19,35,51,67,83,99,115 },
    { 4,20,36,52,68,84,100,116, 5,21,37,53,69,85,101,117,
      6,22,38,54,70,86,102,118, 7,23,39,55,71,87,103,119 },
    { 8,24,40,56,72,88,104,120, 9,24,41,57,73,89,105,121,
      10,26,42,58,74,90,106,122, 11,27,43,59,75,91,107,123 },
    { 12,28,44,60,76,92,108,124, 13,29,45,61,77,93,109,125,
      14,30,46,62,78,94,110,126, 15,31,47,63,79,95,111,127 },
};

void dmr_encode_emblc(const uint8_t lc[9], uint8_t out[4][4])
{
    dmr_bit buf[160];
    int len = 0;
    dmr_bytes_to_bits(lc, 9, buf);   /* 72 bits */
    len = 72;

    uint8_t cs = dmr_csum5(lc);
    dmr_bit csum[5];
    for (int k = 0; k < 5; k++) csum[k] = (cs >> (4 - k)) & 1;
    bit_insert(buf, &len, 32, csum[0]);
    bit_insert(buf, &len, 43, csum[1]);
    bit_insert(buf, &len, 54, csum[2]);
    bit_insert(buf, &len, 65, csum[3]);
    bit_insert(buf, &len, 76, csum[4]);

    /* row Hamming(16,11,4) for blocks at 0,16,...,96 */
    for (int index = 0; index < 112; index += 16) {
        dmr_bit d[11], h[5];
        for (int k = 0; k < 11; k++) d[k] = buf[index + k];
        dmr_ham_16114(d, h);
        for (int p = 0; p < 5; p++)
            bit_insert(buf, &len, index + 11 + p, h[p]);
    }

    /* column parity inserted at 112..127 */
    for (int index = 0; index < 16; index++) {
        dmr_bit v = buf[index] ^ buf[index + 16] ^ buf[index + 32] ^ buf[index + 48]
                  ^ buf[index + 64] ^ buf[index + 80] ^ buf[index + 96];
        bit_insert(buf, &len, index + 112, v);
    }

    /* build the four 32-bit segments */
    for (int seg = 0; seg < 4; seg++) {
        dmr_bit seg_bits[32];
        for (int i = 0; i < 32; i++)
            seg_bits[i] = buf[EMBLC_IDX[seg][i]];
        dmr_bits_to_bytes(seg_bits, 32, out[seg]);
    }
}
