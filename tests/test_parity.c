/* test_parity.c — feed identical IPSC frames through the C translator and
 * compare the synthesized DMRD against the Python reference (stream-id masked).
 *
 * Stubs the ipsc and hbp layers so only translate.c + the dmr module run. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/translate.h"
#include "../src/config.h"
#include "../src/eventloop.h"
#include "../src/dmr/dmr.h"
#include "../src/hbp_const.h"
#include "../src/ipsc_const.h"

/* ---- captured DMRD output ---- */
#define MAXCAP 64
static uint8_t cap[MAXCAP][128];
static int     caplen[MAXCAP];
static int     ncap = 0;

/* ---- stubbed protocol layer ---- */
static uint8_t gvcap[MAXCAP][128];
static int     gvlen[MAXCAP];
static int     ngv = 0;

int  ipsc_has_peers(struct ipsc *ip) { (void)ip; return 1; }
void ipsc_send_voice(struct ipsc *ip, const uint8_t *p, int n) {
    (void)ip;
    if (ngv < MAXCAP) { memcpy(gvcap[ngv], p, (size_t)n); gvlen[ngv] = n; ngv++; }
}
int  hbp_is_connected(struct hbp *hb) { (void)hb; return 1; }
void hbp_activate(struct hbp *hb) { (void)hb; }
void hbp_deactivate(struct hbp *hb) { (void)hb; }
void hbp_send_dmrd(struct hbp *hb, const uint8_t *d, int n) {
    (void)hb;
    if (ncap < MAXCAP) { memcpy(cap[ncap], d, (size_t)n); caplen[ncap] = n; ncap++; }
}

static void stop_cb(ev_loop *loop, void *ud){ (void)ud; ev_stop(loop); }

static int hexv(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; return -1; }
static int unhex(const char *s, uint8_t *out){ int n=0; for(;s[0]&&s[1]&&hexv(s[0])>=0;s+=2) out[n++]=(uint8_t)((hexv(s[0])<<4)|hexv(s[1])); return n; }

