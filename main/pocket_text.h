// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// UTF-8 handling for text shown on the screen: decoding, the missing-glyph
// policy and truncation at character boundaries. Pure C; host-tested.
//
// Missing-glyph policy: a code point the font chain cannot draw is replaced
// by U+25A1 WHITE SQUARE (a real glyph in the bundled CJK font), so an
// unsupported character is visible and counted, never silently dropped. If
// the font lacks U+25A1 too, '?' is used. Invalid UTF-8 follows the same rule.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POCKET_TEXT_REPLACEMENT 0x25A1u
#define POCKET_TEXT_ELLIPSIS    0x2026u

typedef bool (*pocket_glyph_fn)(uint32_t codepoint, void *ctx);

typedef struct {
    uint32_t replaced;        // code points without a glyph
    uint32_t invalid;         // malformed UTF-8 sequences
    bool truncated;           // output buffer too small; ellipsis appended
} pocket_text_stats_t;

// Decodes one code point at *pos. Returns false for a malformed or truncated
// sequence (overlong, surrogate, > U+10FFFF) and advances *pos past one byte.
bool pocket_utf8_next(const uint8_t *s, size_t len, size_t *pos, uint32_t *cp);

// Largest prefix length <= max that does not end inside a multi-byte
// sequence of otherwise valid UTF-8.
size_t pocket_utf8_trim(const uint8_t *s, size_t len, size_t max);

// Encodes cp; returns bytes written (1-4) or 0 if cap is too small.
size_t pocket_utf8_put(uint32_t cp, char *out, size_t cap);

// Produces a NUL-terminated, valid UTF-8 copy of in that only contains code
// points accepted by has_glyph (NULL accepts all), plus '\n'. CR LF and lone
// CR become '\n', TAB becomes a space, other control characters are dropped.
// If out_cap is too small the text is cut at a character boundary and an
// ellipsis is appended. Returns the byte length without the NUL.
size_t pocket_text_sanitize(const uint8_t *in, size_t in_len, char *out, size_t out_cap,
                            pocket_glyph_fn has_glyph, void *ctx, pocket_text_stats_t *stats);
