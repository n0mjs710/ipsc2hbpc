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

    translator_free(tr); ev_free(loop);
    return (fails || in_fails) ? 1 : 0;
}
