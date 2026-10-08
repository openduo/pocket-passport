// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Reply history in a raw flash partition: an append-only ring of records.
// Pure C over three flash callbacks; host-tested with a NOR flash model.
//
// Record (4-byte aligned, may span erase sectors, never the partition end):
//   magic u32 | seq u32 | prev_off u32 | reply_id u32 | text_len u16 |
//   flags u8 | 0xFF | text_crc u32 | head_crc u32 | text (text_len bytes)
// seq counts up by one per record. Before a record enters a sector that it
// does not start inside, that sector is erased, which removes the oldest
// records; a record that lost bytes fails its CRC and ends the history.
// Power loss mid-write leaves a record that fails its CRC: it is ignored, and
// writing resumes at the next sector.
//
// Live history: the newest records, walking back by prev_off, while the sum
// of their text lengths stays within the byte budget. Older records still in
// flash are not shown and are overwritten in time. RAM holds no record text.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POCKET_HIST_MAGIC     0x31524850u  // "PHR1"
#define POCKET_HIST_HEAD_BYTES 28u
#define POCKET_HIST_FLAG_CUT  0x01u        // the text was cut; the full text is on the phone

typedef struct {
    void *ctx;
    // Byte-granular read/write; write only clears bits (NOR flash).
    bool (*read)(void *ctx, uint32_t off, void *buf, size_t len);
    bool (*write)(void *ctx, uint32_t off, const void *buf, size_t len);
    bool (*erase_sector)(void *ctx, uint32_t off);   // off: sector start
    uint32_t size;              // partition bytes, a multiple of sector
    uint32_t sector;            // erase unit
} pocket_hist_flash_t;

typedef struct {
    uint32_t seq;
    uint32_t off;               // record start
    uint32_t prev_off;
    uint32_t reply_id;
    uint32_t text_crc;
    uint16_t text_len;
    uint8_t flags;
} pocket_hist_entry_t;

typedef struct {
    pocket_hist_flash_t fl;
    uint32_t budget;            // live text bytes
    bool any;                   // newest/oldest are valid
    pocket_hist_entry_t newest;
    pocket_hist_entry_t oldest; // oldest live record
    uint32_t head;              // next write offset
} pocket_hist_t;

// Scans the partition and finds the live history. Returns false only when
// the flash cannot be read or the geometry is unusable.
bool pocket_hist_open(pocket_hist_t *h, const pocket_hist_flash_t *fl, uint32_t budget);

// Appends a record and evicts what falls outside the budget. A text longer
// than UINT16_MAX or larger than the partition can hold is refused.
bool pocket_hist_append(pocket_hist_t *h, uint32_t reply_id, const void *text, size_t len,
                        uint8_t flags, pocket_hist_entry_t *out);

// Live record count and the 1-based position of e counted from the newest.
uint32_t pocket_hist_count(const pocket_hist_t *h);
uint32_t pocket_hist_position(const pocket_hist_t *h, const pocket_hist_entry_t *e);

// Neighbours within the live history; false at either end.
bool pocket_hist_prev(const pocket_hist_t *h, const pocket_hist_entry_t *cur,
                      pocket_hist_entry_t *out);
bool pocket_hist_next(const pocket_hist_t *h, const pocket_hist_entry_t *cur,
                      pocket_hist_entry_t *out);

// True if a live record carries reply_id (newest first).
bool pocket_hist_find(const pocket_hist_t *h, uint32_t reply_id, pocket_hist_entry_t *out);

// Reads the text (verified against its CRC); returns its length or -1.
// cap must hold text_len bytes.
int pocket_hist_read_text(const pocket_hist_t *h, const pocket_hist_entry_t *e, void *buf,
                          size_t cap);

// Erases every sector: the history is empty afterwards.
bool pocket_hist_clear(pocket_hist_t *h);

// CRC-32 (IEEE 802.3, reflected), exposed for the tests.
uint32_t pocket_hist_crc32(uint32_t crc, const void *data, size_t len);
