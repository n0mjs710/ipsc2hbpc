/* ipsc.c — IPSC protocol stack: master and peer modes (port of ipsc/protocol.py). */
#include "ipsc.h"
#include "ipsc_const.h"
#include "translate.h"
#include "net.h"
#include "crypto.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define MAX_PEERS 14
#define LOGN "ipsc.protocol"

/* Per-timeslot source lock (peer/mesh mode): one call can be delivered from two
 * sources at once (a c-Bridge or DMRgateway re-injecting it). Lock the timeslot
 * to the first source; drop other sources while it is actively talking, and for
 * a guard window after its last frame keep dropping mid-call bursts (the lagging
 * duplicate's tail). A genuine new call opens with VOICE_HEAD and is let through. */
#define VOICE_LOCK_TIMEOUT 0.5   /* seconds — active-call window */
#define VOICE_DUP_GUARD    2.0   /* seconds — duplicate-tail guard after a call */

enum { ST_IDLE, ST_REGISTERING, ST_REGISTERED, ST_ACTIVE };

typedef struct {
    uint8_t pid[4];
    char    ip[16];
    int     port;
    uint8_t mode;
    double  last_ka;
    int     connected;    /* peer/mesh mode: peer-to-peer handshake complete */
    int     outstanding;  /* peer/mesh mode: unanswered PEER_ALIVE_REQ count */
    int     used;
} peer_t;

struct ipsc {
    const Config      *cfg;
    struct translator *tr;
    ev_loop           *loop;
    int                fd;
    int                is_master;

    uint8_t  our_id[4];      /* ipsc_master_id as 4 bytes */
    uint8_t  ts_flags[5];    /* mode_byte(1) + flags(4) */

    /* master */
    peer_t   peers[MAX_PEERS];
    int      npeers;
    ev_timer *watchdog;

    /* peer */
    int       state;
    int       connected;
    int       missed;
    ev_timer *ka_timer;
    /* peer mesh: ip->peers[] holds peers learned from the master's PEER_LIST_REPLY */
    struct { uint8_t src[4]; double t; int set; } vlock[3];  /* per-TS source lock ([1],[2]) */
};

/* ---------------- auth + send ---------------- */

static int check_auth(ipsc *ip, const uint8_t *data, int len)
{
    if (len <= AUTH_DIGEST_LEN) return 0;
    uint8_t dig[20];
    hmac_sha1(ip->cfg->auth_key, 20, data, (size_t)(len - AUTH_DIGEST_LEN), dig);
    return memcmp(data + len - AUTH_DIGEST_LEN, dig, AUTH_DIGEST_LEN) == 0;
}

static void ipsc_send(ipsc *ip, const uint8_t *pkt, int len, const char *host, int port)
{
    uint8_t out[600];
    if (len > (int)sizeof out - AUTH_DIGEST_LEN) return;
    memcpy(out, pkt, (size_t)len);
    int outlen = len;
    if (ip->cfg->auth_enabled) {
        uint8_t dig[20];
        hmac_sha1(ip->cfg->auth_key, 20, pkt, (size_t)len, dig);
        memcpy(out + len, dig, AUTH_DIGEST_LEN);
        outlen += AUTH_DIGEST_LEN;
    }
    log_wire("ipsc.wire", "IPSC SEND %s %d %s", host, len, log_hex(pkt, len));
    udp_sendto(ip->fd, out, (size_t)outlen, host, port);
}

/* ---------------- peer table (master) ---------------- */

static int find_peer(ipsc *ip, const uint8_t pid[4])
{
    for (int i = 0; i < MAX_PEERS; i++)
        if (ip->peers[i].used && memcmp(ip->peers[i].pid, pid, 4) == 0)
            return i;
    return -1;
}

static int peer_count(ipsc *ip)
{
    int c = 0;
    for (int i = 0; i < MAX_PEERS; i++) if (ip->peers[i].used) c++;
    return c;
}

static void send_peer_list(ipsc *ip, const char *host, int port)
{
    uint8_t entries[MAX_PEERS * 11];
    int n = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!ip->peers[i].used) continue;
        memcpy(entries + n, ip->peers[i].pid, 4); n += 4;
        struct in_addr a; inet_pton(AF_INET, ip->peers[i].ip, &a);
        memcpy(entries + n, &a.s_addr, 4); n += 4;     /* network order = big-endian */
        entries[n++] = (uint8_t)(ip->peers[i].port >> 8);
        entries[n++] = (uint8_t)(ip->peers[i].port & 0xFF);
        entries[n++] = ip->peers[i].mode;
    }
    uint8_t pkt[8 + MAX_PEERS * 11];
    int p = 0;
    pkt[p++] = PEER_LIST_REPLY;
    memcpy(pkt + p, ip->our_id, 4); p += 4;
    pkt[p++] = (uint8_t)(n >> 8);
    pkt[p++] = (uint8_t)(n & 0xFF);
    memcpy(pkt + p, entries, (size_t)n); p += n;
    ipsc_send(ip, pkt, p, host, port);
}

