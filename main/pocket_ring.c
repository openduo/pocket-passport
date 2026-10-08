// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

#include "pocket_ring.h"

#include <string.h>

#define LEN_BYTES 2

void pocket_ring_init(pocket_ring_t *r, uint8_t *buf, size_t cap)
{
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    r->cap = cap;
}

void pocket_ring_clear(pocket_ring_t *r)
{
    r->head = 0;
    r->used = 0;
    r->count = 0;
}

static void copy_in(pocket_ring_t *r, size_t at, const uint8_t *src, size_t n)
{
    at %= r->cap;
    size_t first = r->cap - at < n ? r->cap - at : n;
    memcpy(r->buf + at, src, first);
    if (n > first) memcpy(r->buf, src + first, n - first);
}

static void copy_out(const pocket_ring_t *r, size_t at, uint8_t *dst, size_t n)
{
    at %= r->cap;
    size_t first = r->cap - at < n ? r->cap - at : n;
    memcpy(dst, r->buf + at, first);
    if (n > first) memcpy(dst + first, r->buf, n - first);
}

static size_t peek_len(const pocket_ring_t *r)
{
    uint8_t h[LEN_BYTES];
    copy_out(r, r->head, h, LEN_BYTES);
    return (size_t)h[0] | ((size_t)h[1] << 8);
}

static void drop_oldest(pocket_ring_t *r)
{
    size_t n = LEN_BYTES + peek_len(r);
    r->head = (r->head + n) % r->cap;
    r->used -= n;
    r->count--;
}

bool pocket_ring_push(pocket_ring_t *r, const uint8_t *pkt, size_t len)
{
    if (len == 0 || len > UINT16_MAX || len + LEN_BYTES > r->cap) {
        r->rejected++;
        return false;
    }
    const size_t need = len + LEN_BYTES;
    while (r->cap - r->used < need) {
        drop_oldest(r);
        r->evicted++;
    }
    const size_t tail = r->head + r->used;
    const uint8_t h[LEN_BYTES] = { (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    copy_in(r, tail, h, LEN_BYTES);
    copy_in(r, tail + LEN_BYTES, pkt, len);
    r->used += need;
    r->count++;
    return true;
}

void pocket_ring_trim(pocket_ring_t *r, uint32_t max_packets)
{
    while (r->count > max_packets) {
        drop_oldest(r);
        r->evicted++;
    }
}

size_t pocket_ring_pop(pocket_ring_t *r, uint8_t *out, size_t cap, bool *dropped)
{
    if (dropped) *dropped = false;
    if (r->count == 0) return 0;
    size_t len = peek_len(r);
    if (len > cap) {
        drop_oldest(r);
        if (dropped) *dropped = true;
        return 0;
    }
    copy_out(r, r->head + LEN_BYTES, out, len);
    drop_oldest(r);
    return len;
}
