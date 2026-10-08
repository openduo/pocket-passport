// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

#include "pocket_hist.h"

#include <string.h>

#define NO_PREV 0xFFFFFFFFu
// Bytes read per flash call while scanning or checking a CRC, in a stack
// buffer of the app task. It must divide the sector (checked at open) and the
// 4-byte record alignment; 256 B is the flash page. Measured with it: the boot
// scan of 64 KB takes 24-34 ms, and the app stack keeps >= 3.8 KB free.
#define CHUNK 256u

static uint32_t align4(uint32_t n)
{
    return (n + 3u) & ~3u;
}

static uint32_t rec_bytes(uint32_t text_len)
{
    return align4(POCKET_HIST_HEAD_BYTES + text_len);
}

uint32_t pocket_hist_crc32(uint32_t crc, const void *data, size_t len)
{
    // Nibble table: 64 bytes of constants instead of a 1 KB byte table.
    static const uint32_t t[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu, 0x76dc4190u, 0x6b6b51f4u,
        0x4db26158u, 0x5005713cu, 0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
    };
    const uint8_t *p = data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ t[crc & 15u];
        crc = (crc >> 4) ^ t[crc & 15u];
    }
    return ~crc;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool parse_head(const pocket_hist_t *h, uint32_t off, const uint8_t *b,
                       pocket_hist_entry_t *e)
{
    if (get32(b) != POCKET_HIST_MAGIC) return false;
    if (pocket_hist_crc32(0, b, 24) != get32(b + 24)) return false;
    e->off = off;
    e->seq = get32(b + 4);
    e->prev_off = get32(b + 8);
    e->reply_id = get32(b + 12);
    e->text_len = (uint16_t)(b[16] | b[17] << 8);
    e->flags = b[18];
    e->text_crc = get32(b + 20);
    return off + rec_bytes(e->text_len) <= h->fl.size;
}

static bool read_head(const pocket_hist_t *h, uint32_t off, pocket_hist_entry_t *e)
{
    uint8_t b[POCKET_HIST_HEAD_BYTES];
    if (off > h->fl.size - POCKET_HIST_HEAD_BYTES || (off & 3u)) return false;
    if (!h->fl.read(h->fl.ctx, off, b, sizeof b)) return false;
    return parse_head(h, off, b, e);
}

static bool text_ok(const pocket_hist_t *h, const pocket_hist_entry_t *e)
{
    uint8_t b[CHUNK];
    uint32_t crc = 0;
    uint32_t at = e->off + POCKET_HIST_HEAD_BYTES;
    for (uint32_t left = e->text_len; left;) {
        uint32_t n = left < CHUNK ? left : CHUNK;
        if (!h->fl.read(h->fl.ctx, at, b, n)) return false;
        crc = pocket_hist_crc32(crc, b, n);
        at += n;
        left -= n;
    }
    return crc == e->text_crc;
}

// The record before cur, if it is intact and directly precedes it.
static bool chain_prev(const pocket_hist_t *h, const pocket_hist_entry_t *cur,
                       pocket_hist_entry_t *out)
{
    if (cur->prev_off == NO_PREV || cur->seq <= 1) return false;
    pocket_hist_entry_t p;
    if (!read_head(h, cur->prev_off, &p) || p.seq != cur->seq - 1 || !text_ok(h, &p)) return false;
    *out = p;
    return true;
}

static void find_oldest(pocket_hist_t *h)
{
    pocket_hist_entry_t cur = h->newest, p;
    uint32_t sum = cur.text_len;
    while (chain_prev(h, &cur, &p) && sum + p.text_len <= h->budget) {
        sum += p.text_len;
        cur = p;
    }
    h->oldest = cur;
}

static bool erased(const pocket_hist_t *h, uint32_t from, uint32_t to)
{
    uint8_t b[CHUNK];
    while (from < to) {
        uint32_t n = to - from < CHUNK ? to - from : CHUNK;
        if (!h->fl.read(h->fl.ctx, from, b, n)) return false;
        for (uint32_t i = 0; i < n; i++) {
            if (b[i] != 0xFF) return false;
        }
        from += n;
    }
    return true;
}

bool pocket_hist_open(pocket_hist_t *h, const pocket_hist_flash_t *fl, uint32_t budget)
{
    memset(h, 0, sizeof *h);
    h->fl = *fl;
    h->budget = budget;
    if (!fl->sector || fl->size < 2 * fl->sector || fl->size % fl->sector || fl->sector % CHUNK) {
        return false;
    }
    // Every 4-byte offset can start a record; erased flash is skipped by
    // its first word.
    uint8_t b[CHUNK];
    uint32_t skip_to = 0;
    for (uint32_t blk = 0; blk < fl->size; blk += CHUNK) {
        if (!fl->read(fl->ctx, blk, b, CHUNK)) return false;
        for (uint32_t i = 0; i < CHUNK; i += 4) {
            uint32_t off = blk + i;
            if (off < skip_to || get32(b + i) != POCKET_HIST_MAGIC) continue;
            pocket_hist_entry_t e;
            if (!read_head(h, off, &e) || !text_ok(h, &e)) continue;
            skip_to = off + rec_bytes(e.text_len);
            if (!h->any || e.seq > h->newest.seq) {
                h->newest = e;
                h->any = true;
            }
        }
    }
    if (!h->any) return true;  // empty: the first append erases sector 0
    find_oldest(h);
    h->head = h->newest.off + rec_bytes(h->newest.text_len);
    // The rest of the head's sector must still be erased (a write cut by a
    // power loss may have left bytes there); otherwise resume at the next.
    uint32_t sector_end = (h->head + fl->sector - 1) / fl->sector * fl->sector;
    if (!erased(h, h->head, sector_end)) h->head = sector_end;
    return true;
}

