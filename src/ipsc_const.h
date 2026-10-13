/* ipsc_const.h — IPSC opcodes, masks, and GROUP_VOICE offsets (ipsc/const.py). */
#ifndef IPSC_CONST_H
#define IPSC_CONST_H

/* Opcodes (from DMRlink ipsc/ipsc_const.py) */
#define CALL_CONFIRMATION  0x05
#define TXT_MESSAGE_ACK    0x54
#define CALL_MON_STATUS    0x61
#define CALL_MON_RPT       0x62
#define REPEATER_BLOCKED   0x63
#define XCMP_XNL           0x70   /* NEVER TOUCH */
#define GROUP_VOICE        0x80
#define PVT_VOICE          0x81
#define GROUP_DATA         0x83
#define PVT_DATA           0x84
#define RPT_WAKE_UP        0x85
#define CALL_INTERRUPT_REQ 0x86
#define MASTER_REG_REQ     0x90
#define MASTER_REG_REPLY   0x91
#define PEER_LIST_REQ      0x92
#define PEER_LIST_REPLY    0x93
#define PEER_REG_REQ       0x94
#define PEER_REG_REPLY     0x95
#define MASTER_ALIVE_REQ   0x96
#define MASTER_ALIVE_REPLY 0x97
#define PEER_ALIVE_REQ     0x98
#define PEER_ALIVE_REPLY   0x99
#define DE_REG_REQ         0x9A
#define DE_REG_REPLY       0x9B
#define SYSTEM_MAP_REQ     0x9C
#define SYSTEM_MAP_REPLY   0x9D
#define UNKNOWN_9E         0x9E
#define WIRELINE           0xB2
#define REMOTE_PROG_REQ    0xE0
#define REMOTE_PROG_REPLY  0xE1
#define OPCODE_0xF0        0xF0

/* Burst data type byte values (timeslot encoded inside) */
#define VOICE_HEAD  0x01
#define VOICE_TERM  0x02
#define SLOT1_VOICE 0x0A
#define SLOT2_VOICE 0x8A
#define SYNC_UNDETECT 0x13 /* repeater lost this burst (no sync): 34-byte placeholder, TS in bit 7 */

/* GROUP_VOICE byte 17 (call_info) masks */
#define TS_CALL_MSK 0x20   /* bit 5: 1=TS2 */
#define END_MSK     0x40   /* bit 6: call end */

/* GROUP_VOICE field offsets */
#define GV_PEER_ID_OFF    1
#define GV_CALL_SEQ_OFF   5    /* normally constant within a call, but current XPR8400 firmware mints a
                                  new value every superframe when Talker Alias is interleaved — NOT a
                                  call boundary; anchor on RTP-timestamp continuity instead */
#define GV_SRC_SUB_OFF    6    /* alias bytes on a Talker Alias superframe */
#define GV_DST_GROUP_OFF  9    /* alias bytes on a Talker Alias superframe */
#define GV_CALL_INFO_OFF  17
#define GV_RTP_TS_OFF     22   /* RTP timestamp (8 kHz, +480/burst) — per-call continuity anchor */
#define GV_BURST_TYPE_OFF 30
#define GV_PAYLOAD_OFF    31
#define GV_MIN_LEN        31
/* Voice burst control byte (data[32]); bit names after node-dmr-lib DMRVoicePayload. */
#define GV_CTRL_OFF       32
#define GV_CTRL_LC_ERR    0x80 /* embedded LC failed its check (burst E LC is corrupt) */
#define GV_CTRL_SYNC      0x40 /* burst carries voice sync (burst A) */
#define GV_CTRL_EMB_LC    0x10 /* reassembled embedded LC present (burst E) */
#define GV_CTRL_BAD1      0x01 /* AMBE frame 1 uncorrectable on RF receive */
#define GV_AMBE_OFF       33   /* 19-byte AMBE field: three 49-bit frames at bits 0, 50, 100 */
#define GV_BAD2_BIT       49   /* AMBE field bit: frame 2 uncorrectable */
#define GV_BAD3_BIT       99   /* AMBE field bit: frame 3 uncorrectable */
/* VOICE_HEAD / VOICE_TERM payload (after node-dmr-lib DMRDataPayload) */
#define GV_RSSI_FLAG_OFF  34   /* bit 7: RSSI present */
#define GV_RSSI_FLAG_MSK  0x80
#define GV_SLOTTYPE_OFF   35   /* bit 3: 2-byte slot type follows the data */
#define GV_SLOTTYPE_MSK   0x08
#define GV_DATA_BITS_OFF  36   /* 16-bit data length in bits */
#define GV_DATA_OFF       38   /* data, then slot type, then 16-bit RSSI */
#define GV_BE_LC_FLCO_OFF 56   /* FLCO of the reassembled LC repeat carried only on burst E */
#define FLCO_GROUP        0x00 /* Group Voice Channel User (real call identity); 0x04-0x08 = TA/GPS */
#define FLCO_PRIVATE      0x03 /* Unit-to-Unit Voice Channel User (private call) */

#define AUTH_DIGEST_LEN   10

#endif
