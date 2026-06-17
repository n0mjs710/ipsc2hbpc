/* translate.c — CallTranslator: bidirectional IPSC <-> HBP translation
 * (port of translate/translator.py).  All bit/FEC work uses the dmr module. */
#include "translate.h"
#include "ipsc.h"
#include "hbp.h"
#include "ipsc_const.h"
#include "hbp_const.h"
#include "dmr/dmr.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#define LOGN "translate.translator"

#define JITTER_BUFFER_DEPTH 2     /* slots × 60 ms */
#define MAX_SYNTH_BURSTS    6
#define SLOT_MS             0.060

struct deliv_ctx { translator *tr; int ts; };

struct translator {
    const Config *cfg;
    ev_loop      *loop;
    struct ipsc  *ip;
    struct hbp   *hb;
    uint8_t       repeater_id_b[4];
    uint8_t       master_id_b[4];

    /* outbound (IPSC -> HBP), index 1,2 */
    int      out_has_stream[3];
    uint8_t  out_stream_id[3][4];
    int      out_ipsc_stream_id[3];   /* -1 = none */
    int      out_seq;
    int      out_frame_pos[3];
    int      out_has_lc[3];
    uint8_t  out_lc[3][9];
    int      out_has_emb[3];
    uint8_t  out_emb[3][4][4];
    int      out_ts_mismatch_warned[3];
    double   out_last_pkt[3];

    /* inbound (HBP -> IPSC) */
    int      in_has_lc[3];
    uint8_t  in_lc[3][9];
    int      in_has_emb[3];
    uint8_t  in_emb[3][4][4];
    int      in_stream_id[3];
    int      in_stream_ctr;
    int      in_has_hbp_stream[3];
    uint8_t  in_hbp_stream[3][4];
    int      in_rtp_seq[3];
    uint32_t in_rtp_ts[3];
    double   in_last_pkt[3];
    int      in_buf_present[3][6];
    uint8_t  in_buf[3][6][19];
    double   in_next_slot_time[3];
    int      in_burst_pos[3];
    int      in_consec_synth[3];
    ev_timer *in_delivery_timer[3];

    uint8_t  peer_call_type;
    uint8_t  peer_call_ctrl[4];
    uint8_t  ambe_silence[19];

    struct deliv_ctx deliv[3];
};

/* forward declarations */
static void deliver_slot(translator *tr, int ts);
static void on_stream_timeout(translator *tr, int ts);
static void arm_delivery(translator *tr, int ts);

static void delivery_timer_cb(ev_loop *loop, void *ud)
{
    (void)loop;
    struct deliv_ctx *c = ud;
    translator *tr = c->tr; int ts = c->ts;
    tr->in_delivery_timer[ts] = NULL;
    if (!tr->in_has_lc[ts] || !ipsc_has_peers(tr->ip)) return;
    deliver_slot(tr, ts);
}

static void arm_delivery(translator *tr, int ts)
{
    tr->in_delivery_timer[ts] = ev_timer_at(tr->loop, tr->in_next_slot_time[ts],
                                            delivery_timer_cb, &tr->deliv[ts]);
}

static void rand4(uint8_t out[4])
{
    if (getrandom(out, 4, 0) != 4) { out[0]=rand(); out[1]=rand(); out[2]=rand(); out[3]=rand(); }
}

/* precompute 19-byte IPSC AMBE payload of three silence frames */
static void make_ambe_silence(uint8_t out[19])
{
    static const uint8_t sil72_bytes[9] = {0xAC,0xAA,0x40,0x20,0x00,0x44,0x40,0x80,0x80};
    dmr_bit sil72[72]; dmr_bytes_to_bits(sil72_bytes, 9, sil72);
    dmr_bit sil49[49]; dmr_ambe_72_to_49(sil72, sil49);
    dmr_bit bits[152]; memset(bits, 0, sizeof bits);
    memcpy(bits + 0,   sil49, 49);
    memcpy(bits + 50,  sil49, 49);
    memcpy(bits + 100, sil49, 49);
    dmr_bits_to_bytes(bits, 152, out);
}

/* extract 3×72-bit AMBE from a 33-byte DMR frame → 19-byte IPSC AMBE */
static void extract_ambe_from_dmrd(const uint8_t payload33[33], uint8_t out19[19])
{
    dmr_bit burst[264]; dmr_bytes_to_bits(payload33, 33, burst);
    dmr_bit a1_72[72], a2_72[72], a3_72[72];
    memcpy(a1_72, burst + 0, 72);
    memcpy(a2_72, burst + 72, 36);
    memcpy(a2_72 + 36, burst + 156, 36);
    memcpy(a3_72, burst + 192, 72);
    dmr_bit a1_49[49], a2_49[49], a3_49[49];
    dmr_ambe_72_to_49(a1_72, a1_49);
    dmr_ambe_72_to_49(a2_72, a2_49);
    dmr_ambe_72_to_49(a3_72, a3_49);
    dmr_bit bits[152]; memset(bits, 0, sizeof bits);
    memcpy(bits + 0,   a1_49, 49);
    memcpy(bits + 50,  a2_49, 49);
    memcpy(bits + 100, a3_49, 49);
    dmr_bits_to_bytes(bits, 152, out19);
}