static void remove_peer(ipsc *ip, int idx)
{
    if (idx < 0 || !ip->peers[idx].used) return;
    ip->peers[idx].used = 0;
    if (peer_count(ip) > 0) {
        for (int i = 0; i < MAX_PEERS; i++)
            if (ip->peers[i].used) send_peer_list(ip, ip->peers[i].ip, ip->peers[i].port);
    } else {
        translator_peer_lost(ip->tr);
    }
}

/* ---------------- master handlers ---------------- */

static void master_reg_req(ipsc *ip, const uint8_t *d, int len, const char *host, int port)
{
    if (len < 10) { LOGW(LOGN, "MASTER_REG_REQ too short (%d bytes) from %s:%d", len, host, port); return; }
    uint8_t pid[4]; memcpy(pid, d + 1, 4);
    uint32_t pid_int = (uint32_t)pid[0]<<24 | (uint32_t)pid[1]<<16 | (uint32_t)pid[2]<<8 | pid[3];
    uint8_t pmode = d[5];

    if (ip->cfg->n_allowed_peer_ips > 0) {
        int ok = 0;
        for (int i = 0; i < ip->cfg->n_allowed_peer_ips; i++)
            if (!strcmp(host, ip->cfg->allowed_peer_ips[i])) { ok = 1; break; }
        if (!ok) { LOGW(LOGN, "MASTER_REG_REQ from %s:%d rejected — not in allowed_peer_ips", host, port); return; }
    }
    if (ip->cfg->n_allowed_peer_ids > 0) {
        int ok = 0;
        for (int i = 0; i < ip->cfg->n_allowed_peer_ids; i++)
            if (ip->cfg->allowed_peer_ids[i] == pid_int) { ok = 1; break; }
        if (!ok) { LOGW(LOGN, "MASTER_REG_REQ radio ID %u from %s:%d rejected — not in allowed_peer_ids", pid_int, host, port); return; }
    }

    int idx = find_peer(ip, pid);
    int is_new = (idx < 0);

    if (is_new && peer_count(ip) >= MAX_PEERS) {
        LOGW(LOGN, "MASTER_REG_REQ from %s:%d (id=%u) rejected — IPSC system full "
                   "(%d peers registered; IPSC maximum is 15 including the master)",
                   host, port, pid_int, peer_count(ip));
        return;
    }
    if (!is_new && strcmp(ip->peers[idx].ip, host) != 0) {
        LOGW(LOGN, "MASTER_REG_REQ from %s:%d (id=%u) rejected — peer ID already registered from %s",
                   host, port, pid_int, ip->peers[idx].ip);
        return;
    }

    int was_empty = (peer_count(ip) == 0);

    if (is_new) {
        for (int i = 0; i < MAX_PEERS; i++) if (!ip->peers[i].used) { idx = i; break; }
    }
    memcpy(ip->peers[idx].pid, pid, 4);
    snprintf(ip->peers[idx].ip, sizeof ip->peers[idx].ip, "%s", host);
    ip->peers[idx].port = port;
    ip->peers[idx].mode = pmode;
    ip->peers[idx].last_ka = ev_now(ip->loop);
    ip->peers[idx].used = 1;

    /* MASTER_REG_REPLY: 0x91 + master_id(4) + ts_flags(5) + num_peers(2) + version(4) */
    uint8_t pkt[16];
    int p = 0;
    pkt[p++] = MASTER_REG_REPLY;
    memcpy(pkt + p, ip->our_id, 4); p += 4;
    memcpy(pkt + p, ip->ts_flags, 5); p += 5;
    int npc = peer_count(ip);
    pkt[p++] = (uint8_t)(npc >> 8);
    pkt[p++] = (uint8_t)(npc & 0xFF);
    memcpy(pkt + p, ip->cfg->ipsc_version, 4); p += 4;
    ipsc_send(ip, pkt, p, host, port);
    send_peer_list(ip, host, port);

    if (is_new) {
        LOGI(LOGN, "IPSC peer registered: id=%u  %s:%d  (%d/%d peers)", pid_int, host, port, npc, MAX_PEERS);
        for (int i = 0; i < MAX_PEERS; i++)
            if (ip->peers[i].used && memcmp(ip->peers[i].pid, pid, 4) != 0)
                send_peer_list(ip, ip->peers[i].ip, ip->peers[i].port);
        if (was_empty) translator_peer_joined(ip->tr);
    } else {
        LOGI(LOGN, "IPSC peer re-registered: id=%u  %s:%d", pid_int, host, port);
    }
}

