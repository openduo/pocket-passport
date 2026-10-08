// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Packet ring for the pre-roll and the tap-threshold hold: Opus packets of
// variable length, oldest evicted first. Pure C; host-tested. Not thread-safe:
// one task (the encoder) owns it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t head;              // next byte to read
    size_t used;              // bytes in use, length headers included
    uint32_t count;           // packets stored
    uint32_t evicted;         // packets dropped to make room (cumulative)
    uint32_t rejected;        // packets larger than the ring (cumulative)
} pocket_ring_t;

void pocket_ring_init(pocket_ring_t *r, uint8_t *buf, size_t cap);
void pocket_ring_clear(pocket_ring_t *r);
// Stores a packet, evicting the oldest ones if needed. False if the packet can
// never fit (len + 2 > cap) or len is 0.
bool pocket_ring_push(pocket_ring_t *r, const uint8_t *pkt, size_t len);
// Drops the oldest packets until at most max_packets remain.
void pocket_ring_trim(pocket_ring_t *r, uint32_t max_packets);
// Removes the oldest packet into out. Returns its length, 0 if empty. A packet
// longer than cap is dropped and 0 is returned with *dropped set.
size_t pocket_ring_pop(pocket_ring_t *r, uint8_t *out, size_t cap, bool *dropped);
