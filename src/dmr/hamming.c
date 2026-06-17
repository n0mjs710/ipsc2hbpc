/* hamming.c — Hamming encoders (hamming.py). */
#include "dmr_internal.h"

void dmr_ham_15113(const dmr_bit d[11], dmr_bit c[4])
{
    c[0] = d[0]^d[1]^d[2]^d[3]^d[5]^d[7]^d[8];
    c[1] = d[1]^d[2]^d[3]^d[4]^d[6]^d[8]^d[9];
    c[2] = d[2]^d[3]^d[4]^d[5]^d[7]^d[9]^d[10];
    c[3] = d[0]^d[1]^d[2]^d[4]^d[6]^d[7]^d[10];
}

void dmr_ham_1393(const dmr_bit d[9], dmr_bit c[4])
{
    c[0] = d[0]^d[1]^d[3]^d[5]^d[6];
    c[1] = d[0]^d[1]^d[2]^d[4]^d[6]^d[7];
    c[2] = d[0]^d[1]^d[2]^d[3]^d[5]^d[7]^d[8];
    c[3] = d[0]^d[2]^d[4]^d[5]^d[8];
}

void dmr_ham_16114(const dmr_bit d[11], dmr_bit c[5])
{
    c[0] = d[0]^d[1]^d[2]^d[3]^d[5]^d[7]^d[8];
    c[1] = d[1]^d[2]^d[3]^d[4]^d[6]^d[8]^d[9];
    c[2] = d[2]^d[3]^d[4]^d[5]^d[7]^d[9]^d[10];
    c[3] = d[0]^d[1]^d[2]^d[4]^d[6]^d[7]^d[10];
    c[4] = d[0]^d[2]^d[5]^d[6]^d[8]^d[9]^d[10];
}