static void master_alive_req(ipsc *ip, const uint8_t *d, int len, const char *host, int port)
{
    if (len < 5) return;
    int idx = find_peer(ip, d + 1);
    if (idx < 0) {
        uint32_t pid_int = (uint32_t)d[1]<<24 | (uint32_t)d[2]<<16 | (uint32_t)d[3]<<8 | d[4];
        LOGD(LOGN, "MASTER_ALIVE_REQ from unregistered peer %u at %s:%d — ignored", pid_int, host, port);
        return;
    }
    ip->peers[idx].last_ka = ev_now(ip->loop);
    /* alive_reply: 0x97 + master_id(4) + ts_flags(5) + version(4) */
    uint8_t pkt[14];
    int p = 0;
    pkt[p++] = MASTER_ALIVE_REPLY;
    memcpy(pkt + p, ip->our_id, 4); p += 4;
    memcpy(pkt + p, ip->ts_flags, 5); p += 5;
    memcpy(pkt + p, ip->cfg->ipsc_version, 4); p += 4;
    ipsc_send(ip, pkt, p, host, port);
    LOGD(LOGN, "MASTER_ALIVE_REQ -> MASTER_ALIVE_REPLY to %s:%d", host, port);
}

static void master_peer_list_req(ipsc *ip, const char *host, int port)
{
    int known = 0;
    for (int i = 0; i < MAX_PEERS; i++)
        if (ip->peers[i].used && !strcmp(ip->peers[i].ip, host)) { known = 1; break; }
    if (!known) { LOGD(LOGN, "PEER_LIST_REQ from unregistered host %s — ignored", host); return; }
    LOGD(LOGN, "PEER_LIST_REQ from %s:%d", host, port);
    send_peer_list(ip, host, port);
}

static void master_de_reg_req(ipsc *ip, const uint8_t *d, int len, const char *host, int port)
{
    uint8_t pid[4] = {0,0,0,0};
    if (len >= 5) memcpy(pid, d + 1, 4);
    uint32_t pid_int = (uint32_t)pid[0]<<24 | (uint32_t)pid[1]<<16 | (uint32_t)pid[2]<<8 | pid[3];
    LOGI(LOGN, "IPSC peer de-registering: id=%u  %s:%d", pid_int, host, port);
    uint8_t pkt[5]; pkt[0] = DE_REG_REPLY; memcpy(pkt + 1, ip->our_id, 4);
    ipsc_send(ip, pkt, 5, host, port);
    remove_peer(ip, find_peer(ip, pid));
}

static void on_group_voice(ipsc *ip, const uint8_t *d, int len, const char *host, int port,
                           int from_master)
{
    if (!from_master) {                    /* master mode: require registered peer */
        if (peer_count(ip) == 0) return;
        if (len < 5 || find_peer(ip, d + 1) < 0) {
            LOGD(LOGN, "GROUP_VOICE from unregistered peer at %s:%d — dropped", host, port);
            return;
        }
    } else {
        if (!ip->connected) return;
    }
    if (len < GV_MIN_LEN) { LOGW(LOGN, "GROUP_VOICE too short (%d bytes) from %s:%d", len, host, port); return; }

    int burst_type = d[GV_BURST_TYPE_OFF];
    int call_info  = d[GV_CALL_INFO_OFF];
    LOGD(LOGN, "GROUP_VOICE len=%d burst=0x%02x raw[0:32]=%s from %s:%d",
         len, burst_type, log_hex(d, len < 32 ? len : 32), host, port);

    int ts;
    if (burst_type == VOICE_HEAD || burst_type == VOICE_TERM)
        ts = (call_info & TS_CALL_MSK) ? 2 : 1;
    else
        ts = (burst_type & 0x80) ? 2 : 1;

    /* Per-timeslot source lock (peer/mesh mode only, see VOICE_LOCK_TIMEOUT).
     * The first source to key a timeslot owns it; other sources are dropped
     * while it is active, and their mid-call bursts are dropped for a guard
     * window afterward (the lagging duplicate's tail). */
    if (!ip->is_master && (ts == 1 || ts == 2)) {
        const uint8_t *src = d + 1;   /* sending peer's radio id */
        double now = ev_now(ip->loop);
        if (ip->vlock[ts].set && memcmp(ip->vlock[ts].src, src, 4) != 0) {
            double age = now - ip->vlock[ts].t;
            if (age < VOICE_LOCK_TIMEOUT) {
                LOGD(LOGN, "IPSC peer: duplicate voice on ts=%d from %s:%d — dropped", ts, host, port);
                return;
            }
            if (burst_type != VOICE_HEAD && age < VOICE_DUP_GUARD) {
                LOGD(LOGN, "IPSC peer: duplicate tail on ts=%d from %s:%d (%.2fs) — dropped",
                     ts, host, port, age);
                return;
            }
        }
        memcpy(ip->vlock[ts].src, src, 4);
        ip->vlock[ts].t = now;
        ip->vlock[ts].set = 1;
    }

    translator_ipsc_voice_received(ip->tr, d, len, ts, burst_type);
}

