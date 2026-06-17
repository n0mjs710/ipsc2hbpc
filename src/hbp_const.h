/* hbp_const.h — HomeBrew Protocol magic strings, flags, and layout (hbp/const.py). */
#ifndef HBP_CONST_H
#define HBP_CONST_H

/* DMRD flags byte (byte 15) */
#define HBPF_TGID_TS2          0x80   /* bit 7: timeslot 2 */
#define HBPF_TGID_CALL_P       0x40   /* bit 6: private call */
#define HBPF_FRAMETYPE_VOICE     0x00
#define HBPF_FRAMETYPE_VOICESYNC 0x10
#define HBPF_FRAMETYPE_DATASYNC  0x20
#define HBPF_SLT_VHEAD         0x01
#define HBPF_SLT_VTERM         0x02
#define HBPF_FRAMETYPE_MASK    0x30
#define HBPF_DTYPE_MASK        0x0F

/* DMRD packet layout */
#define DMRD_LEN          55
#define DMRD_SEQ_OFF      4
#define DMRD_SRC_OFF      5
#define DMRD_DST_OFF      8
#define DMRD_RPTR_OFF     11
#define DMRD_FLAGS_OFF    15
#define DMRD_STREAM_OFF   16
#define DMRD_PAYLOAD_OFF  20
#define DMRD_BER_OFF      53
#define DMRD_RSSI_OFF     54

/* RPTC config blob */
#define RPTC_LEN          302
#define RPTACK_NONCE_OFF  6
#define MSTPONG_ID_OFF    7

#endif