bool pocket_hist_append(pocket_hist_t *h, uint32_t reply_id, const void *text, size_t len,
                        uint8_t flags, pocket_hist_entry_t *out)
{
    const pocket_hist_flash_t *fl = &h->fl;
    if (len > UINT16_MAX) return false;
    const uint32_t rec = rec_bytes((uint32_t)len);
    // A record must leave the sector before it intact (its predecessor).
    if (rec > fl->size - fl->sector) return false;
    uint32_t pos = h->head;
    if (pos + rec > fl->size) pos = 0;
    for (uint32_t s = pos / fl->sector * fl->sector; s < pos + rec; s += fl->sector) {
        if (s >= pos && !fl->erase_sector(fl->ctx, s)) return false;
    }
    pocket_hist_entry_t e = {
        .seq = h->any ? h->newest.seq + 1 : 1,
        .off = pos,
        .prev_off = h->any ? h->newest.off : NO_PREV,
        .reply_id = reply_id,
        .text_crc = pocket_hist_crc32(0, text, len),
        .text_len = (uint16_t)len,
        .flags = flags,
    };
    uint8_t b[POCKET_HIST_HEAD_BYTES];
    put32(b, POCKET_HIST_MAGIC);
    put32(b + 4, e.seq);
    put32(b + 8, e.prev_off);
    put32(b + 12, e.reply_id);
    b[16] = (uint8_t)len;
    b[17] = (uint8_t)(len >> 8);
    b[18] = flags;
    b[19] = 0xFF;
    put32(b + 20, e.text_crc);
    put32(b + 24, pocket_hist_crc32(0, b, 24));
    // Text first, head last: a record without its head is never found.
    if (len && !fl->write(fl->ctx, pos + POCKET_HIST_HEAD_BYTES, text, len)) return false;
    if (!fl->write(fl->ctx, pos, b, sizeof b)) return false;
    h->head = pos + rec;
    pocket_hist_entry_t check;
    if (!read_head(h, pos, &check) || !text_ok(h, &check)) return false;
    h->newest = e;
    h->any = true;
    find_oldest(h);
    if (out) *out = e;
    return true;
}

uint32_t pocket_hist_count(const pocket_hist_t *h)
{
    return h->any ? h->newest.seq - h->oldest.seq + 1 : 0;
}

uint32_t pocket_hist_position(const pocket_hist_t *h, const pocket_hist_entry_t *e)
{
    return h->any ? h->newest.seq - e->seq + 1 : 0;
}

bool pocket_hist_prev(const pocket_hist_t *h, const pocket_hist_entry_t *cur,
                      pocket_hist_entry_t *out)
{
    if (!h->any || cur->seq <= h->oldest.seq) return false;
    return chain_prev(h, cur, out);
}

bool pocket_hist_next(const pocket_hist_t *h, const pocket_hist_entry_t *cur,
                      pocket_hist_entry_t *out)
{
    if (!h->any || cur->seq >= h->newest.seq) return false;
    // The next record follows directly, or starts the partition after a wrap.
    const uint32_t at[2] = { cur->off + rec_bytes(cur->text_len), 0 };
    for (int i = 0; i < 2; i++) {
        pocket_hist_entry_t e;
        if (read_head(h, at[i], &e) && e.seq == cur->seq + 1 && text_ok(h, &e)) {
            *out = e;
            return true;
        }
    }
    return false;
}

bool pocket_hist_find(const pocket_hist_t *h, uint32_t reply_id, pocket_hist_entry_t *out)
{
    if (!h->any) return false;
    pocket_hist_entry_t cur = h->newest;
    for (;;) {
        if (cur.reply_id == reply_id) {
            if (out) *out = cur;
            return true;
        }
        if (!pocket_hist_prev(h, &cur, &cur)) return false;
    }
}

int pocket_hist_read_text(const pocket_hist_t *h, const pocket_hist_entry_t *e, void *buf,
                          size_t cap)
{
    if (cap < e->text_len) return -1;
    if (e->text_len &&
        !h->fl.read(h->fl.ctx, e->off + POCKET_HIST_HEAD_BYTES, buf, e->text_len)) {
        return -1;
    }
    if (pocket_hist_crc32(0, buf, e->text_len) != e->text_crc) return -1;
    return e->text_len;
}

bool pocket_hist_clear(pocket_hist_t *h)
{
    for (uint32_t s = 0; s < h->fl.size; s += h->fl.sector) {
        if (!h->fl.erase_sector(h->fl.ctx, s)) return false;
    }
    h->any = false;
    h->head = 0;
    return true;
}