/* ---------------- master watchdog ---------------- */

static void watchdog_cb(ev_loop *loop, void *ud)
{
    ipsc *ip = ud;
    double now = ev_now(loop);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!ip->peers[i].used) continue;
        if (now - ip->peers[i].last_ka > ip->cfg->keepalive_watchdog) {
            uint32_t pid_int = (uint32_t)ip->peers[i].pid[0]<<24 | (uint32_t)ip->peers[i].pid[1]<<16
                             | (uint32_t)ip->peers[i].pid[2]<<8 | ip->peers[i].pid[3];
            LOGW(LOGN, "IPSC watchdog: no keepalive for %.1fs (limit %ds) — peer %u (%s:%d) lost",
                 now - ip->peers[i].last_ka, ip->cfg->keepalive_watchdog,
                 pid_int, ip->peers[i].ip, ip->peers[i].port);
            remove_peer(ip, i);
        }
    }
    translator_check_call_timeouts(ip->tr);
    ip->watchdog = ev_timer_after(loop, 5.0, watchdog_cb, ip);
}

/* ---------------- peer mode ---------------- */

static const char *PMASTER(ipsc *ip) { return ip->cfg->ipsc_master_ip; }
static int          PMPORT(ipsc *ip)  { return ip->cfg->ipsc_master_port; }

static void peer_register(ipsc *ip)
{
    uint8_t pkt[14]; int p = 0;
    pkt[p++] = MASTER_REG_REQ;
    memcpy(pkt + p, ip->our_id, 4); p += 4;
    memcpy(pkt + p, ip->ts_flags, 5); p += 5;
    memcpy(pkt + p, ip->cfg->ipsc_version, 4); p += 4;
    ipsc_send(ip, pkt, p, PMASTER(ip), PMPORT(ip));
    ip->missed = 0;
    ip->state = ST_REGISTERING;
    LOGD(LOGN, "IPSC peer: sent MASTER_REG_REQ to master %s:%d", PMASTER(ip), PMPORT(ip));
}

static void peer_send_alive(ipsc *ip)
{
    uint8_t pkt[14]; int p = 0;
    pkt[p++] = MASTER_ALIVE_REQ;
    memcpy(pkt + p, ip->our_id, 4); p += 4;
    memcpy(pkt + p, ip->ts_flags, 5); p += 5;
    memcpy(pkt + p, ip->cfg->ipsc_version, 4); p += 4;
    ipsc_send(ip, pkt, p, PMASTER(ip), PMPORT(ip));
    LOGD(LOGN, "MASTER_ALIVE_REQ -> master %s:%d", PMASTER(ip), PMPORT(ip));
}

static void peer_lose_connection(ipsc *ip)
{
    int was = ip->connected;
    ip->connected = 0;
    ip->state = ST_IDLE;
    if (was) translator_peer_lost(ip->tr);
}

static void peer_reg_reply(ipsc *ip, const uint8_t *d, int len, const char *host, int port)
{
    if (len < 12) { LOGW(LOGN, "IPSC peer: MASTER_REG_REPLY too short (%d bytes) from %s:%d", len, host, port); return; }
    uint32_t master_id = (uint32_t)d[1]<<24 | (uint32_t)d[2]<<16 | (uint32_t)d[3]<<8 | d[4];
    int peer_cnt = (int)d[10] << 8 | d[11];
    ip->missed = 0;
    ip->state = ST_REGISTERED;
    LOGI(LOGN, "IPSC peer: registered with master id=%u at %s:%d  peers=%d", master_id, host, port, peer_cnt);
    uint8_t pkt[5]; pkt[0] = PEER_LIST_REQ; memcpy(pkt + 1, ip->our_id, 4);
    ipsc_send(ip, pkt, 5, PMASTER(ip), PMPORT(ip));
    if (!ip->connected) { ip->connected = 1; translator_peer_joined(ip->tr); }
}

