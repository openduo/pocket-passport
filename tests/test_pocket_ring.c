// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Host tests: pre-roll / hold packet ring.
#include "pocket_check.h"
#include "pocket_ring.h"

#include <string.h>

static void push_n(pocket_ring_t *r, uint8_t tag, size_t len)
{
    uint8_t p[64];
    memset(p, tag, len);
    CHECK(pocket_ring_push(r, p, len));
}

static uint8_t pop_tag(pocket_ring_t *r, size_t expect_len)
{
    uint8_t p[64];
    bool dropped;
    size_t n = pocket_ring_pop(r, p, sizeof p, &dropped);
    CHECK(!dropped && n == expect_len);
    for (size_t i = 1; i < n; i++) CHECK(p[i] == p[0]);
    return p[0];
}

int main(void)
{
    uint8_t buf[32];
    pocket_ring_t r;
    pocket_ring_init(&r, buf, sizeof buf);

    uint8_t tmp[64];
    CHECK(pocket_ring_pop(&r, tmp, sizeof tmp, NULL) == 0);
    CHECK(!pocket_ring_push(&r, tmp, 0));
    CHECK(!pocket_ring_push(&r, tmp, 31));  // 31 + 2 > 32
    CHECK(r.rejected == 2);

    // FIFO order, wrap-around and eviction of the oldest packets.
    push_n(&r, 1, 10);  // 12 bytes
    push_n(&r, 2, 10);  // 24
    push_n(&r, 3, 10);  // 36 > 32: evicts 1
    CHECK(r.count == 2 && r.evicted == 1);
    CHECK(pop_tag(&r, 10) == 2);
    push_n(&r, 4, 5);
    push_n(&r, 5, 11);  // fills to 32 bytes and wraps
    CHECK(r.count == 3);
    CHECK(pop_tag(&r, 10) == 3);
    CHECK(pop_tag(&r, 5) == 4);
    CHECK(pop_tag(&r, 11) == 5);
    CHECK(r.count == 0 && r.used == 0);

    // Many small packets across many wraps keep order.
    uint8_t next = 0;
    uint8_t expect = 0;
    for (int i = 0; i < 200; i++) {
        push_n(&r, next++, (size_t)(i % 7) + 1);
        if (i % 3 == 2) {
            pocket_ring_trim(&r, 2);
            while (r.count) {
                uint8_t p[64];
                size_t n = pocket_ring_pop(&r, p, sizeof p, NULL);
                CHECK(n > 0);
                CHECK((uint8_t)(p[0] - expect) < 3);
                expect = (uint8_t)(p[0] + 1);
            }
        }
    }

    // Trim keeps the newest packets.
    pocket_ring_clear(&r);
    for (uint8_t t = 0; t < 5; t++) push_n(&r, t, 3);
    pocket_ring_trim(&r, 2);
    CHECK(r.count == 2);
    CHECK(pop_tag(&r, 3) == 3);
    CHECK(pop_tag(&r, 3) == 4);

    // Pop into a too-small buffer drops the packet and reports it.
    push_n(&r, 9, 20);
    bool dropped = false;
    CHECK(pocket_ring_pop(&r, tmp, 4, &dropped) == 0 && dropped && r.count == 0);
    CHECK_DONE("test_pocket_ring");
    return 0;
}
