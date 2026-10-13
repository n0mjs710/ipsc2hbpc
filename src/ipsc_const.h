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
#define GV_AMBE_OFF       33   /* 19-byte AMBE field: three 49-bit frames at bits 0, 50, 100 */
/* Repeater-set "bad voice" flags, one per AMBE frame (uncorrectable on RF receive):
 * frame 1 = data[32] bit 0; frames 2/3 = bits 49/99 of the AMBE field. */
#define GV_BAD1_OFF       32
#define GV_SYNC_OFF       32   /* data[32] & GV_SYNC_MSK: burst carries voice sync (burst A) */
#define GV_SYNC_MSK       0x40
#define GV_BAD1_MSK       0x01
#define GV_BAD2_BIT       49
#define GV_BAD3_BIT       99
#define GV_BE_FLAG        0x16 /* data[32] value identifying burst E (carries reassembled LC repeat) */
#define GV_BE_LC_FLCO_OFF 56   /* FLCO of the reassembled LC repeat carried only on burst E */
#define FLCO_GROUP        0x00 /* Group Voice Channel User (real call identity); 0x04-0x08 = TA/GPS */
#define FLCO_PRIVATE      0x03 /* Unit-to-Unit Voice Channel User (private call) */

#define AUTH_DIGEST_LEN   10

#endif
