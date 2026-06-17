/* test_dsp.c — validate the C DMR DSP port against golden vectors generated
 * from the Python dmr_utils3 reference (tests/dsp_vectors.txt). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/dmr/dmr.h"
#include "../src/dmr/dmr_internal.h"

#define MAXV 512
static char  vkey[MAXV][48];
static uint8_t vbuf[MAXV][64];
static int   vlen[MAXV];
static int   nvec = 0;

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(2); }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *hex = eq + 1;
        strncpy(vkey[nvec], line, sizeof vkey[0] - 1);
        int n = 0;
        for (char *p = hex; p[0] && p[1] && hexval(p[0]) >= 0; p += 2) {
            vbuf[nvec][n++] = (uint8_t)((hexval(p[0]) << 4) | hexval(p[1]));
        }
        vlen[nvec] = n;
        nvec++;
    }
    fclose(f);
}

static const uint8_t *get(const char *key, int *len) {
    for (int i = 0; i < nvec; i++)
        if (strcmp(vkey[i], key) == 0) { if (len) *len = vlen[i]; return vbuf[i]; }
    fprintf(stderr, "MISSING vector: %s\n", key);
    exit(2);
}

static int fails = 0, checks = 0;
/* Look up the expected vector by key and compare against got. */
static void check(const char *what, const uint8_t *got, int gn) {
    checks++;
    int en;
    const uint8_t *exp = get(what, &en);
    if (gn != en || memcmp(got, exp, gn) != 0) {
        fails++;
        fprintf(stderr, "FAIL %s\n  got(%d): ", what, gn);
        for (int i = 0; i < gn; i++) fprintf(stderr, "%02x", got[i]);
        fprintf(stderr, "\n  exp(%d): ", en);
        for (int i = 0; i < en; i++) fprintf(stderr, "%02x", exp[i]);
        fprintf(stderr, "\n");
    }
}