/* ---------------- peer mode: full-mesh peer-to-peer ---------------- */

static uint32_t pid32(const uint8_t p[4])
{
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}

/* PEER_REG_REQ/REPLY = opcode + our_id(4) + version(4); PEER_ALIVE_* = opcode +
 * our_id(4) + ts_flags(5). */
static void peer_mesh_send(ipsc *ip, uint8_t opcode, int with_version, const char *host, int port)
{
    uint8_t pkt[16]; int p = 0;
    pkt[p++] = opcode;
    memcpy(pkt + p, ip->our_id, 4); p += 4;
    if (with_version) { memcpy(pkt + p, ip->cfg->ipsc_version, 4); p += 4; }
    else              { memcpy(pkt + p, ip->ts_flags, 5);        p += 5; }
    ipsc_send(ip, pkt, p, host, port);
}

/* Parse PEER_LIST_REPLY into ip->peers[]: 0x93 + master_id(4) + len(2) + N*[id(4) ip(4) port(2) mode(1)]. */
static void peer_process_list(ipsc *ip, const uint8_t *d, int len)
{
    if (len < 7) { LOGW(LOGN, "IPSC peer: PEER_LIST_REPLY too short (%d bytes)", len); return; }
    int entries_len = (int)d[5] << 8 | d[6];
    int end = 7 + entries_len; if (end > len) end = len;
    uint8_t seen[MAX_PEERS][4]; int nseen = 0;

    for (int off = 7; off + 11 <= end; off += 11) {
        const uint8_t *e = d + off;
        if (memcmp(e, ip->our_id, 4) == 0) continue;   /* never mesh with ourselves */
        struct in_addr a; memcpy(&a.s_addr, e + 4, 4);
        char ipstr[16]; inet_ntop(AF_INET, &a, ipstr, sizeof ipstr);
        int pport = (int)e[8] << 8 | e[9];
        int idx = find_peer(ip, e);
        if (idx < 0) {
            for (int i = 0; i < MAX_PEERS; i++) if (!ip->peers[i].used) { idx = i; break; }
            if (idx < 0) continue;                      /* table full */
            memset(&ip->peers[idx], 0, sizeof ip->peers[idx]);
            memcpy(ip->peers[idx].pid, e, 4);
            ip->peers[idx].used = 1;
            LOGI(LOGN, "IPSC peer: learned mesh peer %u at %s:%d", pid32(e), ipstr, pport);
        }
        snprintf(ip->peers[idx].ip, sizeof ip->peers[idx].ip, "%s", ipstr);
        ip->peers[idx].port = pport;
        ip->peers[idx].mode = e[10];
        if (nseen < MAX_PEERS) memcpy(seen[nseen++], e, 4);
    }
    /* drop peers no longer listed */
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!ip->peers[i].used) continue;
        int found = 0;
        for (int j = 0; j < nseen; j++) if (memcmp(seen[j], ip->peers[i].pid, 4) == 0) { found = 1; break; }
        if (!found) {
            LOGI(LOGN, "IPSC peer: mesh peer %u removed (not in peer list)", pid32(ip->peers[i].pid));
            ip->peers[i].used = 0;
        }
    }
    LOGD(LOGN, "IPSC peer: peer list processed — %d mesh peer(s)", peer_count(ip));
}

static void peer_on_reg_req(ipsc *ip, const uint8_t *d, const char *host, int port)
{
    peer_mesh_send(ip, PEER_REG_REPLY, 1, host, port);   /* confirm so the peer marks us CONNECTED */
    int idx = find_peer(ip, d + 1);
    if (idx >= 0) { snprintf(ip->peers[idx].ip, sizeof ip->peers[idx].ip, "%s", host); ip->peers[idx].port = port; }
    LOGD(LOGN, "IPSC peer: PEER_REG_REQ from %u at %s:%d — replied", pid32(d + 1), host, port);
}

