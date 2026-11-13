/* test_hbp_gap.c — HBP->IPSC: a pause inside a network call is jitter, not the
 * end of the call.  The delivery clock fills a missing burst with silence; it
 * ends the call (synthesized VOICE_TERM) only after the network has been quiet
 * for the watchdog time, as MMDVMHost does for network voice (it fills silence
 * after 360 ms of lost audio and ends the call when m_networkWatchdog, 1500 ms,
 * expires).
 *
 *   1. a 600 ms pause mid-call: one call to the repeater (one IPSC call id, one
 *      VOICE_TERM, at the end), every burst delivered;
 *   2. the stream stops with no VOICE_TERM: the call is ended 1.5 s after the
 *      last burst, not sooner, and only once.
 *
 * The VOICE_HEAD / VOICE_TERM come from tests/parity_in_dmrd.txt (group call
 * 3120000 -> TG 91); the voice bursts between them are built here.  Stubs the
 * ipsc and hbp layers so only translate.c + the dmr module run. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/translate.h"
#include "../src/config.h"
#include "../src/eventloop.h"
#include "../src/dmr/dmr.h"
#include "../src/hbp_const.h"
#include "../src/ipsc_const.h"

#define MAXCAP 512
static uint8_t gv[MAXCAP][128]; static int ngv;
static double  gv_time[MAXCAP];
static ev_loop *g_loop;

int  ipsc_has_peers(struct ipsc *ip) { (void)ip; return 1; }
void ipsc_send_voice(struct ipsc *ip, const uint8_t *p, int n) {
    (void)ip;
    if (ngv < MAXCAP) { memcpy(gv[ngv], p, (size_t)(n < 128 ? n : 128)); gv_time[ngv++] = ev_now(g_loop); }
}
int  hbp_is_connected(struct hbp *hb) { (void)hb; return 1; }
void hbp_activate(struct hbp *hb) { (void)hb; }
void hbp_deactivate(struct hbp *hb) { (void)hb; }
void hbp_send_dmrd(struct hbp *hb, const uint8_t *d, int n) { (void)hb; (void)d; (void)n; }

static int hexv(char c){ if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; return -1; }
static int unhex(const char *s, uint8_t *out){ int n=0; for(;s[0]&&s[1]&&hexv(s[0])>=0;s+=2) out[n++]=(uint8_t)((hexv(s[0])<<4)|hexv(s[1])); return n; }

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); fails++; } } while (0)

/* GROUP_VOICE layout (build_gv): [5] IPSC call id, [30] burst type. */
#define GV_CALL_ID  5
#define GV_BURST    30

static uint8_t head[128], term[128];
static int head_len, term_len;

/* ---- scripted HBP feed: one DMRD per timer tick ---- */
typedef struct { double at; int kind; } feed_item;   /* kind: 0 head, 1..n voice burst index, -1 term */
static feed_item feed[256];
static int nfeed, next_feed;
static translator *g_tr;
static double t_last_burst, g_base;

static void voice_dmrd(int idx, uint8_t out[DMRD_LEN])
{
    memcpy(out, head, 20);                      /* DMRD, seq, src, dst, repeater id, flags, stream id */
    out[4] = (uint8_t)idx;
    int pos = (idx - 1) % 6;                    /* bursts A..F */
    out[15] = (uint8_t)((head[15] & 0xC0) | (pos == 0 ? 0x10 : pos));
    for (int i = 0; i < 33; i++) out[20 + i] = (uint8_t)(idx * 13 + i);
    out[53] = 0; out[54] = 0;
}

static void feed_cb(ev_loop *loop, void *ud)
{
    (void)ud;
    feed_item *f = &feed[next_feed++];
    if (f->kind == 0) translator_hbp_voice_received(g_tr, head, head_len);
    else if (f->kind < 0) translator_hbp_voice_received(g_tr, term, term_len);
    else {
        uint8_t d[DMRD_LEN]; voice_dmrd(f->kind, d);
        translator_hbp_voice_received(g_tr, d, DMRD_LEN);
        t_last_burst = ev_now(loop);
    }
    if (next_feed < nfeed) ev_timer_at(loop, g_base + feed[next_feed].at, feed_cb, NULL);
}

