// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Pocket BLE link protocol (docs/pocket/README.md, "BLE link"): message
// codec, PDU fragmentation and reassembly. Pure C, no ESP-IDF or LVGL
// dependency; covered by host tests.
//
// PDU on the wire (one ATT notification or write):
//   [type u8][flags u8][len u16 LE][payload: len bytes]
//   flags bit0 = more fragments follow.
// A message larger than one ATT payload is split into consecutive PDUs of the
// same type; the receiver appends payloads until a PDU without the "more" bit.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POCKET_PROTO_MAJOR 1
#define POCKET_PROTO_MINOR 2  // 1.1 adds WORK, 1.2 the APP_STATE language

#define POCKET_PDU_HEADER_BYTES 4
#define POCKET_PDU_FLAG_MORE    0x01u

// ATT notification/write value = ATT_MTU - 3 (Bluetooth Core, ATT PDU header).
#define POCKET_ATT_HEADER_BYTES 3

typedef enum {
    // Device -> phone.
    POCKET_MSG_INFO        = 0x01,
    POCKET_MSG_PRESS_START = 0x02,
    POCKET_MSG_AUDIO       = 0x03,
    POCKET_MSG_PRESS_END   = 0x04,
    POCKET_MSG_STATUS      = 0x05,
    POCKET_MSG_KEEPALIVE   = 0x06,
    // Phone -> device.
    POCKET_MSG_RESULT      = 0x81,
    POCKET_MSG_REPLY       = 0x82,
    POCKET_MSG_REPLY_DONE  = 0x83,  // reply_id 0: the phone stopped waiting
    POCKET_MSG_APP_STATE   = 0x84,
    POCKET_MSG_WORK        = 0x85,  // brain work phase while a reply is pending
} pocket_msg_type_t;

typedef enum {
    POCKET_RESULT_TRANSCRIBED     = 0,
    POCKET_RESULT_EMPTY           = 1,
    POCKET_RESULT_ASR_FAILED      = 2,
    POCKET_RESULT_SEND_FAILED     = 3,
    POCKET_RESULT_NO_SERVER       = 4,
} pocket_result_code_t;

// WORK phase (docs/pocket/README.md, "Work status"). Values above
// POCKET_WORK_TOOL come from a later minor and are shown as thinking.
typedef enum {
    POCKET_WORK_IDLE     = 0,   // the brain turn ended
    POCKET_WORK_RECEIVED = 1,   // the phone started waiting for the reply
    POCKET_WORK_THINKING = 2,
    POCKET_WORK_TOOL     = 3,
} pocket_work_phase_t;

typedef enum {
    POCKET_APP_OK                 = 0,
    POCKET_APP_SERVER_UNREACHABLE = 1,
    POCKET_APP_PROTO_MISMATCH     = 2,  // phone refuses our proto_major
} pocket_app_state_t;

// UI language in APP_STATE (protocol 1.2). Other values are ignored.
typedef enum {
    POCKET_LANG_ZH_HANS = 0,
    POCKET_LANG_EN      = 1,
    POCKET_LANG_COUNT,
} pocket_lang_t;

// Battery/charging value when the board cannot measure it (this board has no
// charge-status signal; docs/pocket/README.md, "BLE link").
#define POCKET_UNKNOWN_U8 0xFFu

// INFO payload. fw_version is sent length-prefixed, [len u8][bytes], so the
// fields after it can be found (docs/pocket/README.md, "BLE link").
typedef struct {
    const char *fw_version;   // UTF-8, at most 255 bytes are sent
    uint8_t battery_pct;      // 0..100 or POCKET_UNKNOWN_U8
    uint8_t charging;         // 0/1 or POCKET_UNKNOWN_U8
    uint8_t preroll_on;       // 0/1
} pocket_info_t;

// Message payload builders. Each returns the payload length written to out,
// or 0 if cap is too small. The message type is not part of the payload.
size_t pocket_msg_info(uint8_t *out, size_t cap, const pocket_info_t *info);
size_t pocket_msg_press_start(uint8_t *out, size_t cap, uint16_t press_id);
size_t pocket_msg_audio(uint8_t *out, size_t cap, uint16_t press_id, uint16_t seq,
                        const uint8_t *packet, size_t packet_len);
size_t pocket_msg_press_end(uint8_t *out, size_t cap, uint16_t press_id, uint16_t packets);
size_t pocket_msg_status(uint8_t *out, size_t cap, uint8_t battery_pct, uint8_t charging);
size_t pocket_msg_keepalive(uint8_t *out, size_t cap, uint16_t press_id);