/* build the 23-byte VOICE_HEAD/VOICE_TERM payload (after burst-type byte) */
static void build_ipsc_voice_payload(const uint8_t lc[9], int burst_type, uint8_t out[23])
{
    uint8_t fec[3];
    dmr_rs129_lc_encode(lc, burst_type == VOICE_TERM ? 1 : 0, fec);
    uint8_t tag = (burst_type == VOICE_HEAD) ? 0x11 : 0x12;
    int p = 0;
    out[p++] = 0x80;
    out[p++] = 0x00; out[p++] = 0x0a;   /* length_to_follow = 10 */
    out[p++] = 0x80;
    out[p++] = 0x0a;
    out[p++] = 0x00; out[p++] = 0x60;   /* data size = 96 bits */
    memcpy(out + p, lc, 9); p += 9;
    memcpy(out + p, fec, 3); p += 3;
    out[p++] = 0x00; out[p++] = tag; out[p++] = 0x00; out[p++] = 0x00;
}

/* 48-bit EMBED for superframe position 0..5 */
static void build_embed(translator *tr, int ts, int pos, dmr_bit embed[48])
{
    if (pos == 0) { memcpy(embed, DMR_BS_VOICE_SYNC, 48); return; }
    int idx = pos - 1;   /* BURST_B..F */
    memcpy(embed, DMR_EMB[idx], 8);
    if (pos <= 4 && tr->out_has_emb[ts]) {
        dmr_bytes_to_bits(tr->out_emb[ts][pos - 1], 4, embed + 8);  /* 32 bits */
    } else {
        memset(embed + 8, 0, 32);
    }
    memcpy(embed + 40, DMR_EMB[idx] + 8, 8);
}

/* assemble a complete GROUP_VOICE packet; returns length */
static int build_gv(translator *tr, const uint8_t src[3], const uint8_t dst[3],
                    int call_info, const uint8_t rtp_hdr[12],
                    const uint8_t *gv_payload, int gv_len, int stream_id, uint8_t *out)
{
    int p = 0;
    out[p++] = GROUP_VOICE;
    memcpy(out + p, tr->master_id_b, 4); p += 4;
    out[p++] = (uint8_t)stream_id;
    memcpy(out + p, src, 3); p += 3;
    memcpy(out + p, dst, 3); p += 3;
    out[p++] = tr->peer_call_type;
    memcpy(out + p, tr->peer_call_ctrl, 4); p += 4;
    out[p++] = (uint8_t)call_info;
    memcpy(out + p, rtp_hdr, 12); p += 12;
    memcpy(out + p, gv_payload, (size_t)gv_len); p += gv_len;
    return p;
}

/* ---------------- lifecycle ---------------- */

translator *translator_new(const Config *cfg, ev_loop *loop)
{
    translator *tr = calloc(1, sizeof *tr);
    tr->cfg = cfg; tr->loop = loop;
    tr->repeater_id_b[0]=(uint8_t)(cfg->hbp_repeater_id>>24);
    tr->repeater_id_b[1]=(uint8_t)(cfg->hbp_repeater_id>>16);
    tr->repeater_id_b[2]=(uint8_t)(cfg->hbp_repeater_id>>8);
    tr->repeater_id_b[3]=(uint8_t)(cfg->hbp_repeater_id);
    tr->master_id_b[0]=(uint8_t)(cfg->ipsc_master_id>>24);
    tr->master_id_b[1]=(uint8_t)(cfg->ipsc_master_id>>16);
    tr->master_id_b[2]=(uint8_t)(cfg->ipsc_master_id>>8);
    tr->master_id_b[3]=(uint8_t)(cfg->ipsc_master_id);
    tr->out_ipsc_stream_id[1] = tr->out_ipsc_stream_id[2] = -1;
    tr->peer_call_type = 0x02;
    tr->peer_call_ctrl[0]=0x00; tr->peer_call_ctrl[1]=0x00;
    tr->peer_call_ctrl[2]=0x43; tr->peer_call_ctrl[3]=0xe2;
    make_ambe_silence(tr->ambe_silence);
    for (int ts = 0; ts < 3; ts++) { tr->deliv[ts].tr = tr; tr->deliv[ts].ts = ts; }
    return tr;
}

void translator_set_protocols(translator *tr, struct ipsc *ip, struct hbp *hb)
{
    tr->ip = ip; tr->hb = hb;
}

void translator_free(translator *tr)
{
    if (!tr) return;
    for (int ts = 1; ts <= 2; ts++)
        if (tr->in_delivery_timer[ts]) ev_timer_cancel(tr->loop, tr->in_delivery_timer[ts]);
    free(tr);
}

