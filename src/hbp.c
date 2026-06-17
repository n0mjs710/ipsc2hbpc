/* hbp.c — HBP outbound client + reconnect manager (port of hbp/protocol.py). */
#include "hbp.h"
#include "hbp_const.h"
#include "translate.h"
#include "net.h"
#include "crypto.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOGN "hbp.protocol"

#define KA_INTERVAL     5.0
#define MAX_KA_AGE     15.0
#define RECONNECT_DELAY 5.0

enum { ST_LOGIN, ST_AUTH_SENT, ST_CONFIG_SENT, ST_OPTIONS_SENT, ST_CONNECTED, ST_DISCONNECTED };

struct hbp {
    const Config      *cfg;
    struct translator *tr;
    ev_loop           *loop;
    int                fd;
    int                active;
    int                state;
    double             last_pong;
    uint8_t            radio_id[4];
    ev_timer          *ping_timer;
    ev_timer          *reconnect_timer;
};

static void hbp_connect(hbp *hb);
static void disconnect(hbp *hb, int send_rptcl);

/* ljust(n, '\0')[:n] */
static void enc_field(uint8_t *dst, int n, const char *s)
{
    int sl = (int)strlen(s);
    if (sl > n) sl = n;
    memcpy(dst, s, (size_t)sl);
    for (int i = sl; i < n; i++) dst[i] = 0;
}

static int build_rptc(hbp *hb, uint8_t out[RPTC_LEN])
{
    int p = 0;
    memcpy(out + p, "RPTC", 4); p += 4;
    memcpy(out + p, hb->radio_id, 4); p += 4;
    enc_field(out + p, 8,  hb->cfg->callsign);    p += 8;
    enc_field(out + p, 9,  hb->cfg->rx_freq);     p += 9;
    enc_field(out + p, 9,  hb->cfg->tx_freq);     p += 9;
    enc_field(out + p, 2,  hb->cfg->tx_power);    p += 2;
    enc_field(out + p, 2,  hb->cfg->colorcode);   p += 2;
    enc_field(out + p, 8,  hb->cfg->latitude);    p += 8;
    enc_field(out + p, 9,  hb->cfg->longitude);   p += 9;
    enc_field(out + p, 3,  hb->cfg->height);      p += 3;
    enc_field(out + p, 20, hb->cfg->location);    p += 20;
    enc_field(out + p, 19, hb->cfg->description); p += 19;
    out[p++] = '3';                               /* RPTC_SLOTS_VALUE */
    enc_field(out + p, 124, hb->cfg->url);        p += 124;
    enc_field(out + p, 40,  hb->cfg->software_id);p += 40;
    enc_field(out + p, 40,  hb->cfg->package_id); p += 40;
    return p;   /* == RPTC_LEN */
}

static void send_raw(hbp *hb, const uint8_t *data, int len)
{
    if (hb->fd < 0) return;
    log_wire("hbp.wire", "HBP SEND %s %d %s", hb->cfg->hbp_master_ip, len, log_hex(data, len));
    send(hb->fd, data, (size_t)len, 0);
}

static void become_connected(hbp *hb);

