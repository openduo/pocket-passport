// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

#include "pocket_text.h"

#include <string.h>

bool pocket_utf8_next(const uint8_t *s, size_t len, size_t *pos, uint32_t *cp)
{
    size_t i = *pos;
    uint8_t b0 = s[i];
    if (b0 < 0x80) {
        *cp = b0;
        *pos = i + 1;
        return true;
    }
    size_t n;
    uint32_t v, min;
    if ((b0 & 0xE0) == 0xC0) { n = 2; v = b0 & 0x1F; min = 0x80; }
    else if ((b0 & 0xF0) == 0xE0) { n = 3; v = b0 & 0x0F; min = 0x800; }
    else if ((b0 & 0xF8) == 0xF0) { n = 4; v = b0 & 0x07; min = 0x10000; }
    else { *pos = i + 1; return false; }
    if (i + n > len) { *pos = i + 1; return false; }
    for (size_t k = 1; k < n; k++) {
        uint8_t b = s[i + k];
        if ((b & 0xC0) != 0x80) { *pos = i + 1; return false; }
        v = (v << 6) | (b & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
        *pos = i + 1;
        return false;
    }
    *cp = v;
    *pos = i + n;
    return true;
}

size_t pocket_utf8_trim(const uint8_t *s, size_t len, size_t max)
{
    if (len <= max) return len;
    size_t n = max;
    // Step back over continuation bytes to the lead byte of the cut character.
    while (n > 0 && (s[n] & 0xC0) == 0x80) n--;
    return n;
}

size_t pocket_utf8_put(uint32_t cp, char *out, size_t cap)
{
    if (cp < 0x80) {
        if (cap < 1) return 0;
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        if (cap < 2) return 0;
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        if (cap < 3) return 0;
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cap < 4) return 0;
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static uint32_t replacement(pocket_glyph_fn has_glyph, void *ctx)
{
    if (!has_glyph || has_glyph(POCKET_TEXT_REPLACEMENT, ctx)) return POCKET_TEXT_REPLACEMENT;
    return '?';
}

size_t pocket_text_sanitize(const uint8_t *in, size_t in_len, char *out, size_t out_cap,
                            pocket_glyph_fn has_glyph, void *ctx, pocket_text_stats_t *stats)
{
    pocket_text_stats_t st = { 0 };
    if (out_cap == 0) {
        if (stats) *stats = st;
        return 0;
    }
    const uint32_t repl = replacement(has_glyph, ctx);
    uint32_t ellipsis = POCKET_TEXT_ELLIPSIS;
    if (has_glyph && !has_glyph(ellipsis, ctx)) ellipsis = '.';
    char ell[4];
    const size_t ell_len = pocket_utf8_put(ellipsis, ell, sizeof ell);

    // Reserve the NUL; when the text does not fit, the tail makes room for
    // the ellipsis.
    const size_t limit = out_cap - 1;
    size_t n = 0;
    size_t pos = 0;
    while (pos < in_len) {
        uint32_t cp;
        if (!pocket_utf8_next(in, in_len, &pos, &cp)) {
            st.invalid++;
            cp = repl;
        } else if (cp == '\r') {
            if (pos < in_len && in[pos] == '\n') pos++;
            cp = '\n';
        } else if (cp == '\t') {
            cp = ' ';
        } else if (cp < 0x20 && cp != '\n') {
            continue;
        } else if (cp >= 0x7F && cp < 0xA0) {
            continue;
        } else if (cp != '\n' && has_glyph && !has_glyph(cp, ctx)) {
            st.replaced++;
            cp = repl;
        }
        char enc[4];
        size_t k = pocket_utf8_put(cp, enc, sizeof enc);
        if (n + k > limit) {
            st.truncated = true;
            break;
        }
        memcpy(out + n, enc, k);
        n += k;
    }
    if (st.truncated) {
        // Drop whole characters until the ellipsis fits.
        while (n > 0 && n + ell_len > limit) {
            do {
                n--;
            } while (n > 0 && ((uint8_t)out[n] & 0xC0) == 0x80);
        }
        if (n + ell_len <= limit) {
            memcpy(out + n, ell, ell_len);
            n += ell_len;
        }
    }
    out[n] = '\0';
    if (stats) *stats = st;
    return n;
}