static void cancel_delivery_timer(translator *tr, int ts)
{
    if (tr->in_delivery_timer[ts]) { ev_timer_cancel(tr->loop, tr->in_delivery_timer[ts]); tr->in_delivery_timer[ts] = NULL; }
}

static void init_call_state(translator *tr)
{
    for (int ts = 1; ts <= 2; ts++) cancel_delivery_timer(tr, ts);
    memset(tr->out_has_stream, 0, sizeof tr->out_has_stream);
    tr->out_ipsc_stream_id[1]=tr->out_ipsc_stream_id[2]=-1;
    memset(tr->out_has_lc, 0, sizeof tr->out_has_lc);
    memset(tr->out_has_emb, 0, sizeof tr->out_has_emb);
    tr->out_last_pkt[1]=tr->out_last_pkt[2]=0.0;
    tr->out_ts_mismatch_warned[1]=tr->out_ts_mismatch_warned[2]=0;
    memset(tr->in_has_lc, 0, sizeof tr->in_has_lc);
    memset(tr->in_has_emb, 0, sizeof tr->in_has_emb);
    tr->in_stream_id[1]=tr->in_stream_id[2]=0;
    memset(tr->in_has_hbp_stream, 0, sizeof tr->in_has_hbp_stream);
    tr->in_last_pkt[1]=tr->in_last_pkt[2]=0.0;
    memset(tr->in_buf_present, 0, sizeof tr->in_buf_present);
    tr->in_next_slot_time[1]=tr->in_next_slot_time[2]=0.0;
    tr->in_burst_pos[1]=tr->in_burst_pos[2]=0;
    tr->in_consec_synth[1]=tr->in_consec_synth[2]=0;
    tr->in_delivery_timer[1]=tr->in_delivery_timer[2]=NULL;
}

void translator_peer_joined(translator *tr)
{
    if (!strcmp(tr->cfg->hbp_mode, "TRACKING")) hbp_activate(tr->hb);
}

void translator_peer_lost(translator *tr)
{
    LOGW(LOGN, "IPSC peer lost");
    init_call_state(tr);
    tr->peer_call_type = 0x02;
    tr->peer_call_ctrl[0]=0x00; tr->peer_call_ctrl[1]=0x00;
    tr->peer_call_ctrl[2]=0x43; tr->peer_call_ctrl[3]=0xe2;
    if (!strcmp(tr->cfg->hbp_mode, "TRACKING")) hbp_deactivate(tr->hb);
}

void translator_hbp_connected(translator *tr) { (void)tr; LOGI(LOGN, "HBP connected"); }
void translator_hbp_disconnected(translator *tr) { LOGW(LOGN, "HBP disconnected"); init_call_state(tr); }

/* ---------------- outbound: IPSC -> HBP ---------------- */

