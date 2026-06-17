/* dmr_const.c — DMR constant bit tables (const.py).
 * All arrays validated bit-for-bit against the Python dmr_utils3 tables. */
#include "dmr.h"

/* BS_VOICE_SYNC = 0x75 5F D7 DF 75 F7 */
const dmr_bit DMR_BS_VOICE_SYNC[48] = {
    0,1,1,1,0,1,0,1, 0,1,0,1,1,1,1,1, 1,1,0,1,0,1,1,1,
    1,1,0,1,1,1,1,1, 0,1,1,1,0,1,0,1, 1,1,1,1,0,1,1,1
};

/* BS_DATA_SYNC = 0xDF F5 7D 75 DF 5D */
const dmr_bit DMR_BS_DATA_SYNC[48] = {
    1,1,0,1,1,1,1,1, 1,1,1,1,0,1,0,1, 0,1,1,1,1,1,0,1,
    0,1,1,1,0,1,0,1, 1,1,0,1,1,1,1,1, 0,1,0,1,1,1,0,1
};

/* EMB headers (CC=1, PI=0). Index B,C,D,E,F. */
const dmr_bit DMR_EMB[5][16] = {
    {0,0,0,1,0,0,1,1,1,0,0,1,0,0,0,1},   /* BURST_B 0001001110010001 */
    {0,0,0,1,0,1,1,1,0,1,1,1,0,1,0,0},   /* BURST_C 0001011101110100 */
    {0,0,0,1,0,1,1,1,0,1,1,1,0,1,0,0},   /* BURST_D 0001011101110100 */
    {0,0,0,1,0,1,0,1,0,0,0,0,0,1,1,1},   /* BURST_E 0001010100000111 */
    {0,0,0,1,0,0,0,1,1,1,1,0,0,0,1,0},   /* BURST_F 0001000111100010 */
};

/* Slot type words (CC=1). */
const dmr_bit DMR_SLOT_TYPE_VHEAD[20] =
    {0,0,0,1,0,0,0,1,1,0,1,1,1,0,0,0,1,1,0,0}; /* 00010001101110001100 */
const dmr_bit DMR_SLOT_TYPE_VTERM[20] =
    {0,0,0,1,0,0,1,0,1,0,1,0,0,1,0,1,1,0,0,1}; /* 00010010101001011001 */

const uint8_t DMR_LC_OPT[3] = {0x00, 0x00, 0x00};