static void stop_cb(ev_loop *loop, void *ud){ (void)ud; ev_stop(loop); }

/* Feed the script from now and run the loop for run_s. */
static void play(const Config *cfg, double run_s)
{
    g_tr = translator_new(cfg, g_loop);
    translator_set_protocols(g_tr, (struct ipsc *)1, (struct hbp *)1);
    ngv = 0; next_feed = 0;
    g_base = ev_now(g_loop) + 0.05;
    ev_timer_at(g_loop, g_base + feed[0].at, feed_cb, NULL);
    ev_timer_after(g_loop, run_s, stop_cb, g_loop);
    ev_run(g_loop);
    translator_free(g_tr);
}

static int count_terms(void)
{
    int n = 0;
    for (int i = 0; i < ngv; i++) if (gv[i][GV_BURST] == VOICE_TERM) n++;
    return n;
}

int main(void)
{
    Config cfg; char err[4096];
    if (config_load("tests/parity.toml", &cfg, err, sizeof err)) { fprintf(stderr, "config: %s\n", err); return 2; }
    FILE *f = fopen("tests/parity_in_dmrd.txt", "r");
    if (!f) { perror("tests/parity_in_dmrd.txt"); return 2; }
    char line[512];
    if (fgets(line, sizeof line, f)) head_len = unhex(line, head);
    if (fgets(line, sizeof line, f)) term_len = unhex(line, term);
    fclose(f);
    g_loop = ev_new();

    /* ---- 1: 30 bursts, a 600 ms pause, 30 bursts, VOICE_TERM ---- */
    nfeed = 0;
    feed[nfeed++] = (feed_item){ 0.0, 0 };
    double t = 0.06;
    for (int i = 1; i <= 60; i++) {
        feed[nfeed++] = (feed_item){ t, i };
        t += (i == 30) ? 0.60 : 0.06;
    }
    feed[nfeed++] = (feed_item){ t, -1 };
    play(&cfg, t + 1.0);
    int terms = count_terms(), voice = 0, one_call = 1;
    for (int i = 0; i < ngv; i++) {
        if (gv[i][GV_CALL_ID] != gv[0][GV_CALL_ID]) one_call = 0;
        if (gv[i][GV_BURST] != VOICE_HEAD && gv[i][GV_BURST] != VOICE_TERM) voice++;
    }
    CHECK(terms == 1, "600 ms pause: %d VOICE_TERMs sent to the repeater, want 1 (the call was cut at the pause)", terms);
    CHECK(ngv > 0 && gv[ngv - 1][GV_BURST] == VOICE_TERM, "600 ms pause: the VOICE_TERM is not the last frame");
    CHECK(one_call, "600 ms pause: the call reached the repeater as more than one IPSC call");
    CHECK(voice >= 60, "600 ms pause: %d voice frames delivered, want >= 60 (60 bursts + silence fill)", voice);
    printf("Pause mid-call (HBP->IPSC): %d frames, %d VOICE_TERM, one call: %s\n", ngv, terms, one_call ? "yes" : "no");

    /* ---- 2: 20 bursts, then nothing (VOICE_TERM lost) ---- */
    nfeed = 0;
    feed[nfeed++] = (feed_item){ 0.0, 0 };
    for (int i = 1; i <= 20; i++) feed[nfeed++] = (feed_item){ 0.06 * i, i };
    play(&cfg, 3.0);
    terms = count_terms();
    double end_after = -1;
    for (int i = 0; i < ngv; i++) if (gv[i][GV_BURST] == VOICE_TERM) end_after = gv_time[i] - t_last_burst;
    CHECK(terms == 1, "lost VOICE_TERM: %d VOICE_TERMs synthesized, want 1", terms);
    double want = 1.5 + cfg.jitter_buffer_depth * 0.060;   /* 1.5 s of silence after the buffered bursts drain */
    CHECK(end_after >= want - 0.1 && end_after <= want + 0.15,
          "lost VOICE_TERM: call ended %.2f s after the last burst, want %.2f s (1.5 s network watchdog)", end_after, want);
    printf("Lost VOICE_TERM (HBP->IPSC): call ended %.2f s after the last burst\n", end_after);

    ev_free(g_loop);
    printf("HBP gap self-test: %d failures\n", fails);
    return fails ? 1 : 0;
}