void translator_ipsc_voice_received(translator *tr, const uint8_t *data, int len, int ts, int burst_type)
{
    if (!hbp_is_connected(tr->hb)) return;

    int ipsc_stream_id = data[GV_CALL_SEQ_OFF];

    if (burst_type != VOICE_HEAD && burst_type != VOICE_TERM) {
        int ts_ci = (data[GV_CALL_INFO_OFF] & TS_CALL_MSK) ? 2 : 1;
        if (ts_ci != ts) {
            if (!tr->out_ts_mismatch_warned[ts]) {
                LOGW(LOGN, "TS mismatch on SLOT_VOICE ts=%d: burst_type->TS%d, call_info->TS%d — "
                           "DMRlink confbridge does not rewrite burst_type when translating timeslots; "
                           "set ts_prefer_call_info = true in [ipsc] to work around", ts, ts, ts_ci);
                tr->out_ts_mismatch_warned[ts] = 1;
            } else {
                LOGD(LOGN, "TS mismatch: burst_type->TS%d, call_info->TS%d", ts, ts_ci);
            }
            if (tr->cfg->ipsc_ts_prefer_call_info) ts = ts_ci;
        }
    }

    tr->out_last_pkt[ts] = ev_now(tr->loop);

    const uint8_t *src_sub   = data + GV_SRC_SUB_OFF;
    const uint8_t *dst_group = data + GV_DST_GROUP_OFF;
    int flags = (ts == 2) ? HBPF_TGID_TS2 : 0x00;

    if (len >= 17) {
        tr->peer_call_type = data[12];
        memcpy(tr->peer_call_ctrl, data + 13, 4);
    }

    uint8_t payload_33[33];

    if (burst_type == VOICE_HEAD) {
        if (tr->out_has_stream[ts] && tr->out_ipsc_stream_id[ts] >= 0 &&
            tr->out_ipsc_stream_id[ts] != ipsc_stream_id) {
            LOGW(LOGN, "IPSC stream ID changed on ts=%d (0x%02x->0x%02x) at VOICE_HEAD "
                       "— prior call ended without VOICE_TERM, clearing stale state",
                 ts, tr->out_ipsc_stream_id[ts], ipsc_stream_id);
            tr->out_has_stream[ts]=0; tr->out_ipsc_stream_id[ts]=-1;
            tr->out_has_lc[ts]=0; tr->out_has_emb[ts]=0;
        }
        if (!tr->out_has_stream[ts]) {
            rand4(tr->out_stream_id[ts]);
            tr->out_has_stream[ts] = 1;
            tr->out_ipsc_stream_id[ts] = ipsc_stream_id;
            LOGI(LOGN, "IPSC call start: src=%u  tg=%u  ts=%d  stream=%s  ipsc_id=0x%02x",
                 (unsigned)(src_sub[0]<<16|src_sub[1]<<8|src_sub[2]),
                 (unsigned)(dst_group[0]<<16|dst_group[1]<<8|dst_group[2]),
                 ts, log_hex(tr->out_stream_id[ts], 4), ipsc_stream_id);
        } else {
            LOGD(LOGN, "Duplicate VOICE_HEAD ts=%d — keeping stream=%s", ts, log_hex(tr->out_stream_id[ts], 4));
        }
        tr->out_frame_pos[ts] = 0;
        uint8_t lc[9]; memcpy(lc, DMR_LC_OPT, 3); memcpy(lc+3, dst_group, 3); memcpy(lc+6, src_sub, 3);
        memcpy(tr->out_lc[ts], lc, 9); tr->out_has_lc[ts]=1;
        dmr_encode_emblc(lc, tr->out_emb[ts]); tr->out_has_emb[ts]=1;
        dmr_bit full_lc[196]; dmr_bptc_encode_lc(lc, 0, full_lc);
        dmr_bit fb[264]; int at=0;
        memcpy(fb+at, full_lc, 98); at+=98;
        memcpy(fb+at, DMR_SLOT_TYPE_VHEAD, 10); at+=10;
        memcpy(fb+at, DMR_BS_DATA_SYNC, 48); at+=48;
        memcpy(fb+at, DMR_SLOT_TYPE_VHEAD+10, 10); at+=10;
        memcpy(fb+at, full_lc+98, 98); at+=98;
        dmr_bits_to_bytes(fb, 264, payload_33);
        flags |= HBPF_FRAMETYPE_DATASYNC | HBPF_SLT_VHEAD;
    } else if (burst_type == VOICE_TERM) {
        if (!tr->out_has_stream[ts]) return;
        uint8_t lc[9];
        if (tr->out_has_lc[ts]) memcpy(lc, tr->out_lc[ts], 9);
        else { memcpy(lc, DMR_LC_OPT, 3); memcpy(lc+3, dst_group, 3); memcpy(lc+6, src_sub, 3); }
        dmr_bit full_lc[196]; dmr_bptc_encode_lc(lc, 1, full_lc);
        dmr_bit fb[264]; int at=0;
        memcpy(fb+at, full_lc, 98); at+=98;
        memcpy(fb+at, DMR_SLOT_TYPE_VTERM, 10); at+=10;
        memcpy(fb+at, DMR_BS_DATA_SYNC, 48); at+=48;
        memcpy(fb+at, DMR_SLOT_TYPE_VTERM+10, 10); at+=10;
        memcpy(fb+at, full_lc+98, 98); at+=98;
        dmr_bits_to_bytes(fb, 264, payload_33);
        flags |= HBPF_FRAMETYPE_DATASYNC | HBPF_SLT_VTERM;
    } else {
        /* SLOT1_VOICE / SLOT2_VOICE */
        if (tr->out_has_stream[ts] && tr->out_ipsc_stream_id[ts] >= 0 &&
            tr->out_ipsc_stream_id[ts] != ipsc_stream_id) {
            LOGW(LOGN, "IPSC stream ID changed on ts=%d (0x%02x->0x%02x) mid-stream "
                       "— prior call ended without VOICE_TERM, clearing stale state",
                 ts, tr->out_ipsc_stream_id[ts], ipsc_stream_id);
            tr->out_has_stream[ts]=0; tr->out_ipsc_stream_id[ts]=-1;
            tr->out_has_lc[ts]=0; tr->out_has_emb[ts]=0;
        }
        if (!tr->out_has_stream[ts]) {
            if (len <= 32 || data[32] != 0x16) return;   /* only Burst E gives late entry */
            uint8_t lc[9]; memcpy(lc, DMR_LC_OPT, 3); memcpy(lc+3, dst_group, 3); memcpy(lc+6, src_sub, 3);
            rand4(tr->out_stream_id[ts]); tr->out_has_stream[ts]=1;
            tr->out_ipsc_stream_id[ts]=ipsc_stream_id;
            memcpy(tr->out_lc[ts], lc, 9); tr->out_has_lc[ts]=1;
            dmr_encode_emblc(lc, tr->out_emb[ts]); tr->out_has_emb[ts]=1;
            tr->out_frame_pos[ts]=4;
            LOGI(LOGN, "IPSC late entry: ts=%d src=%u tg=%u — LC from Burst E, stream=%s  ipsc_id=0x%02x",
                 ts, (unsigned)(src_sub[0]<<16|src_sub[1]<<8|src_sub[2]),
                 (unsigned)(dst_group[0]<<16|dst_group[1]<<8|dst_group[2]),
                 log_hex(tr->out_stream_id[ts], 4), ipsc_stream_id);
        }
        if (len < 52) { LOGW(LOGN, "SLOT_VOICE too short for AMBE: %d bytes", len); return; }

        dmr_bit raw[152]; dmr_bytes_to_bits(data + 33, 19, raw);
        dmr_bit a1_72[72], a2_72[72], a3_72[72];
        dmr_ambe_49_to_72(raw + 0,   a1_72);
        dmr_ambe_49_to_72(raw + 50,  a2_72);
        dmr_ambe_49_to_72(raw + 100, a3_72);
        int pos = tr->out_frame_pos[ts] % 6;
        dmr_bit embed[48]; build_embed(tr, ts, pos, embed);
        dmr_bit fb[264]; int at=0;
        memcpy(fb+at, a1_72, 72); at+=72;
        memcpy(fb+at, a2_72, 36); at+=36;
        memcpy(fb+at, embed, 48); at+=48;
        memcpy(fb+at, a2_72+36, 36); at+=36;
        memcpy(fb+at, a3_72, 72); at+=72;
        dmr_bits_to_bytes(fb, 264, payload_33);
        flags |= (pos == 0) ? HBPF_FRAMETYPE_VOICESYNC : (HBPF_FRAMETYPE_VOICE | pos);
        tr->out_frame_pos[ts]++;
    }

    uint8_t dmrd[DMRD_LEN];
    int p = 0;
    memcpy(dmrd+p, "DMRD", 4); p+=4;
    dmrd[p++] = (uint8_t)tr->out_seq;
    memcpy(dmrd+p, src_sub, 3); p+=3;
    memcpy(dmrd+p, dst_group, 3); p+=3;
    memcpy(dmrd+p, tr->repeater_id_b, 4); p+=4;
    dmrd[p++] = (uint8_t)flags;
    memcpy(dmrd+p, tr->out_stream_id[ts], 4); p+=4;
    memcpy(dmrd+p, payload_33, 33); p+=33;
    dmrd[p++] = 0x00; dmrd[p++] = 0x00;   /* BER + RSSI */
    tr->out_seq = (tr->out_seq + 1) & 0xFF;
    hbp_send_dmrd(tr->hb, dmrd, p);

    if (burst_type == VOICE_TERM) {
        LOGI(LOGN, "IPSC call end:   src=%u  tg=%u  ts=%d  stream=%s  ipsc_id=0x%02x",
             (unsigned)(src_sub[0]<<16|src_sub[1]<<8|src_sub[2]),
             (unsigned)(dst_group[0]<<16|dst_group[1]<<8|dst_group[2]),
             ts, log_hex(tr->out_stream_id[ts], 4), tr->out_ipsc_stream_id[ts]);
        tr->out_has_stream[ts]=0; tr->out_ipsc_stream_id[ts]=-1;
        tr->out_has_lc[ts]=0; tr->out_has_emb[ts]=0;
    }
}

