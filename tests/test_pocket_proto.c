// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Host tests: pocket BLE message codec, fragmentation and reassembly.
#include "pocket_check.h"
#include "pocket_proto.h"

#include <string.h>

static void test_builders(void)
{
    uint8_t b[64];
    pocket_info_t info = { .fw_version = "1.2.3", .battery_pct = 87, .charging = 0xFF,
                           .preroll_on = 1 };
    size_t n = pocket_msg_info(b, sizeof b, &info);
    CHECK(n == 2 + 1 + 5 + 3);
    CHECK(b[0] == POCKET_PROTO_MAJOR && b[1] == POCKET_PROTO_MINOR);
    CHECK(b[2] == 5 && memcmp(b + 3, "1.2.3", 5) == 0);
    CHECK(b[8] == 87 && b[9] == 0xFF && b[10] == 1);
    CHECK(pocket_msg_info(b, 10, &info) == 0);

    CHECK(pocket_msg_press_start(b, sizeof b, 0x1234) == 2 && b[0] == 0x34 && b[1] == 0x12);
    const uint8_t pkt[3] = { 0xAA, 0xBB, 0xCC };
    n = pocket_msg_audio(b, sizeof b, 0x0102, 0xFFFE, pkt, 3);
    CHECK(n == 7 && b[0] == 0x02 && b[1] == 0x01 && b[2] == 0xFE && b[3] == 0xFF);
    CHECK(memcmp(b + 4, pkt, 3) == 0);
    CHECK(pocket_msg_audio(b, 6, 1, 1, pkt, 3) == 0);
    n = pocket_msg_press_end(b, sizeof b, 7, 300);
    CHECK(n == 4 && b[0] == 7 && b[2] == (300 & 0xFF) && b[3] == (300 >> 8));
    CHECK(pocket_msg_status(b, sizeof b, 50, 1) == 2 && b[0] == 50 && b[1] == 1);
    CHECK(pocket_msg_keepalive(b, sizeof b, 9) == 2 && b[0] == 9 && b[1] == 0);
}

static void test_parse(void)
{
    pocket_downlink_t d;
    const uint8_t result[] = { 0x05, 0x00, 0x00, 'h', 'i' };
    CHECK(pocket_msg_parse_downlink(POCKET_MSG_RESULT, result, sizeof result, &d));
    CHECK(d.press_id == 5 && d.code == 0 && d.text_len == 2 && d.text[0] == 'h');
    CHECK(!pocket_msg_parse_downlink(POCKET_MSG_RESULT, result, 2, &d));

    const uint8_t reply[] = { 0x01, 0x02, 0x03, 0x04, 0x01 };
    CHECK(pocket_msg_parse_downlink(POCKET_MSG_REPLY, reply, sizeof reply, &d));
    CHECK(d.reply_id == 0x04030201u && d.final && d.text_len == 0);
    CHECK(!pocket_msg_parse_downlink(POCKET_MSG_REPLY, reply, 4, &d));

    CHECK(pocket_msg_parse_downlink(POCKET_MSG_REPLY_DONE, reply, 4, &d));
    CHECK(d.reply_id == 0x04030201u);
    // APP_STATE: state, then the 1.2 language byte; later bytes are ignored.
    const uint8_t state[] = { 1, 99 };  // unknown language value: parsed, unchecked
    CHECK(pocket_msg_parse_downlink(POCKET_MSG_APP_STATE, state, sizeof state, &d));
    CHECK(d.code == 1 && d.has_lang && d.lang == 99);
    CHECK(pocket_msg_parse_downlink(POCKET_MSG_APP_STATE, state, 1, &d));  // 1.0/1.1 phone
    CHECK(d.code == 1 && !d.has_lang);
    const uint8_t state_en[] = { 0, POCKET_LANG_EN, 7 };  // trailing byte: a later minor
    CHECK(pocket_msg_parse_downlink(POCKET_MSG_APP_STATE, state_en, sizeof state_en, &d));
    CHECK(d.code == 0 && d.has_lang && d.lang == POCKET_LANG_EN);
    CHECK(!pocket_msg_parse_downlink(POCKET_MSG_APP_STATE, state, 0, &d));
    const uint8_t work[] = { POCKET_WORK_TOOL, 0xe5, 0x9c, 0xa8 };  // phase 3, label "在"
    CHECK(pocket_msg_parse_downlink(POCKET_MSG_WORK, work, sizeof work, &d));
    CHECK(d.type == POCKET_MSG_WORK && d.code == POCKET_WORK_TOOL && d.text_len == 3 &&
          d.text[0] == 0xe5);
    CHECK(pocket_msg_parse_downlink(POCKET_MSG_WORK, work, 1, &d));  // label is optional
    CHECK(d.code == POCKET_WORK_TOOL && d.text_len == 0);
    CHECK(!pocket_msg_parse_downlink(POCKET_MSG_WORK, work, 0, &d));
    CHECK(!pocket_msg_parse_downlink(0x7F, state, 2, &d));
    CHECK(!pocket_msg_parse_downlink(POCKET_MSG_AUDIO, state, 2, &d));
}