static void peer_on_reg_reply(ipsc *ip, const uint8_t *d)
{
    int idx = find_peer(ip, d + 1);
    if (idx >= 0 && !ip->peers[idx].connected) {
        ip->peers[idx].connected = 1; ip->peers[idx].outstanding = 0;
        LOGI(LOGN, "IPSC peer: mesh peer %u CONNECTED (%s:%d)",
             pid32(d + 1), ip->peers[idx].ip, ip->peers[idx].port);
    }
}

static void peer_on_alive_req(ipsc *ip, const uint8_t *d, const char *host, int port)
{
    peer_mesh_send(ip, PEER_ALIVE_REPLY, 0, host, port);
    int idx = find_peer(ip, d + 1);
    if (idx >= 0) ip->peers[idx].outstanding = 0;
}

static void peer_on_alive_reply(ipsc *ip, const uint8_t *d)
{
    int idx = find_peer(ip, d + 1);
    if (idx >= 0) ip->peers[idx].outstanding = 0;
}

/* Once per keepalive tick (peer mode): register with new peers, keepalive the
 * connected ones, drop peers that stop answering. */
static void peer_service_mesh(ipsc *ip)
{
    for (int i = 0; i < MAX_PEERS; i++) {
        if (!ip->peers[i].used) continue;
        if (!ip->peers[i].connected) {
            peer_mesh_send(ip, PEER_REG_REQ, 1, ip->peers[i].ip, ip->peers[i].port);
            LOGD(LOGN, "IPSC peer: PEER_REG_REQ -> %u (%s:%d)",
                 pid32(ip->peers[i].pid), ip->peers[i].ip, ip->peers[i].port);
        } else if (ip->peers[i].outstanding >= ip->cfg->keepalive_missed_max) {
            LOGW(LOGN, "IPSC peer: mesh peer %u unresponsive — re-registering", pid32(ip->peers[i].pid));
            ip->peers[i].connected = 0; ip->peers[i].outstanding = 0;
        } else {
            peer_mesh_send(ip, PEER_ALIVE_REQ, 0, ip->peers[i].ip, ip->peers[i].port);
            ip->peers[i].outstanding++;
        }
    }
}

static void peer_keepalive_cb(ev_loop *loop, void *ud)
{
    ipsc *ip = ud;
    translator_check_call_timeouts(ip->tr);

    if (ip->state == ST_REGISTERING) {
        ip->missed++;
        if (ip->missed >= ip->cfg->keepalive_missed_max) {
            LOGW(LOGN, "IPSC peer: no registration reply from master %s:%d after %d attempts — retrying",
                 PMASTER(ip), PMPORT(ip), ip->cfg->keepalive_missed_max);
            peer_register(ip);
        }
    } else if (ip->state == ST_REGISTERED || ip->state == ST_ACTIVE) {
        if (ip->missed >= ip->cfg->keepalive_missed_max) {
            LOGW(LOGN, "IPSC peer: %d consecutive keepalives unanswered by master %s:%d — re-registering",
                 ip->cfg->keepalive_missed_max, PMASTER(ip), PMPORT(ip));
            peer_lose_connection(ip);
            peer_register(ip);
        } else {
            peer_send_alive(ip);
            ip->missed++;
        }
    }

    /* Full mesh: once the master has given us the peer list, register with and
     * keepalive every peer directly. */
    if (ip->state == ST_ACTIVE) peer_service_mesh(ip);

    ip->ka_timer = ev_timer_after(loop, ip->cfg->keepalive_interval, peer_keepalive_cb, ip);
}

/* ---------------- recv dispatch ---------------- */