/* ---------------- inbound: HBP -> IPSC ---------------- */

static int build_slot_voice_payload(translator *tr, int ts, int pos, const uint8_t ambe_19[19], uint8_t *out)
{
    uint8_t slot_burst = (ts == 2) ? SLOT2_VOICE : SLOT1_VOICE;
    const uint8_t *lc = tr->in_lc[ts];
    int p = 0;
    if (pos == 0) {
        out[p++]=slot_burst; out[p++]=0x14; out[p++]=0x40;
        memcpy(out+p, ambe_19, 19); p+=19;
    } else if (pos == 4) {
        out[p++]=slot_burst; out[p++]=0x22; out[p++]=0x16;
        memcpy(out+p, ambe_19, 19); p+=19;
        if (tr->in_has_emb[ts]) memcpy(out+p, tr->in_emb[ts][3], 4);  /* key 4 -> emb[3] */
        else memset(out+p, 0, 4);
        p+=4;
        memcpy(out+p, lc+0, 3); p+=3;
        memcpy(out+p, lc+3, 3); p+=3;
        memcpy(out+p, lc+6, 3); p+=3;
        out[p++]=0x14;
    } else if (pos == 5) {
        out[p++]=slot_burst; out[p++]=0x19; out[p++]=0x06;
        memcpy(out+p, ambe_19, 19); p+=19;
        out[p++]=0x00; out[p++]=0x00; out[p++]=0x00; out[p++]=0x00; out[p++]=0x10;
    } else {
        out[p++]=slot_burst; out[p++]=0x19; out[p++]=0x06;
        memcpy(out+p, ambe_19, 19); p+=19;
        if (tr->in_has_emb[ts]) memcpy(out+p, tr->in_emb[ts][pos-1], 4);
        else memset(out+p, 0, 4);
        p+=4;
        uint8_t emb_hdr = 0;
        dmr_bit eh[8]; memcpy(eh, DMR_EMB[pos-1], 8);
        for (int i=0;i<8;i++) emb_hdr = (uint8_t)((emb_hdr<<1)|eh[i]);
        out[p++] = emb_hdr & 0xFE;
    }
    return p;
}