int main(int argc, char **argv) {
    load(argc > 1 ? argv[1] : "tests/dsp_vectors.txt");
    char key[64];

    for (int i = 0; i < 4; i++) {
        int n;
        char base[16]; snprintf(base, sizeof base, "lc%d", i);
        snprintf(key, sizeof key, "%s.in", base);
        const uint8_t *lc = get(key, &n);
        uint8_t lc9[9]; memcpy(lc9, lc, 9);

        uint8_t out3[3];
        dmr_rs129_lc_encode(lc9, 0, out3);
        snprintf(key, sizeof key, "%s.rs_header", base);
        check(key, out3, 3);
        dmr_rs129_lc_encode(lc9, 1, out3);
        snprintf(key, sizeof key, "%s.rs_term", base);
        check(key, out3, 3);

        /* csum5: stored byte is the 5-bit value left-shifted by 3 (bitarray pad) */
        uint8_t cs = (uint8_t)(dmr_csum5(lc9) << 3);
        snprintf(key, sizeof key, "%s.csum5", base);
        check(key, &cs, 1);

        /* encode_19696 of lc + rs_header */
        uint8_t data12[12]; memcpy(data12, lc9, 9);
        dmr_rs129_lc_encode(lc9, 0, data12 + 9);
        dmr_bit b196[196]; uint8_t bytes25[25];
        dmr_bptc_encode_19696(data12, b196);
        dmr_bits_to_bytes(b196, 196, bytes25);
        snprintf(key, sizeof key, "%s.encode_19696", base);
        check(key, bytes25, 25);

        /* encode_header_lc / encode_terminator_lc */
        dmr_bptc_encode_lc(lc9, 0, b196);
        dmr_bits_to_bytes(b196, 196, bytes25);
        snprintf(key, sizeof key, "%s.encode_header_lc", base);
        check(key, bytes25, 25);

        dmr_bit hdr196[196];
        memcpy(hdr196, b196, 196);   /* keep header codeword for decode test */

        dmr_bptc_encode_lc(lc9, 1, b196);
        dmr_bits_to_bytes(b196, 196, bytes25);
        snprintf(key, sizeof key, "%s.encode_terminator_lc", base);
        check(key, bytes25, 25);

        /* decode_full_lc of the header codeword → original LC */
        uint8_t dec9[9];
        dmr_bptc_decode_full_lc(hdr196, dec9);
        snprintf(key, sizeof key, "%s.decode_full_lc", base);
        check(key, dec9, 9);

        /* embedded LC */
        uint8_t emb[4][4];
        dmr_encode_emblc(lc9, emb);
        for (int k = 1; k <= 4; k++) {
            snprintf(key, sizeof key, "%s.emblc%d", base, k);
            check(key, emb[k - 1], 4);
        }
    }

    /* Hamming */
    int v15[] = {0x000,0x7FF,0x555,0x2AA,0x123,0x400,0x001};
    int v9[]  = {0x000,0x1FF,0x155,0x0AA,0x123,0x100,0x001};
    for (size_t i = 0; i < sizeof v15/sizeof v15[0]; i++) {
        int val = v15[i], n;
        dmr_bit d[11]; for (int b = 0; b < 11; b++) d[b] = (val >> (10 - b)) & 1;
        dmr_bit c[5]; uint8_t ob[1];
        dmr_ham_15113(d, c); dmr_bits_to_bytes(c, 4, ob);
        snprintf(key, sizeof key, "ham15113.%03x", val); check(key, ob, 1);
        dmr_ham_16114(d, c); dmr_bits_to_bytes(c, 5, ob);
        snprintf(key, sizeof key, "ham16114.%03x", val); check(key, ob, 1);
    }
    for (size_t i = 0; i < sizeof v9/sizeof v9[0]; i++) {
        int val = v9[i], n;
        dmr_bit d[9]; for (int b = 0; b < 9; b++) d[b] = (val >> (8 - b)) & 1;
        dmr_bit c[4]; uint8_t ob[1];
        dmr_ham_1393(d, c); dmr_bits_to_bytes(c, 4, ob);
        snprintf(key, sizeof key, "ham1393.%03x", val); check(key, ob, 1);
    }

    /* AMBE round trips */
    for (int i = 0; i < 4; i++) {
        int n;
        snprintf(key, sizeof key, "ambe72to49.%d.in", i);
        const uint8_t *in = get(key, &n);
        dmr_bit in72[72]; dmr_bytes_to_bits(in, 9, in72);
        dmr_bit a49[49]; dmr_ambe_72_to_49(in72, a49);
        uint8_t out7[7]; dmr_bits_to_bytes(a49, 49, out7);
        snprintf(key, sizeof key, "ambe72to49.%d.out", i);
        check(key, out7, 7);
        dmr_bit a72[72]; dmr_ambe_49_to_72(a49, a72);
        uint8_t out9[9]; dmr_bits_to_bytes(a72, 72, out9);
        snprintf(key, sizeof key, "ambe49to72.%d.out", i);
        check(key, out9, 9);
    }

    /* const tables */
    { int n; uint8_t b[8];
      dmr_bits_to_bytes(DMR_BS_VOICE_SYNC, 48, b); check("BS_VOICE_SYNC", b, 6);
      dmr_bits_to_bytes(DMR_BS_DATA_SYNC, 48, b);  check("BS_DATA_SYNC",  b, 6);
      check("LC_OPT", DMR_LC_OPT, 3);
      const char *en[] = {"BURST_B","BURST_C","BURST_D","BURST_E","BURST_F"};
      for (int e = 0; e < 5; e++) { dmr_bits_to_bytes(DMR_EMB[e],16,b);
          snprintf(key,sizeof key,"EMB.%s",en[e]); check(key,b,2); }
      dmr_bits_to_bytes(DMR_SLOT_TYPE_VHEAD,20,b); check("SLOT_TYPE.VOICE_LC_HEAD",b,3);
      dmr_bits_to_bytes(DMR_SLOT_TYPE_VTERM,20,b); check("SLOT_TYPE.VOICE_LC_TERM",b,3);
    }

    printf("DSP self-test: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
