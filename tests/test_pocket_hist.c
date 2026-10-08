// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Host tests: reply history ring on a NOR flash model (writes only clear
// bits, erase sets a sector to 0xFF): append, budget eviction, navigation,
// reopen after reboot, wrap-around wear, power loss mid-write, clear.
#include "pocket_check.h"
#include "pocket_hist.h"

#include <string.h>

#define SECTOR 4096u
#define SIZE   (16u * SECTOR)
#define BUDGET 16384u

typedef struct {
    uint8_t mem[SIZE];
    uint32_t erases[SIZE / SECTOR];
    long write_budget;          // bytes left before a simulated power loss; <0: unlimited
} flash_t;

static flash_t F;

static bool f_read(void *ctx, uint32_t off, void *buf, size_t len)
{
    flash_t *f = ctx;
    if (off + len > SIZE) return false;
    memcpy(buf, f->mem + off, len);
    return true;
}

static bool f_write(void *ctx, uint32_t off, const void *buf, size_t len)
{
    flash_t *f = ctx;
    if (off + len > SIZE) return false;
    const uint8_t *p = buf;
    for (size_t i = 0; i < len; i++) {
        if (f->write_budget == 0) return false;
        if (f->write_budget > 0) f->write_budget--;
        f->mem[off + i] &= p[i];
    }
    return true;
}

static bool f_erase(void *ctx, uint32_t off)
{
    flash_t *f = ctx;
    if (off % SECTOR || off >= SIZE) return false;
    memset(f->mem + off, 0xFF, SECTOR);
    f->erases[off / SECTOR]++;
    return true;
}

static const pocket_hist_flash_t FL = { &F, f_read, f_write, f_erase, SIZE, SECTOR };

static uint32_t rng = 12345;
static uint32_t rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

// Text of reply id: deterministic bytes so any record can be checked.
static size_t make_text(uint32_t id, size_t len, uint8_t *out)
{
    for (size_t i = 0; i < len; i++) out[i] = (uint8_t)('a' + (id + i) % 26);
    return len;
}

static uint8_t buf[8192];

static void check_entry(const pocket_hist_t *h, const pocket_hist_entry_t *e, uint32_t id,
                        size_t len)
{
    static uint8_t want[8192];
    CHECK(e->reply_id == id && e->text_len == len);
    CHECK(pocket_hist_read_text(h, e, buf, sizeof buf) == (int)len);
    make_text(id, len, want);
    CHECK(memcmp(buf, want, len) == 0);
}

static void append(pocket_hist_t *h, uint32_t id, size_t len)
{
    make_text(id, len, buf);
    pocket_hist_entry_t e;
    CHECK(pocket_hist_append(h, id, buf, len, 0, &e));
    CHECK(e.reply_id == id && h->newest.seq == e.seq);
}

static void fresh_flash(uint8_t fill)
{
    memset(&F, 0, sizeof F);
    memset(F.mem, fill, SIZE);
    F.write_budget = -1;
}

static void test_empty_and_basic(void)
{
    fresh_flash(0xFF);
    pocket_hist_t h;
    CHECK(pocket_hist_open(&h, &FL, BUDGET));
    CHECK(pocket_hist_count(&h) == 0 && !pocket_hist_find(&h, 1, NULL));

    append(&h, 10, 5);
    append(&h, 11, 0);          // an empty reply is a record too
    append(&h, 12, 300);
    CHECK(pocket_hist_count(&h) == 3);
    pocket_hist_entry_t e = h.newest, p;
    CHECK(pocket_hist_position(&h, &e) == 1);
    check_entry(&h, &e, 12, 300);
    CHECK(pocket_hist_prev(&h, &e, &p));
    check_entry(&h, &p, 11, 0);
    CHECK(pocket_hist_position(&h, &p) == 2);
    CHECK(pocket_hist_prev(&h, &p, &p));
    check_entry(&h, &p, 10, 5);
    CHECK(!pocket_hist_prev(&h, &p, &e));
    CHECK(pocket_hist_next(&h, &p, &p) && p.reply_id == 11);
    CHECK(pocket_hist_next(&h, &p, &p) && p.reply_id == 12);
    CHECK(!pocket_hist_next(&h, &p, &e));
    CHECK(pocket_hist_find(&h, 11, &e) && e.text_len == 0);
    CHECK(!pocket_hist_find(&h, 13, NULL));

    // Reboot: the same history is found by the scan.
    pocket_hist_t h2;
    CHECK(pocket_hist_open(&h2, &FL, BUDGET));
    CHECK(pocket_hist_count(&h2) == 3 && h2.newest.reply_id == 12 && h2.head == h.head);
    append(&h2, 13, 7);
    CHECK(pocket_hist_count(&h2) == 4);
}

static void test_budget_eviction(void)
{
    fresh_flash(0xFF);
    pocket_hist_t h;
    CHECK(pocket_hist_open(&h, &FL, BUDGET));
    // Four full replies fit in 16 KB; the fifth evicts the oldest.
    for (uint32_t id = 1; id <= 4; id++) append(&h, id, 4091);
    CHECK(pocket_hist_count(&h) == 4 && h.oldest.reply_id == 1);
    append(&h, 5, 4091);
    CHECK(pocket_hist_count(&h) == 4 && h.oldest.reply_id == 2);
    CHECK(!pocket_hist_find(&h, 1, NULL) && pocket_hist_find(&h, 2, NULL));
    // Short replies: many fit; the budget counts text bytes.
    for (uint32_t id = 100; id < 140; id++) append(&h, id, 10);
    CHECK(pocket_hist_count(&h) == 40 + 3);  // 3 x 4091 + 400 <= 16384, a 4th 4091 is not
    CHECK(h.oldest.reply_id == 3);
    pocket_hist_t h2;
    CHECK(pocket_hist_open(&h2, &FL, BUDGET));
    CHECK(pocket_hist_count(&h2) == pocket_hist_count(&h) && h2.oldest.reply_id == 3);
}