static void on_stream_timeout(translator *tr, int ts);

static void deliver_slot(translator *tr, int ts)
{
    int pos = tr->in_burst_pos[ts];
    const uint8_t *ambe_19;
    if (tr->in_buf_present[ts][pos]) {
        ambe_19 = tr->in_buf[ts][pos];
        tr->in_buf_present[ts][pos] = 0;
        tr->in_consec_synth[ts] = 0;
    } else {
        ambe_19 = tr->ambe_silence;
        tr->in_consec_synth[ts]++;
        LOGD(LOGN, "^ SYNTH  ts=%d  %c  consec=%d", ts, "ABCDEF"[pos], tr->in_consec_synth[ts]);
        if (tr->in_consec_synth[ts] >= MAX_SYNTH_BURSTS) { on_stream_timeout(tr, ts); return; }
    }

    const uint8_t *lc = tr->in_lc[ts];
    int call_info = (ts == 2) ? TS_CALL_MSK : 0x00;
    const uint8_t *dst = lc + 3, *src = lc + 6;

    uint8_t gv_payload[64];
    int gv_len = build_slot_voice_payload(tr, ts, pos, ambe_19, gv_payload);

    tr->in_rtp_ts[ts] = (tr->in_rtp_ts[ts] + 480) & 0xFFFFFFFF;
    uint8_t rtp_hdr[12] = {0x80, 0x5d};
    rtp_hdr[2]=(uint8_t)(tr->in_rtp_seq[ts]>>8); rtp_hdr[3]=(uint8_t)(tr->in_rtp_seq[ts]&0xFF);
    rtp_hdr[4]=(uint8_t)(tr->in_rtp_ts[ts]>>24); rtp_hdr[5]=(uint8_t)(tr->in_rtp_ts[ts]>>16);
    rtp_hdr[6]=(uint8_t)(tr->in_rtp_ts[ts]>>8); rtp_hdr[7]=(uint8_t)(tr->in_rtp_ts[ts]);
    tr->in_rtp_seq[ts]++;

    uint8_t gv[128];
    int n = build_gv(tr, src, dst, call_info, rtp_hdr, gv_payload, gv_len, tr->in_stream_id[ts], gv);
    ipsc_send_voice(tr->ip, gv, n);

    tr->in_burst_pos[ts] = (pos + 1) % 6;
    tr->in_next_slot_time[ts] += SLOT_MS;
    arm_delivery(tr, ts);
}

static void on_stream_timeout(translator *tr, int ts)
{
    LOGW(LOGN, "HBP->IPSC stream timeout ts=%d: %d consecutive silence bursts — synthesizing VOICE_TERM",
         ts, MAX_SYNTH_BURSTS);
    const uint8_t *lc = tr->in_lc[ts];
    int call_info = ((ts == 2) ? TS_CALL_MSK : 0x00) | END_MSK;
    const uint8_t *dst = lc + 3, *src = lc + 6;
    uint8_t gv_payload[24]; gv_payload[0]=VOICE_TERM;
    build_ipsc_voice_payload(lc, VOICE_TERM, gv_payload+1);
    uint8_t rtp_hdr[12] = {0x80, 0x5e};
    rtp_hdr[2]=(uint8_t)(tr->in_rtp_seq[ts]>>8); rtp_hdr[3]=(uint8_t)(tr->in_rtp_seq[ts]&0xFF);
    rtp_hdr[4]=(uint8_t)(tr->in_rtp_ts[ts]>>24); rtp_hdr[5]=(uint8_t)(tr->in_rtp_ts[ts]>>16);
    rtp_hdr[6]=(uint8_t)(tr->in_rtp_ts[ts]>>8); rtp_hdr[7]=(uint8_t)(tr->in_rtp_ts[ts]);
    tr->in_rtp_seq[ts]++;
    uint8_t gv[128];
    int n = build_gv(tr, src, dst, call_info, rtp_hdr, gv_payload, 24, tr->in_stream_id[ts], gv);
    ipsc_send_voice(tr->ip, gv, n);
    memset(tr->in_buf_present[ts], 0, sizeof tr->in_buf_present[ts]);
    tr->in_next_slot_time[ts]=0.0; tr->in_burst_pos[ts]=0; tr->in_consec_synth[ts]=0;
    tr->in_delivery_timer[ts]=NULL;
    tr->in_has_hbp_stream[ts]=0; tr->in_has_lc[ts]=0; tr->in_has_emb[ts]=0;
}