static void recv_cb(ev_loop *loop, int fd, void *ud)
{
    (void)loop;
    ipsc *ip = ud;
    uint8_t buf[1024];
    char host[INET_ADDRSTRLEN]; int port = 0;
    int n = udp_recvfrom(fd, buf, sizeof buf, host, &port);
    if (n < 0) return;

    if (ip->cfg->auth_enabled) {
        if (!check_auth(ip, buf, n)) {
            LOGW(LOGN, "IPSC auth failure from %s:%d — packet dropped", host, port);
            return;
        }
        n -= AUTH_DIGEST_LEN;
    }
    if (n == 0) return;

    log_wire("ipsc.wire", "IPSC RECV %s %d %s", host, n, log_hex(buf, n));

    int opcode = buf[0];
    if (opcode == XCMP_XNL) { LOGD(LOGN, "XCMP/XNL received from %s:%d — ignored", host, port); return; }

    if (ip->is_master) {
        if (n >= 5) {
            int idx = find_peer(ip, buf + 1);
            if (idx >= 0 && !strcmp(ip->peers[idx].ip, host))
                ip->peers[idx].last_ka = ev_now(ip->loop);
        }
        switch (opcode) {
            case MASTER_REG_REQ:   master_reg_req(ip, buf, n, host, port); break;
            case MASTER_ALIVE_REQ: master_alive_req(ip, buf, n, host, port); break;
            case PEER_LIST_REQ:    master_peer_list_req(ip, host, port); break;
            case DE_REG_REQ:       master_de_reg_req(ip, buf, n, host, port); break;
            case GROUP_VOICE:      on_group_voice(ip, buf, n, host, port, 0); break;
            case PVT_VOICE:        LOGD(LOGN, "PVT_VOICE from %s:%d — ignored", host, port); break;
            case GROUP_DATA: case PVT_DATA:
                LOGD(LOGN, "Data packet 0x%02x from %s:%d — ignored", opcode, host, port); break;
            case REPEATER_BLOCKED: LOGD(LOGN, "REPEATER_BLOCKED from %s:%d", host, port); break;
            case CALL_INTERRUPT_REQ: LOGD(LOGN, "CALL_INTERRUPT_REQ from %s:%d", host, port); break;
            case OPCODE_0xF0:      LOGD(LOGN, "0xF0 from %s:%d — observed, benign, no response sent", host, port); break;
            case CALL_CONFIRMATION: case TXT_MESSAGE_ACK: case CALL_MON_STATUS:
            case CALL_MON_RPT: case RPT_WAKE_UP: case MASTER_REG_REPLY: case PEER_LIST_REPLY:
            case PEER_REG_REQ: case PEER_REG_REPLY: case MASTER_ALIVE_REPLY: case PEER_ALIVE_REQ:
            case PEER_ALIVE_REPLY: case DE_REG_REPLY: case SYSTEM_MAP_REQ: case SYSTEM_MAP_REPLY:
            case UNKNOWN_9E: case WIRELINE: case REMOTE_PROG_REQ: case REMOTE_PROG_REPLY:
                LOGD(LOGN, "opcode 0x%02x from %s:%d — received, not handled", opcode, host, port); break;
            default:
                LOGW(LOGN, "unknown opcode 0x%02x from %s:%d len=%d — no handler  raw=%s",
                     opcode, host, port, n, log_hex(buf, n));
        }
    } else {
        /* IPSC is a full mesh: voice and the peer-to-peer handshake arrive
         * directly from every peer we learned in PEER_LIST_REPLY, not just from
         * the master. Accept from the master or any known peer; drop the rest. */
        int from_master = !strcmp(host, ip->cfg->ipsc_master_ip);
        int known = (n >= 5) && (find_peer(ip, buf + 1) >= 0);
        if (!from_master && !known) {
            LOGD(LOGN, "IPSC peer: 0x%02x from unknown source %s:%d — dropped", opcode, host, port);
            return;
        }
        switch (opcode) {
            case GROUP_VOICE:        on_group_voice(ip, buf, n, host, port, 1); break;
            case MASTER_REG_REPLY:   peer_reg_reply(ip, buf, n, host, port); break;
            case PEER_LIST_REPLY:    ip->missed = 0; ip->state = ST_ACTIVE;
                                     peer_process_list(ip, buf, n); break;
            case MASTER_ALIVE_REPLY: ip->missed = 0; LOGD(LOGN, "MASTER_ALIVE_REPLY from master %s:%d", host, port); break;
            case PEER_REG_REQ:       peer_on_reg_req(ip, buf, host, port); break;
            case PEER_REG_REPLY:     peer_on_reg_reply(ip, buf); break;
            case PEER_ALIVE_REQ:     peer_on_alive_req(ip, buf, host, port); break;
            case PEER_ALIVE_REPLY:   peer_on_alive_reply(ip, buf); break;
            case SYSTEM_MAP_REPLY:   LOGI(LOGN, "SYSTEM_MAP_REPLY (0x9D) from master %s:%d len=%d raw=%s",
                                          host, port, n, log_hex(buf, n)); break;
            case SYSTEM_MAP_REQ:     LOGI(LOGN, "SYSTEM_MAP_REQ (0x9C) from master %s:%d len=%d raw=%s",
                                          host, port, n, log_hex(buf, n)); break;
            case GROUP_DATA: case PVT_DATA:
                LOGD(LOGN, "Data packet 0x%02x from %s:%d — ignored", opcode, host, port); break;
            case CALL_CONFIRMATION: case TXT_MESSAGE_ACK: case CALL_MON_STATUS: case CALL_MON_RPT:
            case REPEATER_BLOCKED: case PVT_VOICE: case RPT_WAKE_UP: case CALL_INTERRUPT_REQ:
            case DE_REG_REPLY: case UNKNOWN_9E: case WIRELINE: case REMOTE_PROG_REQ:
            case REMOTE_PROG_REPLY: case OPCODE_0xF0:
                LOGD(LOGN, "opcode 0x%02x from %s:%d — received, not handled", opcode, host, port); break;
            default:
                LOGW(LOGN, "unknown opcode 0x%02x from %s:%d len=%d — no handler  raw=%s",
                     opcode, host, port, n, log_hex(buf, n));
        }
    }
}