// Parsed phone -> device message. Text points into the parsed payload and is
// not NUL-terminated; text_len may be 0.
typedef struct {
    pocket_msg_type_t type;
    uint16_t press_id;        // RESULT
    uint8_t code;             // RESULT code, APP_STATE value, WORK phase
    bool has_lang;            // APP_STATE carried a language byte (1.2)
    uint8_t lang;             // APP_STATE language, unchecked (pocket_lang_t)
    uint32_t reply_id;        // REPLY, REPLY_DONE
    bool final;               // REPLY
    const uint8_t *text;      // RESULT, REPLY, WORK label
    size_t text_len;
} pocket_downlink_t;

// Returns false for unknown types or payloads shorter than the fixed fields.
// Extra bytes after the fixed fields of REPLY_DONE/APP_STATE are ignored so a
// later minor version can append fields. APP_STATE's language byte (1.2) is
// optional: a 1.0/1.1 phone sends the state alone.
bool pocket_msg_parse_downlink(uint8_t type, const uint8_t *payload, size_t len,
                               pocket_downlink_t *out);

// Largest PDU payload for a negotiated ATT MTU; 0 if the MTU cannot carry one
// byte of payload.
size_t pocket_pdu_chunk_max(uint16_t att_mtu);

// Fragmenter: iterate PDUs for one message.
typedef struct {
    uint8_t type;
    const uint8_t *payload;
    size_t len;
    size_t offset;
    size_t chunk;             // payload bytes per PDU
    bool done;
} pocket_frag_t;

// chunk = pocket_pdu_chunk_max(mtu). Returns false if chunk is 0.
bool pocket_frag_init(pocket_frag_t *f, uint8_t type, const uint8_t *payload, size_t len,
                      size_t chunk);
// Writes the next PDU into out (cap >= chunk + header). Returns its length,
// or 0 when the message is complete. An empty message yields one PDU.
size_t pocket_frag_next(pocket_frag_t *f, uint8_t *out, size_t cap);

// Reads one PDU that is a whole message: well formed and without the "more"
// flag. Returns false otherwise (the caller then uses the reassembler, which
// counts malformed PDUs). The payload view points into pdu.
bool pocket_pdu_single(const uint8_t *pdu, size_t len, uint8_t *type, const uint8_t **payload,
                       size_t *payload_len);

// True if a received PDU is a whole small message the receiver may hand on
// directly from the PDU buffer: no reassembly in progress, no "more" flag,
// payload at most POCKET_RX_INLINE_BYTES (so the app copies it at once).
// Anything larger goes through the reassembler, whose buffer the app may
// keep; the PDU buffer is overwritten by the next write.
struct pocket_reasm;
bool pocket_pdu_inline(const struct pocket_reasm *r, const uint8_t *pdu, size_t len,
                       uint8_t *type, const uint8_t **payload, size_t *payload_len);

// A received phone message handed from the BLE host task to the app task.
// Messages up to POCKET_RX_INLINE_BYTES travel inside the struct; a larger
// one points at the BLE receive buffer (kept until the app releases it).
// Basis for 16: the longest fixed part of a phone message (REPLY: reply_id +
// final, 5 bytes) with room for fields a later minor version may append.
#define POCKET_RX_INLINE_BYTES 16
typedef struct {
    uint8_t type;
    bool truncated;
    const uint8_t *kept;        // the receive buffer, or NULL: bytes inline
    size_t len;
    uint8_t bytes[POCKET_RX_INLINE_BYTES];
} pocket_rx_msg_t;

// Fills every field of m (whatever it held before). Returns true if m points
// at payload (the caller must keep it valid), false if the bytes were copied.
bool pocket_rx_msg_pack(pocket_rx_msg_t *m, uint8_t type, const uint8_t *payload, size_t len,
                        bool truncated);
// The message payload.
const uint8_t *pocket_rx_msg_data(const pocket_rx_msg_t *m);

// Reassembler for one direction. The buffer is caller-owned; its size is the
// maximum message size. Longer messages are truncated to the buffer and
// flagged, never overflowed.
typedef struct pocket_reasm {
    uint8_t *buf;
    size_t cap;
    size_t len;
    uint8_t type;
    bool active;
    bool truncated;
    uint32_t errors;          // malformed or interrupted PDUs since init
} pocket_reasm_t;

typedef enum {
    POCKET_REASM_MORE = 0,    // fragment stored, message incomplete
    POCKET_REASM_DONE,        // message complete: see type/buf/len/truncated
    POCKET_REASM_ERROR,       // malformed PDU dropped (counted in errors)
} pocket_reasm_result_t;

void pocket_reasm_init(pocket_reasm_t *r, uint8_t *buf, size_t cap);
void pocket_reasm_reset(pocket_reasm_t *r);
// Feed one PDU. After DONE the message stays valid until the next push.
pocket_reasm_result_t pocket_reasm_push(pocket_reasm_t *r, const uint8_t *pdu, size_t len);
