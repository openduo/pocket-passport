// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

#include "pocket_proto.h"

#include <string.h>

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

size_t pocket_msg_info(uint8_t *out, size_t cap, const pocket_info_t *info)
{
    const char *fw = info->fw_version ? info->fw_version : "";
    size_t fw_len = strlen(fw);
    if (fw_len > 255) fw_len = 255;
    const size_t need = 2 + 1 + fw_len + 3;
    if (cap < need) return 0;
    size_t n = 0;
    out[n++] = POCKET_PROTO_MAJOR;
    out[n++] = POCKET_PROTO_MINOR;
    out[n++] = (uint8_t)fw_len;
    memcpy(out + n, fw, fw_len);
    n += fw_len;
    out[n++] = info->battery_pct;
    out[n++] = info->charging;
    out[n++] = info->preroll_on ? 1 : 0;
    return n;
}

size_t pocket_msg_press_start(uint8_t *out, size_t cap, uint16_t press_id)
{
    if (cap < 2) return 0;
    put_u16(out, press_id);
    return 2;
}

size_t pocket_msg_audio(uint8_t *out, size_t cap, uint16_t press_id, uint16_t seq,
                        const uint8_t *packet, size_t packet_len)
{
    if (cap < 4 + packet_len) return 0;
    put_u16(out, press_id);
    put_u16(out + 2, seq);
    if (packet_len) memcpy(out + 4, packet, packet_len);
    return 4 + packet_len;
}

size_t pocket_msg_press_end(uint8_t *out, size_t cap, uint16_t press_id, uint16_t packets)
{
    if (cap < 4) return 0;
    put_u16(out, press_id);
    put_u16(out + 2, packets);
    return 4;
}

size_t pocket_msg_status(uint8_t *out, size_t cap, uint8_t battery_pct, uint8_t charging)
{
    if (cap < 2) return 0;
    out[0] = battery_pct;
    out[1] = charging;
    return 2;
}

size_t pocket_msg_keepalive(uint8_t *out, size_t cap, uint16_t press_id)
{
    return pocket_msg_press_start(out, cap, press_id);
}

bool pocket_msg_parse_downlink(uint8_t type, const uint8_t *payload, size_t len,
                               pocket_downlink_t *out)
{
    memset(out, 0, sizeof(*out));
    out->type = (pocket_msg_type_t)type;
    switch (type) {
    case POCKET_MSG_RESULT:
        if (len < 3) return false;
        out->press_id = get_u16(payload);
        out->code = payload[2];
        out->text = payload + 3;
        out->text_len = len - 3;
        return true;
    case POCKET_MSG_REPLY:
        if (len < 5) return false;
        out->reply_id = get_u32(payload);
        out->final = payload[4] != 0;
        out->text = payload + 5;
        out->text_len = len - 5;
        return true;
    case POCKET_MSG_REPLY_DONE:
        if (len < 4) return false;
        out->reply_id = get_u32(payload);
        return true;
    case POCKET_MSG_APP_STATE:
        if (len < 1) return false;
        out->code = payload[0];
        if (len >= 2) {
            out->has_lang = true;
            out->lang = payload[1];
        }
        return true;
    case POCKET_MSG_WORK:
        if (len < 1) return false;
        out->code = payload[0];
        out->text = payload + 1;
        out->text_len = len - 1;
        return true;
    default:
        return false;
    }
}

size_t pocket_pdu_chunk_max(uint16_t att_mtu)
{
    if (att_mtu <= POCKET_ATT_HEADER_BYTES + POCKET_PDU_HEADER_BYTES) return 0;
    size_t chunk = (size_t)att_mtu - POCKET_ATT_HEADER_BYTES - POCKET_PDU_HEADER_BYTES;
    // The PDU length field is u16.
    return chunk > UINT16_MAX ? UINT16_MAX : chunk;
}

bool pocket_frag_init(pocket_frag_t *f, uint8_t type, const uint8_t *payload, size_t len,
                      size_t chunk)
{
    memset(f, 0, sizeof(*f));
    if (chunk == 0) {
        f->done = true;
        return false;
    }
    f->type = type;
    f->payload = payload;
    f->len = len;
    f->chunk = chunk > UINT16_MAX ? UINT16_MAX : chunk;
    return true;
}

