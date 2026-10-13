/* test_private.c — private (unit-to-unit) voice in both directions, using calls
 * captured from a MOTOTRBO repeater and an HBlink3 server.
 *
 *   pvt_ipsc_in.txt : PVT_VOICE call 3120999 -> 3120101, TS1, from the repeater
 *   pvt_hbp_in.txt  : HBP private-call DMRD 3120101 -> 3120999, TS1, from HBlink3
 *
 * Each direction is run with the full call and as a late entry (headers dropped).
 * Stubs the ipsc and hbp layers so only translate.c + the dmr module run. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/translate.h"
#include "../src/config.h"
#include "../src/eventloop.h"
#include "../src/dmr/dmr.h"

#define MAXCAP 256
static uint8_t cap[MAXCAP][128]; static int caplen[MAXCAP]; static int ncap;
static uint8_t gv[MAXCAP][128];  static int gvlen[MAXCAP];  static int ngv;

int  ipsc_has_peers(struct ipsc *ip) { (void)ip; return 1; }
void ipsc_send_voice(struct ipsc *ip, const uint8_t *p, int n) {
    (void)ip; if (ngv < MAXCAP) { memcpy(gv[ngv], p, (size_t)n); gvlen[ngv++] = n; }
}
int  hbp_is_connected(struct hbp *hb) { (void)hb; return 1; }
void hbp_activate(struct hbp *hb) { (void)hb; }
void hbp_deactivate(struct hbp *hb) { (void)hb; }
void hbp_send_dmrd(struct hbp *hb, const uint8_t *d, int n) {
    (void)hb; if (ncap < MAXCAP) { memcpy(cap[ncap], d, (size_t)n); caplen[ncap++] = n; }
}

static void stop_cb(ev_loop *loop, void *ud){ (void)ud; ev_stop(loop); }
static int hexv(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; return -1; }
static int unhex(const char *s, uint8_t *out){ int n=0; for(;s[0]&&s[1]&&hexv(s[0])>=0;s+=2) out[n++]=(uint8_t)((hexv(s[0])<<4)|hexv(s[1])); return n; }
static unsigned id3(const uint8_t *p) { return (unsigned)(p[0]<<16 | p[1]<<8 | p[2]); }

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); fails++; } } while (0)

/* LC carried in a DMRD voice header / terminator (BPTC full LC). */
static void dmrd_lc(const uint8_t *dmrd, uint8_t lc[9])
{
    dmr_bit fb[264]; dmr_bytes_to_bits(dmrd + 20, 33, fb);
    dmr_bit b[196]; memcpy(b, fb, 98); memcpy(b + 98, fb + 166, 98);
    dmr_bptc_decode_full_lc(b, lc);
}

static void ipsc_to_hbp(const Config *cfg, ev_loop *loop, int late)
{
    const char *name = late ? "IPSC->HBP late entry" : "IPSC->HBP";
    translator *tr = translator_new(cfg, loop);
    translator_set_protocols(tr, (struct ipsc *)1, (struct hbp *)1);
    ncap = 0;
    FILE *f = fopen("tests/pvt_ipsc_in.txt", "r");
    if (!f) { perror("tests/pvt_ipsc_in.txt"); exit(2); }
    char line[512]; int fed = 0;
    while (fgets(line, sizeof line, f)) {
        int ts, bt; char hex[400];
        if (sscanf(line, "%d %d %399s", &ts, &bt, hex) != 3) continue;
        if (late && bt == 1) continue;              /* drop VOICE_HEADs */
        uint8_t raw[400]; int n = unhex(hex, raw);
        translator_ipsc_voice_received(tr, raw, n, ts, bt);
        fed++;
    }
    fclose(f);
    CHECK(ncap > 0, "%s: no DMRD produced", name);
    int hdr = 0, term = 0;
    for (int i = 0; i < ncap; i++) {
        const uint8_t *d = cap[i]; int fl = d[15];
        CHECK(fl & 0x40, "%s: DMRD[%d] flags 0x%02x missing private-call bit", name, i, fl);
        CHECK(!(fl & 0x80), "%s: DMRD[%d] not TS1", name, i);
        CHECK(id3(d + 5) == 3120999 && id3(d + 8) == 3120101, "%s: DMRD[%d] src/dst %u->%u", name, i, id3(d + 5), id3(d + 8));
        if ((fl & 0x30) == 0x20) {
            uint8_t lc[9]; dmrd_lc(d, lc);
            CHECK((lc[0] & 0x3F) == 3, "%s: DMRD[%d] LC FLCO 0x%02x, want 3", name, i, lc[0] & 0x3F);
            if ((fl & 0x0F) == 1) hdr++; else if ((fl & 0x0F) == 2) term++;
        }
    }
    CHECK(term == 1, "%s: %d terminators, want 1", name, term);
    if (!late) CHECK(hdr == 4 && ncap == fed, "%s: %d DMRD (%d headers) from %d IPSC packets", name, ncap, hdr, fed);
    else       CHECK(hdr == 0, "%s: %d headers, want 0", name, hdr);
    printf("%s: %d DMRD\n", name, ncap);
    translator_free(tr);
}

static void hbp_to_ipsc(const Config *cfg, ev_loop *loop, int late)
{
    const char *name = late ? "HBP->IPSC late entry" : "HBP->IPSC";
    translator *tr = translator_new(cfg, loop);
    translator_set_protocols(tr, (struct ipsc *)1, (struct hbp *)1);
    ngv = 0;
    FILE *f = fopen("tests/pvt_hbp_in.txt", "r");
    if (!f) { perror("tests/pvt_hbp_in.txt"); exit(2); }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        uint8_t raw[128]; int n = unhex(line, raw);
        if (n < 55) continue;
        if (late && (raw[15] & 0x3F) == 0x21) continue;   /* drop voice headers */
        translator_hbp_voice_received(tr, raw, n);
    }
    fclose(f);
    ev_timer_after(loop, 4.0, stop_cb, loop);
    ev_run(loop);
    CHECK(ngv > 0, "%s: no IPSC voice produced", name);
    int term = 0;
    for (int i = 0; i < ngv; i++) {
        const uint8_t *g = gv[i];
        CHECK(g[0] == 0x81, "%s: GV[%d] opcode 0x%02x, want PVT_VOICE 0x81", name, i, g[0]);
        CHECK(id3(g + 6) == 3120101 && id3(g + 9) == 3120999, "%s: GV[%d] src/dst %u->%u", name, i, id3(g + 6), id3(g + 9));
        if (g[30] == 1 || g[30] == 2) {             /* VOICE_HEAD / VOICE_TERM: LC at 38 */
            CHECK((g[38] & 0x3F) == 3, "%s: GV[%d] LC FLCO 0x%02x, want 3", name, i, g[38] & 0x3F);
            if (g[30] == 2) term++;
        }
    }
    CHECK(term == 1, "%s: %d terminators, want 1", name, term);
    printf("%s: %d PVT_VOICE\n", name, ngv);
    translator_free(tr);
}

int main(void)
{
    Config cfg; char err[4096];
    if (config_load("tests/parity.toml", &cfg, err, sizeof err)) { fprintf(stderr, "config: %s\n", err); return 2; }
    ev_loop *loop = ev_new();
    ipsc_to_hbp(&cfg, loop, 0);
    ipsc_to_hbp(&cfg, loop, 1);
    hbp_to_ipsc(&cfg, loop, 0);
    hbp_to_ipsc(&cfg, loop, 1);
    ev_free(loop);
    printf("Private calls: %d failures\n", fails);
    return fails ? 1 : 0;
}