static void on_rptack(hbp *hb, const uint8_t *d, int len)
{
    if (hb->state == ST_LOGIN) {
        if (len < RPTACK_NONCE_OFF + 4) { LOGE(LOGN, "HBP: RPTACK+salt too short (%d bytes)", len); return; }
        uint8_t msg[4 + 256];
        memcpy(msg, d + RPTACK_NONCE_OFF, 4);
        memcpy(msg + 4, hb->cfg->hbp_passphrase, (size_t)hb->cfg->hbp_passphrase_len);
        uint8_t digest[32];
        sha256(msg, (size_t)(4 + hb->cfg->hbp_passphrase_len), digest);
        uint8_t pkt[4 + 4 + 32];
        memcpy(pkt, "RPTK", 4);
        memcpy(pkt + 4, hb->radio_id, 4);
        memcpy(pkt + 8, digest, 32);
        send_raw(hb, pkt, 40);
        hb->state = ST_AUTH_SENT;
        LOGI(LOGN, "HBP: <- RPTACK+salt  -> RPTK");
    } else if (hb->state == ST_AUTH_SENT) {
        uint8_t rptc[RPTC_LEN];
        int n = build_rptc(hb, rptc);
        send_raw(hb, rptc, n);
        hb->state = ST_CONFIG_SENT;
        LOGI(LOGN, "HBP: <- RPTACK(auth)  -> RPTC (%d bytes)", n);
    } else if (hb->state == ST_CONFIG_SENT) {
        if (hb->cfg->options[0]) {
            uint8_t pkt[4 + 4 + 300];
            memcpy(pkt, "RPTO", 4);
            memcpy(pkt + 4, hb->radio_id, 4);
            enc_field(pkt + 8, 300, hb->cfg->options);
            send_raw(hb, pkt, 308);
            hb->state = ST_OPTIONS_SENT;
            LOGI(LOGN, "HBP: <- RPTACK(config)  -> RPTO  options=%s", hb->cfg->options);
        } else {
            LOGI(LOGN, "HBP: <- RPTACK(config)  CONNECTED to %s:%d",
                 hb->cfg->hbp_master_ip, hb->cfg->hbp_master_port);
            become_connected(hb);
        }
    } else if (hb->state == ST_OPTIONS_SENT) {
        LOGI(LOGN, "HBP: <- RPTACK(options)  CONNECTED to %s:%d",
             hb->cfg->hbp_master_ip, hb->cfg->hbp_master_port);
        become_connected(hb);
    } else {
        LOGD(LOGN, "HBP: unexpected RPTACK in state %d", hb->state);
    }
}

static void ping_cb(ev_loop *loop, void *ud)
{
    hbp *hb = ud;
    hb->ping_timer = NULL;
    if (hb->state != ST_CONNECTED) return;
    /* RPTPING magic (7 bytes) + radio_id(4) = 11 bytes */
    uint8_t ping[7 + 4];
    memcpy(ping, "RPTPING", 7);
    memcpy(ping + 7, hb->radio_id, 4);
    send_raw(hb, ping, 11);
    LOGD(LOGN, "HBP: -> RPTPING");
    double age = ev_now(loop) - hb->last_pong;
    if (age > MAX_KA_AGE) {
        LOGE(LOGN, "HBP: watchdog — no MSTPONG for %.1fs — disconnecting", age);
        translator_hbp_disconnected(hb->tr);
        disconnect(hb, 0);
        return;
    }
    hb->ping_timer = ev_timer_after(loop, KA_INTERVAL, ping_cb, hb);
}

static void become_connected(hbp *hb)
{
    hb->state = ST_CONNECTED;
    hb->last_pong = ev_now(hb->loop);
    hb->ping_timer = ev_timer_after(hb->loop, KA_INTERVAL, ping_cb, hb);
    translator_hbp_connected(hb->tr);
}

static void recv_cb(ev_loop *loop, int fd, void *ud)
{
    hbp *hb = ud;
    uint8_t buf[1024];
    int n = (int)recv(fd, buf, sizeof buf, 0);
    if (n < 4) return;
    log_wire("hbp.wire", "HBP RECV %s %d %s", hb->cfg->hbp_master_ip, n, log_hex(buf, n));

    if (n >= 6 && memcmp(buf, "RPTACK", 6) == 0) {
        on_rptack(hb, buf, n);
    } else if (n >= 6 && memcmp(buf, "MSTNAK", 6) == 0) {
        LOGE(LOGN, "HBP: <- MSTNAK — rejected by server");
        disconnect(hb, 0);
    } else if (n >= 7 && memcmp(buf, "MSTPONG", 7) == 0) {
        hb->last_pong = ev_now(loop);
        LOGD(LOGN, "HBP: <- MSTPONG");
    } else if (n >= 5 && memcmp(buf, "MSTCL", 5) == 0) {
        LOGI(LOGN, "HBP: <- MSTCL — server initiated disconnect");
        disconnect(hb, 0);
    } else if (memcmp(buf, "DMRD", 4) == 0) {
        if (hb->state == ST_CONNECTED)
            translator_hbp_voice_received(hb->tr, buf, n);
    } else {
        LOGD(LOGN, "HBP: unknown packet len=%d", n);
    }
}

static void reconnect_cb(ev_loop *loop, void *ud)
{
    (void)loop;
    hbp *hb = ud;
    hb->reconnect_timer = NULL;
    if (hb->active) hbp_connect(hb);
}