int main(void)
{
    Config cfg; char err[4096];
    if (config_load("tests/parity.toml", &cfg, err, sizeof err)) {
        fprintf(stderr, "config: %s\n", err); return 2;
    }
    ev_loop *loop = ev_new();
    translator *tr = translator_new(&cfg, loop);
    translator_set_protocols(tr, (struct ipsc *)1, (struct hbp *)1);

    FILE *fi = fopen("tests/parity_in.txt", "r");
    if (!fi) { perror("tests/parity_in.txt"); return 2; }
    char line[512];
    while (fgets(line, sizeof line, fi)) {
        int ts, bt; char hex[400];
        if (sscanf(line, "%d %d %399s", &ts, &bt, hex) != 3) continue;
        uint8_t raw[400]; int n = unhex(hex, raw);
        translator_ipsc_voice_received(tr, raw, n, ts, bt);
    }
    fclose(fi);

    /* compare against reference (mask stream id 16:20) */
    FILE *fr = fopen("tests/parity_ref.txt", "r");
    if (!fr) { perror("tests/parity_ref.txt"); return 2; }
    int idx = 0, fails = 0;
    while (fgets(line, sizeof line, fr)) {
        uint8_t ref[128]; int rn = unhex(line, ref);
        if (idx >= ncap) { fprintf(stderr, "FAIL: ref[%d] but C produced only %d DMRD\n", idx, ncap); fails++; break; }
        uint8_t got[128]; memcpy(got, cap[idx], (size_t)caplen[idx]);
        if (caplen[idx] >= 20) memset(got + 16, 0, 4);   /* mask stream id */
        if (rn != caplen[idx] || memcmp(got, ref, (size_t)rn) != 0) {
            fails++;
            fprintf(stderr, "FAIL DMRD[%d]\n  got(%d): ", idx, caplen[idx]);
            for (int i=0;i<caplen[idx];i++) fprintf(stderr,"%02x",got[i]);
            fprintf(stderr, "\n  ref(%d): ", rn);
            for (int i=0;i<rn;i++) fprintf(stderr,"%02x",ref[i]);
            fprintf(stderr, "\n");
        }
        idx++;
    }
    fclose(fr);
    if (idx != ncap) { fprintf(stderr, "FAIL: C produced %d DMRD, ref had %d\n", ncap, idx); fails++; }

    printf("Parity (IPSC->HBP): %d DMRD compared, %d failures\n", idx, fails);

    /* ---- inbound parity (HBP -> IPSC): HEAD + TERM, compared verbatim ---- */
    fi = fopen("tests/parity_in_dmrd.txt", "r");
    fr = fopen("tests/parity_ref_gv.txt", "r");
    int in_fails = 0;
    if (fi && fr) {
        while (fgets(line, sizeof line, fi)) {
            uint8_t raw[128]; int n = unhex(line, raw);
            translator_hbp_voice_received(tr, raw, n);
        }
        /* HEAD/voice/TERM are now clocked out via the delivery timer; run the
         * loop briefly so those timers fire and emit the GROUP_VOICE frames. */
        ev_timer_after(loop, 0.4, stop_cb, loop);
        ev_run(loop);
        int gidx = 0;
        while (fgets(line, sizeof line, fr)) {
            uint8_t ref[128]; int rn = unhex(line, ref);
            if (gidx >= ngv) { fprintf(stderr, "FAIL: ref GV[%d] but C produced only %d\n", gidx, ngv); in_fails++; break; }
            if (rn != gvlen[gidx] || memcmp(gvcap[gidx], ref, (size_t)rn) != 0) {
                in_fails++;
                fprintf(stderr, "FAIL GV[%d]\n  got(%d): ", gidx, gvlen[gidx]);
                for (int i=0;i<gvlen[gidx];i++) fprintf(stderr,"%02x",gvcap[gidx][i]);
                fprintf(stderr, "\n  ref(%d): ", rn);
                for (int i=0;i<rn;i++) fprintf(stderr,"%02x",ref[i]);
                fprintf(stderr, "\n");
            }
            gidx++;
        }
        printf("Parity (HBP->IPSC): %d GROUP_VOICE compared, %d failures\n", gidx, in_fails);
    }
    if (fi) fclose(fi);
    if (fr) fclose(fr);
    translator_free(tr);

    /* ---- bad-voice flags (IPSC->HBP): flagged AMBE frames become MMDVMHost silence ---- */
    static const uint8_t SIL[9] = {0xB9,0xE8,0x81,0x52,0x61,0x73,0x00,0x2A,0x6B};
    static const int want[2][3] = {{1,1,1}, {0,1,0}};   /* per voice frame: flagged? */
    int bad_fails = 0;
    tr = translator_new(&cfg, loop);
    translator_set_protocols(tr, (struct ipsc *)1, (struct hbp *)1);
    ncap = 0;
    fi = fopen("tests/parity_in.txt", "r");
    for (int k = 0; k < 3 && fi && fgets(line, sizeof line, fi); k++) {
        int ts, bt; char hex[400];
        if (sscanf(line, "%d %d %399s", &ts, &bt, hex) != 3) continue;
        uint8_t raw[400]; int n = unhex(hex, raw);
        if (k >= 1) {
            const int *w = want[k - 1];
            if (w[0]) raw[32] |= 0x01;
            if (w[1]) raw[33 + 49 / 8] |= (uint8_t)(0x80 >> (49 % 8));
            if (w[2]) raw[33 + 99 / 8] |= (uint8_t)(0x80 >> (99 % 8));
        }
        translator_ipsc_voice_received(tr, raw, n, ts, bt);
    }
    if (fi) fclose(fi);
    if (ncap != 3) { fprintf(stderr, "FAIL bad-voice: expected 3 DMRD, got %d\n", ncap); bad_fails++; }
    for (int v = 0; v < 2 && v + 1 < ncap; v++) {
        dmr_bit b[264]; dmr_bytes_to_bits(cap[v + 1] + 20, 33, b);
        dmr_bit f[3][72];
        memcpy(f[0], b, 72);
        memcpy(f[1], b + 72, 36); memcpy(f[1] + 36, b + 156, 36);
        memcpy(f[2], b + 192, 72);
        dmr_bit s[72]; dmr_bytes_to_bits(SIL, 9, s);
        for (int i = 0; i < 3; i++) {
            int is_sil = memcmp(f[i], s, sizeof s) == 0;
            if (is_sil != want[v][i]) {
                fprintf(stderr, "FAIL bad-voice: voice %d frame %d silence=%d, want %d\n", v, i + 1, is_sil, want[v][i]);
                bad_fails++;
            }
        }
    }
    printf("Bad-voice flags (IPSC->HBP): %d failures\n", bad_fails);
    translator_free(tr);

    /* ---- superframe re-sync (IPSC->HBP): weak-signal call where the repeater lost a
     * burst (0x13 placeholder, dropped in ipsc.c) and restarted at burst A ---- */
    int sync_fails = 0, sync_seen = 0;
    tr = translator_new(&cfg, loop);
    translator_set_protocols(tr, (struct ipsc *)1, (struct hbp *)1);
    ncap = 0;
    int want_sync[MAXCAP];
    int nin = 0;
    fi = fopen("tests/synclost_ipsc_in.txt", "r");
    while (fi && fgets(line, sizeof line, fi)) {
        int ts, bt; char hex[400];
        if (sscanf(line, "%d %d %399s", &ts, &bt, hex) != 3) continue;
        if ((bt & 0x7F) == 0x13) continue;
        uint8_t raw[400]; int n = unhex(hex, raw);
        if (nin < MAXCAP) want_sync[nin++] = (bt != 1 && bt != 2) ? ((raw[32] & 0x40) ? 1 : 0) : -1;
        translator_ipsc_voice_received(tr, raw, n, ts, bt);
    }
    if (fi) fclose(fi);
    if (ncap != nin) { fprintf(stderr, "FAIL re-sync: %d IPSC packets -> %d DMRD\n", nin, ncap); sync_fails++; }
    int expect_pos = -1;
    for (int i = 0; i < ncap && i < nin; i++) {
        if (want_sync[i] < 0) continue;
        int fl = cap[i][15];
        int is_sync = (fl & 0x30) == 0x10;
        if (want_sync[i]) {
            sync_seen++;
            if (!is_sync) { fprintf(stderr, "FAIL re-sync: DMRD[%d] burst A sent as flags 0x%02x\n", i, fl); sync_fails++; }
            expect_pos = 1;
        } else if (expect_pos > 0) {
            if (is_sync || (fl & 0x0F) != expect_pos) {
                fprintf(stderr, "FAIL re-sync: DMRD[%d] flags 0x%02x, want voice burst %d\n", i, fl, expect_pos);
                sync_fails++;
            }
            expect_pos++;
        }
    }
    printf("Superframe re-sync (IPSC->HBP): %d burst A checked, %d failures\n", sync_seen, sync_fails);
    translator_free(tr);

    /* ---- late entry on weak signal (IPSC->HBP): weak-signal call 3120101 -> TG 2.
     * Joining at packet 4, the first burst E (packet 8, ctrl 0x96) has a corrupt LC and
     * must be skipped; anchor on packet 14 (0x16).  Joining at packet 33, the first
     * burst E (packet 38, ctrl 0x17: voice frame bad, LC good) is usable.  The third case
     * gives packet 8's corrupt LC a group FLCO, so only the LC-error bit can reject it. ---- */
    static const int le_join[3] = {4, 33, 4}, le_anchor[3] = {14, 38, 14};
    int le_fails = 0;
    for (int c = 0; c < 3; c++) {
        tr = translator_new(&cfg, loop);
        translator_set_protocols(tr, (struct ipsc *)1, (struct hbp *)1);
        ncap = 0;
        int idx = -1, anchor = -1;
        fi = fopen("tests/lateentry_ipsc_in.txt", "r");
        while (fi && fgets(line, sizeof line, fi)) {
            int ts, bt; char hex[400];
            if (sscanf(line, "%d %d %399s", &ts, &bt, hex) != 3) continue;
            idx++;
            if (idx < le_join[c] || bt == 1 || (bt & 0x7F) == 0x13) continue;
            uint8_t raw[400]; int n = unhex(hex, raw);
            if (c == 2 && idx == 8) raw[GV_BE_LC_FLCO_OFF] = 0x00;
            int before = ncap;
            translator_ipsc_voice_received(tr, raw, n, ts, bt);
            if (anchor < 0 && ncap > before) anchor = idx;
        }
        if (fi) fclose(fi);
        if (anchor != le_anchor[c]) {
            fprintf(stderr, "FAIL late entry: joined at %d, anchored at packet %d, want %d\n", le_join[c], anchor, le_anchor[c]);
            le_fails++;
        }
        for (int i = 0; i < ncap; i++) {
            unsigned src = (unsigned)(cap[i][5]<<16 | cap[i][6]<<8 | cap[i][7]);
            unsigned dst = (unsigned)(cap[i][8]<<16 | cap[i][9]<<8 | cap[i][10]);
            if (src != 3120101 || dst != 2) {
                fprintf(stderr, "FAIL late entry: joined at %d, DMRD[%d] %u->%u, want 3120101->2\n", le_join[c], i, src, dst);
                le_fails++; break;
            }
        }
        translator_free(tr);
    }
    printf("Late entry on weak signal (IPSC->HBP): %d failures\n", le_fails);

    /* ---- RSSI (IPSC->HBP): DMRD byte 54 from captured VOICE_HEAD / VOICE_TERM, 0 on
     * voice bursts.  pvt_ipsc_in.txt raw 40a7 40b8 40bd 40c2 (heads), 3f4e (term);
     * lateentry_ipsc_in.txt raw 705d 6fff 70a2 70f0 (heads), no term. ---- */
    static const char *rs_file[2] = {"tests/pvt_ipsc_in.txt", "tests/lateentry_ipsc_in.txt"};
    static const int rs_new[2][5] = {{65, 65, 65, 65, 63}, {112, 112, 113, 113, -1}};
    int rs_fails = 0;
    for (int legacy = 0; legacy < 2; legacy++) {
        /* A config still carrying the retired rssi_encoding_new key must load and decode the same. */
        Config c2;
        if (config_load(legacy ? "tests/rssi_legacy_key.toml" : "tests/parity.toml", &c2, err, sizeof err)) {
            fprintf(stderr, "FAIL rssi: %s config rejected: %s\n", legacy ? "legacy-key" : "plain", err);
            rs_fails++; continue;
        }
        for (int fidx = 0; fidx < 2; fidx++) {
            tr = translator_new(&c2, loop);
            translator_set_protocols(tr, (struct ipsc *)1, (struct hbp *)1);
            ncap = 0;
            fi = fopen(rs_file[fidx], "r");
            while (fi && fgets(line, sizeof line, fi)) {
                int ts, bt; char hex[400];
                if (sscanf(line, "%d %d %399s", &ts, &bt, hex) != 3) continue;
                if ((bt & 0x7F) == 0x13) continue;
                uint8_t raw[400]; int n = unhex(hex, raw);
                translator_ipsc_voice_received(tr, raw, n, ts, bt);
            }
            if (fi) fclose(fi);
            int k = 0;
            for (int i = 0; i < ncap; i++) {
                int fl = cap[i][15], got = cap[i][54];
                int want = 0;
                if ((fl & 0x30) == 0x20) want = rs_new[fidx][k++];
                if (got != want) {
                    fprintf(stderr, "FAIL rssi: %s %s DMRD[%d] flags 0x%02x byte54=%d, want %d\n",
                            rs_file[fidx], legacy ? "legacy-key" : "plain", i, fl, got, want);
                    rs_fails++;
                }
            }
            translator_free(tr);
        }
    }
    printf("RSSI (IPSC->HBP): %d failures\n", rs_fails);

    /* ---- HBP->IPSC: 53/55-byte DMRD, RSSI packing, slot bit.  parity_in_dmrd.txt is one
     * TS1 VOICE_HEAD + VOICE_TERM (55 bytes, byte 54 = 0).  Each emitted HEAD/TERM is fed
     * back through the IPSC->HBP decoder, which is verified against repeater captures. ---- */
    int hr_fails = 0;
    uint8_t dm[2][128]; int dmn[2] = {0, 0};
    fi = fopen("tests/parity_in_dmrd.txt", "r");
    for (int k = 0; k < 2 && fi && fgets(line, sizeof line, fi); k++) dmn[k] = unhex(line, dm[k]);
    if (fi) fclose(fi);
    if (dmn[0] != 55 || dmn[1] != 55) { fprintf(stderr, "FAIL hbp rssi: parity_in_dmrd.txt not 2x55 bytes\n"); hr_fails++; }
    static const int hr_mag[][2] = {{0, 0}, {60, 63}, {85, 112}, {113, 57}, {255, 1}};
    if (!hr_fails) {
        for (int m = 0; m < (int)(sizeof hr_mag / sizeof hr_mag[0]); m++) {
            for (int ts = 1; ts <= 2; ts++) {
                for (int len = 53; len <= 55; len += 2) {
                    tr = translator_new(&cfg, loop);
                    translator_set_protocols(tr, (struct ipsc *)1, (struct hbp *)1);
                    ngv = 0; ncap = 0;
                    for (int k = 0; k < 2; k++) {
                        uint8_t d[128]; memcpy(d, dm[k], 55);
                        if (ts == 2) d[15] |= 0x80;
                        d[54] = (uint8_t)hr_mag[m][k];
                        translator_hbp_voice_received(tr, d, len);
                    }
                    ev_timer_after(loop, 0.05, stop_cb, loop);
                    ev_run(loop);
                    if (ngv != 2) {
                        fprintf(stderr, "FAIL hbp rssi: len %d ts %d: %d GROUP_VOICE, want 2\n", len, ts, ngv);
                        hr_fails++; translator_free(tr); continue;
                    }
                    for (int k = 0; k < 2; k++) {
                        const uint8_t *g = gvcap[k];
                        int mag  = (len == 55) ? hr_mag[m][k] : 0;
                        int raw = g[52] << 8 | g[53];
                        int want_raw = mag << 8, want = mag;
                        if (g[31] != (ts == 2 ? 0x80 : 0x00) || g[34] != 0x80 || raw != want_raw) {
                            fprintf(stderr, "FAIL hbp rssi: len %d ts %d %s: byte31=%02x byte34=%02x raw=%04x, want %02x 80 %04x\n",
                                    len, ts, k ? "TERM" : "HEAD", g[31], g[34], raw,
                                    ts == 2 ? 0x80 : 0x00, want_raw);
                            hr_fails++;
                        }
                        translator_ipsc_voice_received(tr, g, gvlen[k], ts, g[30]);
                        if (ncap != k + 1 || cap[k][54] != want || ((cap[k][15] & 0x80) != 0) != (ts == 2)) {
                            fprintf(stderr, "FAIL hbp rssi: len %d ts %d %s round trip: byte54=%d, want %d\n",
                                    len, ts, k ? "TERM" : "HEAD",
                                    ncap > k ? cap[k][54] : -1, want);
                            hr_fails++;
                        }
                    }
                    translator_free(tr);
                }
            }
        }
    }
    /* A terminator reading must not carry into the next call's TERM, whether that TERM is
     * relayed or synthesized after a stream timeout. */
    if (!hr_fails) {
        tr = translator_new(&cfg, loop);
        translator_set_protocols(tr, (struct ipsc *)1, (struct hbp *)1);
        ngv = 0;
        uint8_t d[128];
        /* call 1: HEAD, TERM with a reading */
        memcpy(d, dm[0], 55); translator_hbp_voice_received(tr, d, 55);
        memcpy(d, dm[1], 55); d[54] = 90; translator_hbp_voice_received(tr, d, 55);
        /* call 2: HEAD, one voice burst, then the stream stops -> synthesized TERM */
        memcpy(d, dm[0], 55); d[16] ^= 0x0F; translator_hbp_voice_received(tr, d, 55);
        d[15] = (d[15] & 0xC0) | HBPF_FRAMETYPE_VOICESYNC; translator_hbp_voice_received(tr, d, 55);
        ev_timer_after(loop, 0.8, stop_cb, loop);
        ev_run(loop);
        if (ngv < 4 || gvcap[1][30] != 0x02 || (gvcap[1][52] << 8 | gvcap[1][53]) != 90 << 8 ||
            gvcap[ngv - 1][30] != 0x02 || (gvcap[ngv - 1][52] | gvcap[ngv - 1][53]) != 0) {
            fprintf(stderr, "FAIL hbp rssi: TERM reading carried into the next call (%d GROUP_VOICE)\n", ngv);
            hr_fails++;
        }
        translator_free(tr);
    }
    printf("53/55-byte DMRD, RSSI packing, slot bit (HBP->IPSC): %d failures\n", hr_fails);

    ev_free(loop);
    return (fails || in_fails || bad_fails || sync_fails || le_fails || rs_fails || hr_fails) ? 1 : 0;
}