static void test_wrap_and_wear(void)
{
    fresh_flash(0xFF);
    pocket_hist_t h;
    CHECK(pocket_hist_open(&h, &FL, BUDGET));
    static uint32_t ids[4000];
    static uint16_t lens[4000];
    uint32_t n = 0;
    for (uint32_t i = 0; i < 1500; i++) {
        uint32_t id = 1000 + i;
        size_t len = rnd() % 4092;
        append(&h, id, len);
        ids[n] = id;
        lens[n] = (uint16_t)len;
        n++;
        // The live set is exactly the newest records within the budget.
        uint32_t sum = 0, want = 0;
        for (uint32_t k = n; k-- > 0;) {
            if (want && sum + lens[k] > BUDGET) break;
            sum += lens[k];
            want++;
        }
        CHECK(pocket_hist_count(&h) == want);
        if (i % 97 == 0) {
            pocket_hist_t r;
            CHECK(pocket_hist_open(&r, &FL, BUDGET));
            CHECK(pocket_hist_count(&r) == want && r.newest.reply_id == id && r.head == h.head);
            // Walk the whole live history from the newest.
            pocket_hist_entry_t e = r.newest;
            for (uint32_t k = 0; k < want; k++) {
                check_entry(&r, &e, ids[n - 1 - k], lens[n - 1 - k]);
                if (k + 1 < want) CHECK(pocket_hist_prev(&r, &e, &e));
            }
            CHECK(!pocket_hist_prev(&r, &e, &e));
            h = r;
        }
    }
    // Wear spreads over every sector of the ring.
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t s = 0; s < SIZE / SECTOR; s++) {
        if (F.erases[s] < lo) lo = F.erases[s];
        if (F.erases[s] > hi) hi = F.erases[s];
    }
    CHECK(lo > 0 && hi - lo <= 2);
}

static void test_power_loss(void)
{
    fresh_flash(0xFF);
    pocket_hist_t h;
    CHECK(pocket_hist_open(&h, &FL, BUDGET));
    append(&h, 1, 100);
    append(&h, 2, 100);
    for (long cut = 0; cut < 128; cut += 7) {
        // Power fails after `cut` of the next record's 128 bytes.
        F.write_budget = cut;
        make_text(3, 100, buf);
        CHECK(!pocket_hist_append(&h, 3, buf, 100, 0, NULL));
        F.write_budget = -1;
        pocket_hist_t r;
        CHECK(pocket_hist_open(&r, &FL, BUDGET));
        CHECK(r.newest.reply_id == 2 && pocket_hist_count(&r) == 2);
        // Writing resumes past the damaged bytes and is found again.
        append(&r, 4, 50);
        pocket_hist_t r2;
        CHECK(pocket_hist_open(&r2, &FL, BUDGET));
        CHECK(r2.newest.reply_id == 4);
        check_entry(&r2, &r2.newest, 4, 50);
        pocket_hist_entry_t p;
        CHECK(pocket_hist_prev(&r2, &r2.newest, &p) && p.reply_id == 2);
        // Back to two records for the next cut.
        fresh_flash(0xFF);
        CHECK(pocket_hist_open(&h, &FL, BUDGET));
        append(&h, 1, 100);
        append(&h, 2, 100);
    }
}

static void test_garbage_and_clear(void)
{
    // A partition never erased holds no valid record.
    fresh_flash(0x00);
    for (uint32_t i = 0; i < SIZE; i++) F.mem[i] = (uint8_t)rnd();
    pocket_hist_t h;
    CHECK(pocket_hist_open(&h, &FL, BUDGET));
    CHECK(pocket_hist_count(&h) == 0);
    append(&h, 7, 20);
    pocket_hist_t r;
    CHECK(pocket_hist_open(&r, &FL, BUDGET) && pocket_hist_count(&r) == 1);
    check_entry(&r, &r.newest, 7, 20);

    CHECK(pocket_hist_clear(&r) && pocket_hist_count(&r) == 0);
    CHECK(pocket_hist_open(&h, &FL, BUDGET) && pocket_hist_count(&h) == 0);
    append(&h, 8, 1);
    CHECK(h.newest.seq == 1);

    // Too large for the partition, or for the u16 length.
    CHECK(!pocket_hist_append(&h, 9, buf, SIZE, 0, NULL));
    CHECK(!pocket_hist_append(&h, 9, buf, 70000, 0, NULL));
}

static void test_crc(void)
{
    // Standard check value of CRC-32/ISO-HDLC.
    CHECK(pocket_hist_crc32(0, "123456789", 9) == 0xCBF43926u);
    // Incremental equals one pass.
    CHECK(pocket_hist_crc32(pocket_hist_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
}

int main(void)
{
    test_crc();
    test_empty_and_basic();
    test_budget_eviction();
    test_wrap_and_wear();
    test_power_loss();
    test_garbage_and_clear();
    CHECK_DONE("test_pocket_hist");
    return 0;
}
