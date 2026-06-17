/* ambe.c — AMBE 49<->72 bit conversion (ambe_utils.py).
 *
 * ambe_fr is a 4x24 bit matrix (C0..C3).  Conversions go through Golay(23,12)
 * encode/parity on C0/C1 and a pseudo-random demodulator on C1, exactly as in
 * the Python reference. */
#include "dmr_internal.h"
#include <string.h>

/* DMR AMBE interleave schedule (ambe_utils.py rW/rX/rY/rZ). */
static const int rW[36] = {
    0,1,0,1,0,1, 0,1,0,1,0,1, 0,1,0,1,0,1,
    0,1,0,1,0,2, 0,2,0,2,0,2, 0,2,0,2,0,2
};
static const int rX[36] = {
    23,10,22,9,21,8, 20,7,19,6,18,5, 17,4,16,3,15,2,
    14,1,13,0,12,10, 11,9,10,8,9,7, 8,6,7,5,6,4
};
static const int rY[36] = {
    0,2,0,2,0,2, 0,2,0,3,0,3, 1,3,1,3,1,3,
    1,3,1,3,1,3, 1,3,1,3,1,3, 1,3,1,3,1,3
};
static const int rZ[36] = {
    5,3,4,2,3,1, 2,0,1,13,0,12, 22,11,21,10,20,9,
    19,8,18,7,17,6, 16,5,15,4,14,3, 13,2,12,1,11,0
};

/* Demodulate C1 with the pseudo-random sequence (modifies ambe_fr[1]). */
static void demodulate(int ambe_fr[4][24])
{
    long pr[115];
    long foo = 0;
    for (int i = 23; i >= 12; i--)
        foo = (foo << 1) | ambe_fr[0][i];
    pr[0] = 16 * foo;
    for (int i = 1; i < 24; i++) {
        long v = 173 * pr[i - 1] + 13849;
        pr[i] = v - 65536 * (v / 65536);      /* mod 65536 */
    }
    for (int i = 1; i < 24; i++)
        pr[i] = pr[i] / 32768;                /* 0 or 1 */
    int k = 1;
    for (int j = 22; j >= 0; j--)
        ambe_fr[1][j] = ambe_fr[1][j] ^ (int)pr[k++];
}

/* Build a 4x24 frame from 49 raw AMBE bits (convert49BitAmbeTo72BitFrames). */
static void frames_from_49(const dmr_bit ambe_d[49], int ambe_fr[4][24])
{
    memset(ambe_fr, 0, sizeof(int) * 4 * 24);
    uint32_t tmp;

    /* C0: 12 bits + Golay(23,12) + parity → 24 bits */
    tmp = 0;
    for (int i = 11; i >= 0; i--)
        tmp = (tmp << 1) | ambe_d[i];
    tmp = dmr_golay2312(tmp);
    tmp = tmp | ((uint32_t)dmr_parity(tmp) << 23);
    for (int i = 23; i >= 0; i--) { ambe_fr[0][i] = (int)(tmp & 1); tmp >>= 1; }

    /* C1: 12 bits + Golay(23,12), no parity → 23 bits */
    tmp = 0;
    for (int i = 23; i >= 12; i--)
        tmp = (tmp << 1) | ambe_d[i];
    tmp = dmr_golay2312(tmp);
    for (int j = 22; j >= 0; j--) { ambe_fr[1][j] = (int)(tmp & 1); tmp >>= 1; }

    /* C2: 11 bits raw */
    for (int j = 10; j >= 0; j--) ambe_fr[2][j] = ambe_d[34 - j];

    /* C3: 14 bits raw */
    for (int j = 13; j >= 0; j--) ambe_fr[3][j] = ambe_d[48 - j];
}

/* Extract 49 raw AMBE bits from a 4x24 frame (eccAmbe3600x2450Data). */
static void ecc_extract_49(const int ambe_fr[4][24], dmr_bit out49[49])
{
    int n = 0;
    for (int j = 23; j >= 12; j--) out49[n++] = (dmr_bit)ambe_fr[0][j];  /* 12 */
    for (int j = 22; j >= 11; j--) out49[n++] = (dmr_bit)ambe_fr[1][j];  /* 12 */
    for (int j = 10; j >= 0;  j--) out49[n++] = (dmr_bit)ambe_fr[2][j];  /* 11 */
    for (int j = 13; j >= 0;  j--) out49[n++] = (dmr_bit)ambe_fr[3][j];  /* 14 */
}

void dmr_ambe_49_to_72(const dmr_bit in49[49], dmr_bit out72[72])
{
    int ambe_fr[4][24];
    frames_from_49(in49, ambe_fr);
    demodulate(ambe_fr);
    /* interleave → 72 bits, MSB-first packing order matches Python frombytes */
    int bitIndex = 0;
    for (int i = 0; i < 36; i++) {
        out72[bitIndex++] = (dmr_bit)(ambe_fr[rW[i]][rX[i]] ? 1 : 0);
        out72[bitIndex++] = (dmr_bit)(ambe_fr[rY[i]][rZ[i]] ? 1 : 0);
    }
}

void dmr_ambe_72_to_49(const dmr_bit in72[72], dmr_bit out49[49])
{
    int ambe_fr[4][24];
    memset(ambe_fr, 0, sizeof(ambe_fr));
    int bitIndex = 0;
    for (int i = 0; i < 36; i++) {
        int bit1 = in72[bitIndex++] ? 1 : 0;
        int bit0 = in72[bitIndex++] ? 1 : 0;
        ambe_fr[rW[i]][rX[i]] = bit1;
        ambe_fr[rY[i]][rZ[i]] = bit0;
    }
    demodulate(ambe_fr);
    ecc_extract_49(ambe_fr, out49);
}