/* ---------------- public API ---------------- */

ipsc *ipsc_new(const Config *cfg, struct translator *tr, ev_loop *loop)
{
    ipsc *ip = calloc(1, sizeof *ip);
    ip->cfg = cfg; ip->tr = tr; ip->loop = loop; ip->fd = -1;
    ip->is_master = !strcmp(cfg->ipsc_mode, "MASTER");
    ip->our_id[0] = (uint8_t)(cfg->ipsc_master_id >> 24);
    ip->our_id[1] = (uint8_t)(cfg->ipsc_master_id >> 16);
    ip->our_id[2] = (uint8_t)(cfg->ipsc_master_id >> 8);
    ip->our_id[3] = (uint8_t)(cfg->ipsc_master_id);
    ip->ts_flags[0] = cfg->ipsc_mode_byte;
    memcpy(ip->ts_flags + 1, cfg->ipsc_flags_bytes, 4);
    ip->state = ST_IDLE;
    return ip;
}

int ipsc_start(ipsc *ip)
{
    ip->fd = udp_bind(ip->cfg->ipsc_bind_ip, ip->cfg->ipsc_bind_port);
    if (ip->fd < 0) return -1;
    ev_add_fd(ip->loop, ip->fd, recv_cb, ip);
    if (ip->is_master) {
        LOGI(LOGN, "IPSC master listening — %s:%d  (max %d peers)",
             ip->cfg->ipsc_bind_ip, ip->cfg->ipsc_bind_port, MAX_PEERS);
        ip->watchdog = ev_timer_after(ip->loop, 5.0, watchdog_cb, ip);
    } else {
        LOGI(LOGN, "IPSC peer socket bound — connecting to master %s:%d as id=%u",
             ip->cfg->ipsc_master_ip, ip->cfg->ipsc_master_port, ip->cfg->ipsc_master_id);
        peer_register(ip);
        ip->ka_timer = ev_timer_after(ip->loop, ip->cfg->keepalive_interval, peer_keepalive_cb, ip);
    }
    return 0;
}

void ipsc_stop(ipsc *ip)
{
    if (!ip->is_master && ip->connected) {
        uint8_t pkt[5]; pkt[0] = DE_REG_REQ; memcpy(pkt + 1, ip->our_id, 4);
        ipsc_send(ip, pkt, 5, PMASTER(ip), PMPORT(ip));
        LOGI(LOGN, "IPSC peer: sent DE_REG_REQ to master %s:%d", PMASTER(ip), PMPORT(ip));
    }
    ip->connected = 0;
}

void ipsc_free(ipsc *ip)
{
    if (!ip) return;
    if (ip->fd >= 0) { ev_del_fd(ip->loop, ip->fd); close(ip->fd); }
    free(ip);
}

void ipsc_send_voice(ipsc *ip, const uint8_t *pkt, int len)
{
    if (ip->is_master) {
        for (int i = 0; i < MAX_PEERS; i++)
            if (ip->peers[i].used) ipsc_send(ip, pkt, len, ip->peers[i].ip, ip->peers[i].port);
    } else {
        /* Fan out to the whole mesh — master plus every connected peer (unicast
         * emulation of multicast). Traffic we RECEIVE over IPSC is never sent
         * back here: the source already flooded it to every peer directly. */
        if (!ip->connected) return;
        ipsc_send(ip, pkt, len, PMASTER(ip), PMPORT(ip));
        for (int i = 0; i < MAX_PEERS; i++)
            if (ip->peers[i].used && ip->peers[i].connected)
                ipsc_send(ip, pkt, len, ip->peers[i].ip, ip->peers[i].port);
    }
}

int ipsc_has_peers(ipsc *ip)
{
    return ip->is_master ? (peer_count(ip) > 0) : ip->connected;
}
