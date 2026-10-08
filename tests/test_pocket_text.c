// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Host tests: UTF-8 decoding, the missing-glyph policy and truncation.
#include "pocket_check.h"
#include "pocket_text.h"

#include <string.h>

// Pretend font: ASCII, U+4E2D, U+6587, U+25A1, U+2026.
static bool fake_font(uint32_t cp, void *ctx)
{
    (void)ctx;
    return (cp >= 0x20 && cp < 0x7F) || cp == 0x4E2D || cp == 0x6587 || cp == 0x25A1 ||
           cp == 0x2026;
}

static bool ascii_only(uint32_t cp, void *ctx)
{
    (void)ctx;
    return cp >= 0x20 && cp < 0x7F;
}

static size_t san(const char *in, char *out, size_t cap, pocket_glyph_fn fn,
                  pocket_text_stats_t *st)
{
    return pocket_text_sanitize((const uint8_t *)in, strlen(in), out, cap, fn, NULL, st);
}

int main(void)
{
    // Decoder.
    const uint8_t zh[] = { 0xE4, 0xB8, 0xAD };
    size_t pos = 0;
    uint32_t cp = 0;
    CHECK(pocket_utf8_next(zh, 3, &pos, &cp) && cp == 0x4E2D && pos == 3);
    const uint8_t overlong[] = { 0xC0, 0x80 };
    pos = 0;
    CHECK(!pocket_utf8_next(overlong, 2, &pos, &cp) && pos == 1);
    const uint8_t surrogate[] = { 0xED, 0xA0, 0x80 };
    pos = 0;
    CHECK(!pocket_utf8_next(surrogate, 3, &pos, &cp));
    pos = 0;
    CHECK(!pocket_utf8_next(zh, 2, &pos, &cp) && pos == 1);
    const uint8_t emoji[] = { 0xF0, 0x9F, 0x98, 0x80 };
    pos = 0;
    CHECK(pocket_utf8_next(emoji, 4, &pos, &cp) && cp == 0x1F600);

    // Trim never ends inside a character.
    const uint8_t two[] = { 0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87 };
    CHECK(pocket_utf8_trim(two, 6, 6) == 6);
    CHECK(pocket_utf8_trim(two, 6, 5) == 3);
    CHECK(pocket_utf8_trim(two, 6, 4) == 3);
    CHECK(pocket_utf8_trim(two, 6, 3) == 3);
    CHECK(pocket_utf8_trim(two, 6, 2) == 0);

    char out[64];
    pocket_text_stats_t st;

    // Covered text passes through; uncovered code points become U+25A1.
    size_t n = san("A\xE4\xB8\xAD\xE6\x96\x87", out, sizeof out, fake_font, &st);
    CHECK(n == 7 && strcmp(out, "A\xE4\xB8\xAD\xE6\x96\x87") == 0);
    CHECK(st.replaced == 0 && st.invalid == 0 && !st.truncated);
    san("\xE9\xBE\x98" "x", out, sizeof out, fake_font, &st);  // U+9F98 not in font
    CHECK(strcmp(out, "\xE2\x96\xA1" "x") == 0 && st.replaced == 1);
    san("\xF0\x9F\x98\x80", out, sizeof out, fake_font, &st);
    CHECK(strcmp(out, "\xE2\x96\xA1") == 0 && st.replaced == 1);

    // Invalid UTF-8 is visible, not dropped.
    san("a\xFF" "b", out, sizeof out, fake_font, &st);
    CHECK(strcmp(out, "a\xE2\x96\xA1" "b") == 0 && st.invalid == 1);

    // Without U+25A1 in the font the replacement is '?'.
    san("\xE4\xB8\xAD", out, sizeof out, ascii_only, &st);
    CHECK(strcmp(out, "?") == 0 && st.replaced == 1);

    // Controls: CR LF and CR -> LF, TAB -> space, others dropped.
    san("a\r\nb\rc\td\x01" "e", out, sizeof out, NULL, &st);
    CHECK(strcmp(out, "a\nb\nc d" "e") == 0);

    // Truncation at a character boundary with an ellipsis (3 bytes).
    n = san("\xE4\xB8\xAD\xE4\xB8\xAD\xE4\xB8\xAD", out, 8, fake_font, &st);
    CHECK(st.truncated && n == 6);
    CHECK(strcmp(out, "\xE4\xB8\xAD\xE2\x80\xA6") == 0);
    // Exactly fitting text is not truncated.
    n = san("abcdefg", out, 8, fake_font, &st);
    CHECK(!st.truncated && n == 7);
    n = san("abcdefgh", out, 8, fake_font, &st);
    CHECK(st.truncated && strcmp(out, "abcd\xE2\x80\xA6") == 0);
    // Font without the ellipsis falls back to '.'.
    n = san("abcdefgh", out, 8, ascii_only, &st);
    CHECK(strcmp(out, "abcdef.") == 0);

    CHECK(pocket_utf8_put(0x10FFFF, out, 4) == 4);
    CHECK(pocket_utf8_put(0x4E2D, out, 2) == 0);
    CHECK_DONE("test_pocket_text");
    return 0;
}