// Fragment a message at an MTU, reassemble it, compare.
static void roundtrip(uint16_t mtu, size_t len)
{
    static uint8_t msg[6000], pdu[600], rbuf[6000];
    for (size_t i = 0; i < len; i++) msg[i] = (uint8_t)(i * 31 + len);
    size_t chunk = pocket_pdu_chunk_max(mtu);
    CHECK(chunk == (size_t)mtu - 7);
    pocket_frag_t f;
    CHECK(pocket_frag_init(&f, POCKET_MSG_REPLY, msg, len, chunk));
    pocket_reasm_t r;
    pocket_reasm_init(&r, rbuf, sizeof rbuf);
    size_t pdus = 0, n;
    pocket_reasm_result_t res = POCKET_REASM_ERROR;
    while ((n = pocket_frag_next(&f, pdu, sizeof pdu)) > 0) {
        CHECK(n <= (size_t)mtu - 3);
        CHECK(pdu[0] == POCKET_MSG_REPLY);
        pdus++;
        res = pocket_reasm_push(&r, pdu, n);
        CHECK(res != POCKET_REASM_ERROR);
    }
    size_t expect = len == 0 ? 1 : (len + chunk - 1) / chunk;
    CHECK(pdus == expect);
    CHECK(res == POCKET_REASM_DONE);
    CHECK(r.type == POCKET_MSG_REPLY && r.len == len && !r.truncated);
    CHECK(memcmp(rbuf, msg, len) == 0);
    CHECK(r.errors == 0);
}

static void test_fragmentation(void)
{
    const uint16_t mtus[] = { 23, 27, 185, 247, 512 };
    const size_t lens[] = { 0, 1, 15, 16, 17, 100, 178, 179, 1000, 4096, 5999 };
    for (size_t i = 0; i < sizeof mtus / sizeof mtus[0]; i++) {
        for (size_t j = 0; j < sizeof lens / sizeof lens[0]; j++) roundtrip(mtus[i], lens[j]);
    }
    CHECK(pocket_pdu_chunk_max(7) == 0);
    CHECK(pocket_pdu_chunk_max(8) == 1);
    pocket_frag_t f;
    CHECK(!pocket_frag_init(&f, 1, NULL, 0, 0));
    uint8_t out[8];
    CHECK(pocket_frag_next(&f, out, sizeof out) == 0);
}

static void test_reassembly_errors(void)
{
    uint8_t buf[8];
    pocket_reasm_t r;
    pocket_reasm_init(&r, buf, sizeof buf);

    // Short PDU and length mismatch.
    const uint8_t short_pdu[] = { 0x82, 0x00, 0x00 };
    CHECK(pocket_reasm_push(&r, short_pdu, sizeof short_pdu) == POCKET_REASM_ERROR);
    const uint8_t bad_len[] = { 0x82, 0x00, 0x05, 0x00, 1, 2 };
    CHECK(pocket_reasm_push(&r, bad_len, sizeof bad_len) == POCKET_REASM_ERROR);
    CHECK(r.errors == 2);

    // Truncation: 12 bytes into an 8-byte buffer.
    const uint8_t a[] = { 0x82, 0x01, 6, 0, 1, 2, 3, 4, 5, 6 };
    const uint8_t b[] = { 0x82, 0x00, 6, 0, 7, 8, 9, 10, 11, 12 };
    CHECK(pocket_reasm_push(&r, a, sizeof a) == POCKET_REASM_MORE);
    CHECK(pocket_reasm_push(&r, b, sizeof b) == POCKET_REASM_DONE);
    CHECK(r.len == 8 && r.truncated && buf[7] == 8);

    // A different type mid-message drops the partial one.
    const uint8_t c[] = { 0x82, 0x01, 2, 0, 1, 2 };
    const uint8_t d[] = { 0x84, 0x00, 1, 0, 1 };
    CHECK(pocket_reasm_push(&r, c, sizeof c) == POCKET_REASM_MORE);
    CHECK(pocket_reasm_push(&r, d, sizeof d) == POCKET_REASM_DONE);
    CHECK(r.type == 0x84 && r.len == 1 && !r.truncated && r.errors == 3);

    // A malformed PDU mid-message drops the partial one; the next message is clean.
    CHECK(pocket_reasm_push(&r, c, sizeof c) == POCKET_REASM_MORE);
    CHECK(pocket_reasm_push(&r, short_pdu, sizeof short_pdu) == POCKET_REASM_ERROR);
    CHECK(pocket_reasm_push(&r, d, sizeof d) == POCKET_REASM_DONE);
    CHECK(r.len == 1);
}