static void schedule_reconnect(hbp *hb)
{
    LOGI(LOGN, "HBP: reconnecting in %.0fs", RECONNECT_DELAY);
    hb->reconnect_timer = ev_timer_after(hb->loop, RECONNECT_DELAY, reconnect_cb, hb);
}

static void disconnect(hbp *hb, int send_rptcl)
{
    if (send_rptcl && hb->state == ST_CONNECTED) {
        /* RPTCL magic (5 bytes) + radio_id(4) = 9 bytes */
        uint8_t cl[5 + 4];
        memcpy(cl, "RPTCL", 5);
        memcpy(cl + 5, hb->radio_id, 4);
        send_raw(hb, cl, 9);
        LOGI(LOGN, "HBP: -> RPTCL (clean disconnect)");
    }
    hb->state = ST_DISCONNECTED;
    if (hb->ping_timer) { ev_timer_cancel(hb->loop, hb->ping_timer); hb->ping_timer = NULL; }
    if (hb->fd >= 0) { ev_del_fd(hb->loop, hb->fd); close(hb->fd); hb->fd = -1; }
    if (hb->active) schedule_reconnect(hb);
}

static void hbp_connect(hbp *hb)
{
    hb->fd = udp_connect(hb->cfg->hbp_master_ip, hb->cfg->hbp_master_port);
    if (hb->fd < 0) {
        LOGE(LOGN, "HBP: connect failed");
        if (hb->active) schedule_reconnect(hb);
        return;
    }
    ev_add_fd(hb->loop, hb->fd, recv_cb, hb);
    hb->state = ST_LOGIN;
    LOGI(LOGN, "HBP: UDP endpoint created -> %s:%d", hb->cfg->hbp_master_ip, hb->cfg->hbp_master_port);
    uint8_t pkt[4 + 4];
    memcpy(pkt, "RPTL", 4);
    memcpy(pkt + 4, hb->radio_id, 4);
    send_raw(hb, pkt, 8);
    LOGI(LOGN, "HBP: -> RPTL  radio_id=%u", hb->cfg->hbp_repeater_id);
}

/* ---------------- public API ---------------- */

hbp *hbp_new(const Config *cfg, struct translator *tr, ev_loop *loop)
{
    hbp *hb = calloc(1, sizeof *hb);
    hb->cfg = cfg; hb->tr = tr; hb->loop = loop; hb->fd = -1;
    hb->state = ST_DISCONNECTED;
    hb->radio_id[0] = (uint8_t)(cfg->hbp_repeater_id >> 24);
    hb->radio_id[1] = (uint8_t)(cfg->hbp_repeater_id >> 16);
    hb->radio_id[2] = (uint8_t)(cfg->hbp_repeater_id >> 8);
    hb->radio_id[3] = (uint8_t)(cfg->hbp_repeater_id);
    return hb;
}

void hbp_start(hbp *hb)
{
    if (!strcmp(hb->cfg->hbp_mode, "PERSISTENT"))
        hbp_activate(hb);
}

void hbp_activate(hbp *hb)
{
    if (hb->active) return;
    hb->active = 1;
    LOGI(LOGN, "HBP client activated");
    hbp_connect(hb);
}

void hbp_deactivate(hbp *hb)
{
    if (!hb->active) return;
    hb->active = 0;
    LOGI(LOGN, "HBP client deactivated");
    disconnect(hb, 1);
}

void hbp_stop(hbp *hb)
{
    hb->active = 0;
    disconnect(hb, 1);
    if (hb->reconnect_timer) { ev_timer_cancel(hb->loop, hb->reconnect_timer); hb->reconnect_timer = NULL; }
}

void hbp_free(hbp *hb)
{
    if (!hb) return;
    if (hb->fd >= 0) { ev_del_fd(hb->loop, hb->fd); close(hb->fd); }
    free(hb);
}

void hbp_send_dmrd(hbp *hb, const uint8_t *data, int len)
{
    if (hb->state == ST_CONNECTED && hb->fd >= 0)
        send_raw(hb, data, len);
}

int hbp_is_connected(hbp *hb)
{
    return hb->state == ST_CONNECTED;
}