void translator_hbp_voice_received(translator *tr, const uint8_t *dmrd, int len)
{
    if (!ipsc_has_peers(tr->ip)) return;
    if (len < DMRD_LEN) return;

    const uint8_t *src_sub   = dmrd + DMRD_SRC_OFF;
    const uint8_t *dst_group = dmrd + DMRD_DST_OFF;
    int flags = dmrd[DMRD_FLAGS_OFF];
    const uint8_t *hbp_stream = dmrd + 16;
    const uint8_t *payload_33 = dmrd + DMRD_PAYLOAD_OFF;

    int ts = (flags & HBPF_TGID_TS2) ? 2 : 1;
    tr->in_last_pkt[ts] = ev_now(tr->loop);
    int frame_type = flags & HBPF_FRAMETYPE_MASK;
    int dtype = flags & HBPF_DTYPE_MASK;
    int call_info = (ts == 2) ? TS_CALL_MSK : 0x00;

    int rtp_pt = 0;
    uint8_t gv_payload[24];
    int do_send = 0;

    if (frame_type == HBPF_FRAMETYPE_DATASYNC && dtype == HBPF_SLT_VHEAD) {
        dmr_bit fb[264]; dmr_bytes_to_bits(payload_33, 33, fb);
        dmr_bit bptc_bits[196];
        memcpy(bptc_bits, fb, 98);
        memcpy(bptc_bits + 98, fb + 166, 98);
        uint8_t lc[9]; dmr_bptc_decode_full_lc(bptc_bits, lc);
        memcpy(tr->in_lc[ts], lc, 9); tr->in_has_lc[ts]=1;
        dmr_encode_emblc(lc, tr->in_emb[ts]); tr->in_has_emb[ts]=1;
        if (tr->in_has_hbp_stream[ts] && memcmp(hbp_stream, tr->in_hbp_stream[ts], 4) == 0) {
            LOGD(LOGN, "Duplicate VOICE_HEAD ts=%d stream=%s — forwarding, reusing stream_id=0x%02x",
                 ts, log_hex(hbp_stream, 4), tr->in_stream_id[ts]);
        } else {
            memcpy(tr->in_hbp_stream[ts], hbp_stream, 4); tr->in_has_hbp_stream[ts]=1;
            tr->in_stream_ctr = (tr->in_stream_ctr + 1) & 0xFF;
            tr->in_stream_id[ts] = tr->in_stream_ctr;
            cancel_delivery_timer(tr, ts);
            memset(tr->in_buf_present[ts], 0, sizeof tr->in_buf_present[ts]);
            tr->in_burst_pos[ts]=0; tr->in_consec_synth[ts]=0; tr->in_next_slot_time[ts]=0.0;
            LOGI(LOGN, "HBP call start: src=%u  tg=%u  ts=%d  stream=%s  ipsc_id=0x%02x",
                 (unsigned)(src_sub[0]<<16|src_sub[1]<<8|src_sub[2]),
                 (unsigned)(dst_group[0]<<16|dst_group[1]<<8|dst_group[2]),
                 ts, log_hex(hbp_stream, 4), tr->in_stream_id[ts]);
        }
        gv_payload[0]=VOICE_HEAD; build_ipsc_voice_payload(lc, VOICE_HEAD, gv_payload+1);
        rtp_pt = 0xdd; do_send = 1;
    } else if (frame_type == HBPF_FRAMETYPE_DATASYNC && dtype == HBPF_SLT_VTERM) {
        uint8_t lc[9];
        if (tr->in_has_lc[ts]) memcpy(lc, tr->in_lc[ts], 9);
        else { memcpy(lc, DMR_LC_OPT, 3); memcpy(lc+3, dst_group, 3); memcpy(lc+6, src_sub, 3); }
        call_info |= END_MSK;
        cancel_delivery_timer(tr, ts);
        memset(tr->in_buf_present[ts], 0, sizeof tr->in_buf_present[ts]);
        tr->in_next_slot_time[ts]=0.0; tr->in_burst_pos[ts]=0; tr->in_consec_synth[ts]=0;
        gv_payload[0]=VOICE_TERM; build_ipsc_voice_payload(lc, VOICE_TERM, gv_payload+1);
        rtp_pt = 0x5e; do_send = 1;
    } else {
        /* VOICESYNC (burst A) or VOICE (B-F) */
        if (tr->in_has_lc[ts] && tr->in_has_hbp_stream[ts] &&
            memcmp(tr->in_hbp_stream[ts], hbp_stream, 4) != 0) {
            LOGW(LOGN, "HBP stream ID changed on ts=%d (%s->%s) — prior call ended without "
                       "VOICE_TERM, clearing stale state",
                 ts, log_hex(tr->in_hbp_stream[ts], 4), log_hex(hbp_stream, 4));
            cancel_delivery_timer(tr, ts);
            tr->in_has_lc[ts]=0; tr->in_has_emb[ts]=0; tr->in_has_hbp_stream[ts]=0;
            tr->in_next_slot_time[ts]=0.0; tr->in_burst_pos[ts]=0; tr->in_consec_synth[ts]=0;
        }
        if (!tr->in_has_lc[ts]) {
            uint8_t lc[9]; memcpy(lc, DMR_LC_OPT, 3); memcpy(lc+3, dst_group, 3); memcpy(lc+6, src_sub, 3);
            memcpy(tr->in_lc[ts], lc, 9); tr->in_has_lc[ts]=1;
            dmr_encode_emblc(lc, tr->in_emb[ts]); tr->in_has_emb[ts]=1;
            memcpy(tr->in_hbp_stream[ts], hbp_stream, 4); tr->in_has_hbp_stream[ts]=1;
            tr->in_stream_ctr = (tr->in_stream_ctr + 1) & 0xFF;
            tr->in_stream_id[ts] = tr->in_stream_ctr;
            LOGI(LOGN, "HBP late entry: ts=%d src=%u tg=%u — LC from stream, hbp_stream=%s",
                 ts, (unsigned)(src_sub[0]<<16|src_sub[1]<<8|src_sub[2]),
                 (unsigned)(dst_group[0]<<16|dst_group[1]<<8|dst_group[2]), log_hex(hbp_stream, 4));
        }
        int cur_pos;
        if (frame_type == HBPF_FRAMETYPE_VOICESYNC) cur_pos = 0;
        else if (dtype == 4) cur_pos = 4;
        else if (dtype >= 5) cur_pos = 5;
        else cur_pos = dtype < 1 ? 1 : dtype;   /* max(dtype,1) */

        extract_ambe_from_dmrd(payload_33, tr->in_buf[ts][cur_pos]);
        tr->in_buf_present[ts][cur_pos] = 1;

        if (tr->in_delivery_timer[ts] == NULL && tr->in_next_slot_time[ts] == 0.0) {
            tr->in_burst_pos[ts] = cur_pos;
            tr->in_consec_synth[ts] = 0;
            tr->in_next_slot_time[ts] = ev_now(tr->loop) + JITTER_BUFFER_DEPTH * SLOT_MS;
            arm_delivery(tr, ts);
        }
        return;
    }

    if (do_send) {
        uint8_t rtp_hdr[12] = {0x80, (uint8_t)rtp_pt};
        rtp_hdr[2]=(uint8_t)(tr->in_rtp_seq[ts]>>8); rtp_hdr[3]=(uint8_t)(tr->in_rtp_seq[ts]&0xFF);
        rtp_hdr[4]=(uint8_t)(tr->in_rtp_ts[ts]>>24); rtp_hdr[5]=(uint8_t)(tr->in_rtp_ts[ts]>>16);
        rtp_hdr[6]=(uint8_t)(tr->in_rtp_ts[ts]>>8); rtp_hdr[7]=(uint8_t)(tr->in_rtp_ts[ts]);
        tr->in_rtp_seq[ts]++;
        uint8_t gv[128];
        int n = build_gv(tr, src_sub, dst_group, call_info, rtp_hdr, gv_payload, 24, tr->in_stream_id[ts], gv);
        ipsc_send_voice(tr->ip, gv, n);

        if (frame_type == HBPF_FRAMETYPE_DATASYNC && dtype == HBPF_SLT_VTERM) {
            LOGI(LOGN, "HBP call end:   src=%u  tg=%u  ts=%d  stream=%s  ipsc_id=0x%02x",
                 (unsigned)(src_sub[0]<<16|src_sub[1]<<8|src_sub[2]),
                 (unsigned)(dst_group[0]<<16|dst_group[1]<<8|dst_group[2]),
                 ts, log_hex(hbp_stream, 4), tr->in_stream_id[ts]);
            tr->in_has_lc[ts]=0; tr->in_has_emb[ts]=0; tr->in_has_hbp_stream[ts]=0;
        }
    }
}