size_t pocket_frag_next(pocket_frag_t *f, uint8_t *out, size_t cap)
{
    if (f->done) return 0;
    size_t left = f->len - f->offset;
    size_t n = left < f->chunk ? left : f->chunk;
    if (cap < POCKET_PDU_HEADER_BYTES + n) return 0;
    bool more = f->offset + n < f->len;
    out[0] = f->type;
    out[1] = more ? POCKET_PDU_FLAG_MORE : 0;
    put_u16(out + 2, (uint16_t)n);
    if (n) memcpy(out + POCKET_PDU_HEADER_BYTES, f->payload + f->offset, n);
    f->offset += n;
    if (!more) f->done = true;
    return POCKET_PDU_HEADER_BYTES + n;
}

bool pocket_pdu_single(const uint8_t *pdu, size_t len, uint8_t *type, const uint8_t **payload,
                       size_t *payload_len)
{
    if (len < POCKET_PDU_HEADER_BYTES || get_u16(pdu + 2) != len - POCKET_PDU_HEADER_BYTES ||
        (pdu[1] & POCKET_PDU_FLAG_MORE)) {
        return false;
    }
    *type = pdu[0];
    *payload = pdu + POCKET_PDU_HEADER_BYTES;
    *payload_len = len - POCKET_PDU_HEADER_BYTES;
    return true;
}

bool pocket_pdu_inline(const struct pocket_reasm *r, const uint8_t *pdu, size_t len,
                       uint8_t *type, const uint8_t **payload, size_t *payload_len)
{
    return !r->active && pocket_pdu_single(pdu, len, type, payload, payload_len) &&
           *payload_len <= POCKET_RX_INLINE_BYTES;
}

bool pocket_rx_msg_pack(pocket_rx_msg_t *m, uint8_t type, const uint8_t *payload, size_t len,
                        bool truncated)
{
    memset(m, 0, sizeof *m);
    m->type = type;
    m->truncated = truncated;
    m->len = len;
    if (len <= sizeof m->bytes) {
        if (len) memcpy(m->bytes, payload, len);
        return false;
    }
    m->kept = payload;
    return true;
}

const uint8_t *pocket_rx_msg_data(const pocket_rx_msg_t *m)
{
    return m->kept ? m->kept : m->bytes;
}

void pocket_reasm_init(pocket_reasm_t *r, uint8_t *buf, size_t cap)
{
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    r->cap = cap;
}

void pocket_reasm_reset(pocket_reasm_t *r)
{
    r->len = 0;
    r->active = false;
    r->truncated = false;
}

pocket_reasm_result_t pocket_reasm_push(pocket_reasm_t *r, const uint8_t *pdu, size_t len)
{
    if (len < POCKET_PDU_HEADER_BYTES ||
        get_u16(pdu + 2) != len - POCKET_PDU_HEADER_BYTES) {
        // A malformed PDU poisons any partial message: its bytes are lost.
        pocket_reasm_reset(r);
        r->errors++;
        return POCKET_REASM_ERROR;
    }
    const uint8_t type = pdu[0];
    const bool more = (pdu[1] & POCKET_PDU_FLAG_MORE) != 0;
    const uint8_t *body = pdu + POCKET_PDU_HEADER_BYTES;
    const size_t n = len - POCKET_PDU_HEADER_BYTES;

    if (r->active && type != r->type) {
        // Fragments of one message are consecutive; a new type means the
        // previous message was cut off. Drop it and start over with this PDU.
        r->errors++;
        pocket_reasm_reset(r);
    }
    if (!r->active) {
        r->active = true;
        r->type = type;
        r->len = 0;
        r->truncated = false;
    }
    size_t room = r->cap - r->len;
    size_t copy = n < room ? n : room;
    if (copy) memcpy(r->buf + r->len, body, copy);
    r->len += copy;
    if (copy < n) r->truncated = true;

    if (more) return POCKET_REASM_MORE;
    r->active = false;
    return POCKET_REASM_DONE;
}