static void test_single_pdu(void)
{
    uint8_t type = 0;
    const uint8_t *payload = NULL;
    size_t n = 99;
    const uint8_t done[] = { POCKET_MSG_REPLY_DONE, 0, 4, 0, 1, 2, 3, 4 };
    CHECK(pocket_pdu_single(done, sizeof done, &type, &payload, &n));
    CHECK(type == POCKET_MSG_REPLY_DONE && n == 4 && payload == done + 4);
    const uint8_t empty[] = { POCKET_MSG_APP_STATE, 0, 0, 0 };
    CHECK(pocket_pdu_single(empty, sizeof empty, &type, &payload, &n) && n == 0);
    // A fragment, a length mismatch and a short header are not whole messages.
    const uint8_t frag[] = { POCKET_MSG_REPLY, POCKET_PDU_FLAG_MORE, 1, 0, 'a' };
    CHECK(!pocket_pdu_single(frag, sizeof frag, &type, &payload, &n));
    const uint8_t bad_len[] = { POCKET_MSG_REPLY, 0, 2, 0, 'a' };
    CHECK(!pocket_pdu_single(bad_len, sizeof bad_len, &type, &payload, &n));
    CHECK(!pocket_pdu_single(done, 3, &type, &payload, &n));
}

static void test_rx_msg_pack(void)
{
    // Regression: the app event is a union whose initializer zeroed only its
    // first member, so an inline message kept a stale pointer and RESULT and
    // WORK were parsed from garbage. Packing must set every field.
    pocket_rx_msg_t m;
    memset(&m, 0xA5, sizeof m);
    const uint8_t result[] = { 0x34, 0x12, 0, 0xe4, 0xbd, 0xa0 };  // press 0x1234, code 0, "ni"
    CHECK(!pocket_rx_msg_pack(&m, POCKET_MSG_RESULT, result, sizeof result, false));
    CHECK(m.kept == NULL && pocket_rx_msg_data(&m) == m.bytes && !m.truncated);
    pocket_downlink_t d;
    CHECK(pocket_msg_parse_downlink(m.type, pocket_rx_msg_data(&m), m.len, &d));
    CHECK(d.press_id == 0x1234 && d.code == 0 && d.text_len == 3);
    memset(&m, 0xA5, sizeof m);
    const uint8_t work[] = { POCKET_WORK_TOOL };
    CHECK(!pocket_rx_msg_pack(&m, POCKET_MSG_WORK, work, 1, false));
    CHECK(pocket_msg_parse_downlink(m.type, pocket_rx_msg_data(&m), m.len, &d) &&
          d.code == POCKET_WORK_TOOL);
    // Larger messages point at the caller's buffer.
    static uint8_t big[POCKET_RX_INLINE_BYTES + 1];
    memset(&m, 0xA5, sizeof m);
    CHECK(pocket_rx_msg_pack(&m, POCKET_MSG_REPLY, big, sizeof big, true));
    CHECK(pocket_rx_msg_data(&m) == big && m.truncated && m.len == sizeof big);
    // Empty payload: inline, no copy from NULL.
    CHECK(!pocket_rx_msg_pack(&m, POCKET_MSG_APP_STATE, NULL, 0, false) && m.kept == NULL);
}

static void test_pdu_inline(void)
{
    // Regression: a single-PDU RESULT with a transcript (> 16 bytes) was
    // handed on from the PDU buffer, kept by the app, and overwritten by the
    // WORK that followed: the transcript never showed. Only small messages
    // may bypass the reassembler.
    static uint8_t buf[64];
    pocket_reasm_t r;
    pocket_reasm_init(&r, buf, sizeof buf);
    uint8_t type;
    const uint8_t *payload;
    size_t n;
    uint8_t pdu[4 + 40] = { POCKET_MSG_RESULT, 0, 40, 0 };
    CHECK(!pocket_pdu_inline(&r, pdu, sizeof pdu, &type, &payload, &n));
    uint8_t small[4 + POCKET_RX_INLINE_BYTES] = { POCKET_MSG_RESULT, 0, POCKET_RX_INLINE_BYTES, 0 };
    CHECK(pocket_pdu_inline(&r, small, sizeof small, &type, &payload, &n) &&
          n == POCKET_RX_INLINE_BYTES && payload == small + 4);
    uint8_t one_more[4 + POCKET_RX_INLINE_BYTES + 1] = { POCKET_MSG_RESULT, 0,
                                                         POCKET_RX_INLINE_BYTES + 1, 0 };
    CHECK(!pocket_pdu_inline(&r, one_more, sizeof one_more, &type, &payload, &n));
    // While a fragmented message is open, even small PDUs go to the reassembler.
    const uint8_t frag[] = { POCKET_MSG_REPLY, POCKET_PDU_FLAG_MORE, 1, 0, 'a' };
    CHECK(pocket_reasm_push(&r, frag, sizeof frag) == POCKET_REASM_MORE);
    const uint8_t done[] = { POCKET_MSG_REPLY_DONE, 0, 4, 0, 1, 2, 3, 4 };
    CHECK(!pocket_pdu_inline(&r, done, sizeof done, &type, &payload, &n));
}

int main(void)
{
    test_builders();
    test_parse();
    test_fragmentation();
    test_reassembly_errors();
    test_single_pdu();
    test_rx_msg_pack();
    test_pdu_inline();
    CHECK_DONE("test_pocket_proto");
    return 0;
}