void translator_check_call_timeouts(translator *tr)
{
    double timeout = 10.0;
    double now = ev_now(tr->loop);
    for (int ts = 1; ts <= 2; ts++) {
        if (tr->out_has_stream[ts]) {
            double el = now - tr->out_last_pkt[ts];
            if (el > timeout) {
                LOGW(LOGN, "IPSC->HBP call timeout: ts=%d stream=%s — no voice for %.1fs, clearing",
                     ts, log_hex(tr->out_stream_id[ts], 4), el);
                tr->out_has_stream[ts]=0; tr->out_ipsc_stream_id[ts]=-1;
                tr->out_has_lc[ts]=0; tr->out_has_emb[ts]=0;
            }
        }
        if (tr->in_has_lc[ts]) {
            double el = now - tr->in_last_pkt[ts];
            if (el > timeout) {
                LOGW(LOGN, "HBP->IPSC call timeout: ts=%d — no voice for %.1fs, clearing", ts, el);
                cancel_delivery_timer(tr, ts);
                memset(tr->in_buf_present[ts], 0, sizeof tr->in_buf_present[ts]);
                tr->in_has_lc[ts]=0; tr->in_has_emb[ts]=0; tr->in_has_hbp_stream[ts]=0;
                tr->in_next_slot_time[ts]=0.0; tr->in_burst_pos[ts]=0; tr->in_consec_synth[ts]=0;
            }
        }
    }
}
