// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

#include "pocket_ui.h"

#include "pocket_audio.h"
#include "pocket_config.h"
#include "pocket_mark.h"
#include "pocket_strings.h"
#include "pocket_text.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "pocket_ui";

LV_FONT_DECLARE(pocket_cjk_20);
LV_FONT_DECLARE(pocket_digits_44);

// Colours: dark theme tokens of the DuoDuo web app (openduo/ambient,
// packages/channel-ambient/web/tokens/colors.css).
#define C_BG             0x0e0e0d
#define C_BRAND          0x1fd9de
#define C_BRAND_TINT     0x0c2b2e
#define C_BRAND_DIM      0x2a6c6f
#define C_KICKER         0x6ec5cb
#define C_STRONG         0xf5f4ed
#define C_PRIMARY        0xdddcd5
#define C_BODY           0xb4b3ac
#define C_MUTED          0x8d8c86
#define C_FAINT          0x6f6e69
#define C_HAIRLINE       0x282826
#define C_ATTENTION      0xd9a066
#define C_ATTENTION_TINT 0x2a2015

// Layout (px, 240x320 panel with a 30 px corner mask).
#define SCR_W            240
#define TEXT_W           200
// Text column on screens with the key rail: leaves the rail's 54 px free.
#define TEXT_W_RAIL      170
#define MARK_Y           40
#define TITLE_Y          176
#define HINT_Y           282
#define SET_ROW_Y        72
#define SET_ROW_PITCH    40
#define SET_ROW_H        36
// Settings rows on screen at once; the list scrolls to keep the selection
// visible. Five 36 px rows on a 40 px pitch fit between the title and the
// hint (y 72..268).
#define SET_ROWS_VISIBLE 5
_Static_assert(POCKET_SET_COUNT > SET_ROWS_VISIBLE, "the settings scroll indicator divides by the overflow");
#define HEADER_Y         36
// Header title right of the small mark, 10 px apart.
#define HEAD_TITLE_X     (REPLY_X + POCKET_MARK_SMALL_PX + 10)
#define DIVIDER_Y        88
#define REPLY_X          20
#define REPLY_Y          94
#define REPLY_W          200
#define REPLY_H          188   // 7 lines of 26 px plus padding
#define SCROLLBAR_X      226
// Right-edge key rail: action labels at the physical keys' heights. Measured
// on the board: the case keys top to bottom are UP, DOWN, OK (0/1k/2.2k on
// the ADC ladder) — NOT the UP, OK, DOWN order one would guess; the case keys
// are unlabeled. Only screens offering a key choice show it. Slot order
// matches the physical order, so the Y table stays monotonic.
#define KEY_RAIL_RIGHT   14
#define KEY_RAIL_Y       { 96, 156, 216 }  // [UP]=top, [DOWN]=middle, [OK]=bottom
#define KEY_SLOT_UP      0
#define KEY_SLOT_DOWN    1
#define KEY_SLOT_OK      2
#define WORK_DOTS        3
#define WORK_DOT_PX      6
#define WORK_DOT_GAP     6
// Peak treated as full level of the voice bars (about -12 dBFS): speech at
// arm's length moves the bars most of their range. Display only.
#define LEVEL_FULL_SCALE 8192
// Refresh of the recording clock and voice bars. Display only, a visual choice:
// the clock lags by at most this, and the bars follow every 5th audio frame.
#define REC_REFRESH_MS   100

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *dot, *battery;
    lv_obj_t *mark_large, *bars, *bar[POCKET_MARK_BARS], *col, *title, *sub, *clock, *hint,
        *device;
    lv_obj_t *code;
    lv_obj_t *mark_small, *head_title, *divider;
    lv_obj_t *reply_box, *reply_label, *scrollbar, *hist;
    lv_obj_t *kicker, *transcript;
    lv_obj_t *set_title, *rows[SET_ROWS_VISIBLE], *row_label[SET_ROWS_VISIBLE],
        *row_value[SET_ROWS_VISIBLE], *row_bar[SET_ROWS_VISIBLE];
    lv_obj_t *key_rail[3];      // slots in physical order: UP, DOWN, OK
    uint8_t set_top;            // first setting shown in the top row
    lv_obj_t *banner, *banner_label;
    lv_obj_t *work_dots, *work_dot[WORK_DOTS];
    lv_timer_t *rec_timer, *work_timer;
    uint8_t work_step;
    bool rec_active;
    uint32_t rec_start_ms;
    // End points of the live voice bars; lv_line keeps a pointer to them.
    lv_point_precise_t bar_pts[POCKET_MARK_BARS][2];
} ui_t;

static ui_t u;
// UI language of the copy (pocket_lang_t), from the model at each render.
// Brain text (transcript, reply) is shown as sent.
static uint8_t s_lang = POCKET_LANG_DEFAULT;
#define T(id) (s_lang == POCKET_LANG_EN ? POCKET_STR_##id##_EN : POCKET_STR_##id)
// The only RAM copy of the shown reply: sanitized text, then the "full text
// on the phone" line when it was cut. Text: the largest message plus its NUL.
// REPLY_MORE_RESERVE: the newline and POCKET_STR_REPLY_MORE in the longer of
// the two languages, which reuse the text's NUL position and add their own. Sanitizing a valid message never
// grows it; replaced invalid bytes can, and are then cut with an ellipsis.
#define MORE_ZH_BYTES (sizeof("\n" POCKET_STR_REPLY_MORE) - 1)
#define MORE_EN_BYTES (sizeof("\n" POCKET_STR_REPLY_MORE_EN) - 1)
#define REPLY_MORE_RESERVE (MORE_ZH_BYTES > MORE_EN_BYTES ? MORE_ZH_BYTES : MORE_EN_BYTES)
static char s_reply_text[POCKET_MAX_MSG_BYTES + 1 + REPLY_MORE_RESERVE];
static size_t s_reply_len;      // sanitized text without the line
static bool s_reply_cut;
// Transcript plus its NUL; a longer one is cut with an ellipsis.
static char s_transcript_text[POCKET_TRANSCRIPT_MAX_BYTES + 1];

bool pocket_ui_has_glyph(uint32_t cp)
{
    lv_font_glyph_dsc_t g = { 0 };
    return lv_font_get_glyph_dsc(&pocket_cjk_20, &g, cp, 0) && !g.is_placeholder;
}

static bool glyph_cb(uint32_t cp, void *ctx)
{
    (void)ctx;
    return pocket_ui_has_glyph(cp);
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text_static(l, "");
    return l;
}

static lv_obj_t *centered_label(lv_obj_t *parent, uint32_t color, int32_t y, int32_t w)
{
    lv_obj_t *l = label(parent, &pocket_cjk_20, color);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void show(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void set_text(lv_obj_t *l, const char *text)
{
    const char *cur = lv_label_get_text(l);
    if (cur != text && (cur == NULL || strcmp(cur, text) != 0)) lv_label_set_text_static(l, text);
}

// Voice bars of the recording screen at the current mic level, ms into the
// press. Motion and geometry come from tools/pocket/gen_mark.py.
static void bars_update(uint32_t ms)
{
    uint32_t peak = pocket_audio_level();
    const float level = peak >= LEVEL_FULL_SCALE ? 1.0f : (float)peak / LEVEL_FULL_SCALE;
    const float t = ms / 1000.0f;
    for (int i = 0; i < POCKET_MARK_BARS; i++) {
        const pocket_mark_bar_t *b = &pocket_mark_live_bars[i];
        const float wave = 0.5f + 0.5f * sinf(t * POCKET_MARK_WAVE_RATE + i * POCKET_MARK_WAVE_PHASE);
        const float half = b->rest + POCKET_MARK_TALK_PX * level *
                                         (POCKET_MARK_TALK_BASE + POCKET_MARK_TALK_SWING * wave);
        const lv_point_precise_t p[2] = {
            { (int32_t)lroundf(b->x - b->ax * half), (int32_t)lroundf(b->y - b->ay * half) },
            { (int32_t)lroundf(b->x + b->ax * half), (int32_t)lroundf(b->y + b->ay * half) },
        };
        // An unchanged bar (a silent mic) is not redrawn.
        if (memcmp(p, u.bar_pts[i], sizeof p) == 0 && lv_line_get_point_count(u.bar[i])) continue;
        memcpy(u.bar_pts[i], p, sizeof p);
        lv_line_set_points(u.bar[i], u.bar_pts[i], 2);
    }
}

static void rec_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!u.rec_active) return;
    static char clock[12];
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t s = (now - u.rec_start_ms) / 1000;
    snprintf(clock, sizeof clock, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    lv_label_set_text(u.clock, clock);
    bars_update(now - u.rec_start_ms);
}

// Working-state animation: one dot of three lit at a time. The timer runs
// only while the working screen is shown and the brain is active.
static void work_dots_set(int lit)
{
    for (int i = 0; i < WORK_DOTS; i++) {
        lv_obj_set_style_bg_opa(u.work_dot[i], i == lit ? LV_OPA_COVER : LV_OPA_30, 0);
    }
}

static void work_timer_cb(lv_timer_t *t)
{
    (void)t;
    u.work_step = (uint8_t)((u.work_step + 1) % WORK_DOTS);
    work_dots_set(u.work_step);
}

esp_err_t pocket_ui_init(void)
{
    u.scr = lv_screen_active();
    lv_obj_clean(u.scr);
    lv_obj_set_style_bg_color(u.scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(u.scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(u.scr, LV_OBJ_FLAG_SCROLLABLE);

    // Status: link dot (left), battery (right, default top-right placement).
    u.dot = plain(u.scr);
    lv_obj_set_size(u.dot, 8, 8);
    lv_obj_set_style_radius(u.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(u.dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(u.dot, 28, 15);
    u.battery = label(u.scr, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(u.battery, LV_ALIGN_TOP_RIGHT, -28, 10);

    // State screens.
    u.mark_large = lv_image_create(u.scr);
    lv_obj_align(u.mark_large, LV_ALIGN_TOP_MID, 0, MARK_Y);
    // Live voice bars over the recording frame, in a box that holds them at
    // full level, so a refresh redraws only that box.
    u.bars = plain(u.scr);
    lv_obj_set_pos(u.bars, (SCR_W - POCKET_MARK_LARGE_PX) / 2 + POCKET_MARK_BOX_X,
                   MARK_Y + POCKET_MARK_BOX_Y);
    lv_obj_set_size(u.bars, POCKET_MARK_BOX_W, POCKET_MARK_BOX_H);
    for (int i = 0; i < POCKET_MARK_BARS; i++) {
        lv_obj_t *l = lv_line_create(u.bars);
        lv_obj_set_style_line_width(l, POCKET_MARK_BAR_WIDTH, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        lv_obj_set_style_line_color(
            l, lv_color_hex(i == POCKET_MARK_BARS - 1 ? POCKET_MARK_ACCENT_RGB : POCKET_MARK_BAR_RGB), 0);
        u.bar[i] = l;
    }
    u.col = plain(u.scr);
    lv_obj_set_size(u.col, TEXT_W, LV_SIZE_CONTENT);
    lv_obj_align(u.col, LV_ALIGN_TOP_MID, 0, TITLE_Y);
    lv_obj_set_flex_flow(u.col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(u.col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(u.col, 4, 0);
    u.title = centered_label(u.col, C_STRONG, 0, LV_PCT(100));
    u.sub = centered_label(u.col, C_MUTED, 0, LV_PCT(100));
    u.clock = label(u.col, &pocket_cjk_20, C_BRAND);
    u.hint = centered_label(u.scr, C_FAINT, HINT_Y, TEXT_W);
    u.device = label(u.scr, &lv_font_montserrat_14, C_FAINT);
    lv_obj_align(u.device, LV_ALIGN_TOP_MID, 0, HINT_Y + 2);

    // Pairing code.
    u.code = label(u.scr, &pocket_digits_44, C_BRAND);
    lv_obj_align(u.code, LV_ALIGN_TOP_MID, 0, 104);

    // Key rail.
    const int32_t rail_y[3] = KEY_RAIL_Y;
    for (int i = 0; i < 3; i++) {
        u.key_rail[i] = label(u.scr, &pocket_cjk_20, C_FAINT);
        lv_obj_align(u.key_rail[i], LV_ALIGN_TOP_RIGHT, -KEY_RAIL_RIGHT, rail_y[i]);
    }

    // Header for the transcript and reply screens.
    u.mark_small = lv_image_create(u.scr);
    lv_obj_set_pos(u.mark_small, REPLY_X, HEADER_Y);
    u.head_title = label(u.scr, &pocket_cjk_20, C_STRONG);
    lv_obj_set_pos(u.head_title, HEAD_TITLE_X, HEADER_Y + 10);
    u.divider = plain(u.scr);
    lv_obj_set_size(u.divider, REPLY_W, 1);
    lv_obj_set_pos(u.divider, REPLY_X, DIVIDER_Y);
    lv_obj_set_style_bg_color(u.divider, lv_color_hex(C_HAIRLINE), 0);
    lv_obj_set_style_bg_opa(u.divider, LV_OPA_COVER, 0);

    // Reply: a clipped box scrolled by the model, own scroll indicator.
    u.reply_box = plain(u.scr);
    lv_obj_set_pos(u.reply_box, REPLY_X, REPLY_Y);
    lv_obj_set_size(u.reply_box, REPLY_W, REPLY_H);
    lv_obj_add_flag(u.reply_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(u.reply_box, LV_SCROLLBAR_MODE_OFF);
    u.reply_label = label(u.reply_box, &pocket_cjk_20, C_PRIMARY);
    lv_obj_set_width(u.reply_label, REPLY_W);
    lv_label_set_long_mode(u.reply_label, LV_LABEL_LONG_WRAP);
    // Position among the stored replies, right of the reply header.
    u.hist = label(u.scr, &lv_font_montserrat_14, C_FAINT);
    lv_obj_align(u.hist, LV_ALIGN_TOP_RIGHT, -24, HEADER_Y + 14);
    u.scrollbar = plain(u.scr);
    lv_obj_set_style_bg_color(u.scrollbar, lv_color_hex(C_BRAND_DIM), 0);
    lv_obj_set_style_bg_opa(u.scrollbar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(u.scrollbar, 2, 0);

    // Transcript.
    u.kicker = label(u.scr, &pocket_cjk_20, C_KICKER);
    lv_obj_set_pos(u.kicker, REPLY_X, 98);
    u.transcript = label(u.scr, &pocket_cjk_20, C_BODY);
    lv_obj_set_pos(u.transcript, REPLY_X, 126);
    lv_obj_set_size(u.transcript, REPLY_W, 26 * 6);
    lv_label_set_long_mode(u.transcript, LV_LABEL_LONG_DOT);

    // Settings.
    u.set_title = label(u.scr, &pocket_cjk_20, C_STRONG);
    lv_obj_set_pos(u.set_title, 24, 40);
    // Rows between the title (y 40) and the hint (HINT_Y): 36 px rows on a
    // 40 px pitch end at y 268.
    for (int i = 0; i < SET_ROWS_VISIBLE; i++) {
        lv_obj_t *r = plain(u.scr);
        lv_obj_set_pos(r, 16, SET_ROW_Y + i * SET_ROW_PITCH);
        lv_obj_set_size(r, 208, SET_ROW_H);
        lv_obj_set_style_radius(r, 8, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(C_BRAND_TINT), 0);
        u.rows[i] = r;
        u.row_bar[i] = plain(r);
        lv_obj_set_size(u.row_bar[i], 4, 24);
        lv_obj_set_pos(u.row_bar[i], 0, (SET_ROW_H - 24) / 2);
        lv_obj_set_style_bg_color(u.row_bar[i], lv_color_hex(C_BRAND), 0);
        lv_obj_set_style_bg_opa(u.row_bar[i], LV_OPA_COVER, 0);
        u.row_label[i] = label(r, &pocket_cjk_20, C_BODY);
        lv_obj_align(u.row_label[i], LV_ALIGN_LEFT_MID, 14, 0);
        u.row_value[i] = label(r, &pocket_cjk_20, C_MUTED);
        lv_obj_align(u.row_value[i], LV_ALIGN_RIGHT_MID, -14, 0);
    }

    // Server banner.
    u.banner = plain(u.scr);
    lv_obj_set_size(u.banner, 168, 28);
    lv_obj_align(u.banner, LV_ALIGN_TOP_MID, 0, 284);
    lv_obj_set_style_radius(u.banner, 8, 0);
    lv_obj_set_style_bg_color(u.banner, lv_color_hex(C_ATTENTION_TINT), 0);
    lv_obj_set_style_bg_opa(u.banner, LV_OPA_COVER, 0);
    u.banner_label = label(u.banner, &pocket_cjk_20, C_ATTENTION);
    lv_obj_center(u.banner_label);

    // Working-state dots, placed after the phase label at render time.
    u.work_dots = plain(u.scr);
    lv_obj_set_size(u.work_dots, WORK_DOTS * WORK_DOT_PX + (WORK_DOTS - 1) * WORK_DOT_GAP,
                    WORK_DOT_PX);
    for (int i = 0; i < WORK_DOTS; i++) {
        lv_obj_t *d = plain(u.work_dots);
        lv_obj_set_size(d, WORK_DOT_PX, WORK_DOT_PX);
        lv_obj_set_pos(d, i * (WORK_DOT_PX + WORK_DOT_GAP), 0);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(C_BRAND), 0);
        u.work_dot[i] = d;
    }
    work_dots_set(0);

    u.rec_timer = lv_timer_create(rec_timer_cb, REC_REFRESH_MS, NULL);
    u.work_timer = lv_timer_create(work_timer_cb, POCKET_WORK_ANIM_STEP_MS, NULL);
    if (!u.rec_timer || !u.work_timer) return ESP_ERR_NO_MEM;
    lv_timer_pause(u.rec_timer);
    lv_timer_pause(u.work_timer);
    return ESP_OK;
}

static void hide_all(void)
{
    lv_obj_t *all[] = { u.mark_large, u.bars, u.col, u.sub, u.clock, u.hint, u.device, u.code,
                        u.mark_small, u.head_title, u.divider, u.reply_box, u.scrollbar, u.hist,
                        u.kicker, u.transcript, u.set_title, u.banner, u.work_dots,
                        u.key_rail[0], u.key_rail[1], u.key_rail[2] };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) show(all[i], false);
    for (int i = 0; i < SET_ROWS_VISIBLE; i++) show(u.rows[i], false);
    // Back to the full-width text column the key-rail screens narrow.
    lv_obj_set_width(u.col, TEXT_W);
    lv_obj_align(u.col, LV_ALIGN_TOP_MID, 0, TITLE_Y);
}

// One key-rail slot: the action of the physical key at the same height.
static void rail(int slot, const char *text, uint32_t color)
{
    set_text(u.key_rail[slot], text);
    lv_obj_set_style_text_color(u.key_rail[slot], lv_color_hex(color), 0);
    show(u.key_rail[slot], true);
}

static void state_screen(pocket_mark_t mood, const char *title, const char *sub,
                         const char *hint, uint32_t title_color, uint32_t sub_color)
{
    lv_image_set_src(u.mark_large, pocket_marks_large[mood]);
    show(u.mark_large, true);
    show(u.col, true);
    set_text(u.title, title);
    lv_obj_set_style_text_color(u.title, lv_color_hex(title_color), 0);
    if (sub) {
        set_text(u.sub, sub);
        lv_obj_set_style_text_color(u.sub, lv_color_hex(sub_color), 0);
        show(u.sub, true);
    }
    if (hint) {
        set_text(u.hint, hint);
        show(u.hint, true);
    }
}

static void header(pocket_mark_t mood, const char *title, uint32_t color)
{
    lv_image_set_src(u.mark_small, pocket_marks_small[mood]);
    show(u.mark_small, true);
    set_text(u.head_title, title);
    lv_obj_set_style_text_color(u.head_title, lv_color_hex(color), 0);
    show(u.head_title, true);
    show(u.divider, true);
}

static void reply_label_update(void)
{
    if (s_reply_cut) {
        snprintf(s_reply_text + s_reply_len, sizeof s_reply_text - s_reply_len, "\n%s",
                 T(REPLY_MORE));
    }
    lv_label_set_text_static(u.reply_label, s_reply_text);
}

void pocket_ui_set_reply(const uint8_t *text, size_t len, bool truncated)
{
    pocket_text_stats_t st;
    s_reply_len = pocket_text_sanitize(text, len, s_reply_text,
                                       sizeof s_reply_text - REPLY_MORE_RESERVE, glyph_cb, NULL, &st);
    s_reply_cut = truncated || st.truncated;
    if (st.replaced || st.invalid) {
        ESP_LOGW(TAG, "reply: %lu code points without a glyph, %lu invalid",
                 (unsigned long)st.replaced, (unsigned long)st.invalid);
    }
    reply_label_update();
}

char *pocket_ui_reply_buffer(size_t *cap)
{
    *cap = sizeof s_reply_text - REPLY_MORE_RESERVE;
    return s_reply_text;
}

void pocket_ui_reply_loaded(size_t len, bool cut)
{
    s_reply_len = len < sizeof s_reply_text - REPLY_MORE_RESERVE ? len : 0;
    s_reply_text[s_reply_len] = 0;
    s_reply_cut = cut;
    reply_label_update();
}

const char *pocket_ui_reply_text(size_t *len, bool *cut)
{
    *len = s_reply_len;
    *cut = s_reply_cut;
    return s_reply_text;
}

void pocket_ui_set_transcript(const uint8_t *text, size_t len)
{
    pocket_text_sanitize(text, len, s_transcript_text, sizeof s_transcript_text, glyph_cb, NULL,
                         NULL);
    lv_label_set_text_static(u.transcript, s_transcript_text);
}

static void render_reply(pocket_model_t *m)
{
    header(m->reply_final ? POCKET_MARK_IDLE : POCKET_MARK_SPEAKING,
           m->reply_final ? T(REPLY_NAME) : T(REPLY_STREAMING),
           m->reply_final ? C_STRONG : C_KICKER);
    show(u.reply_box, true);
    if (m->hist_pos && m->hist_count > 1) {
        static char pos[12];
        snprintf(pos, sizeof pos, "%u/%u", (unsigned)m->hist_pos, (unsigned)m->hist_count);
        lv_label_set_text(u.hist, pos);
        show(u.hist, true);
    }
    lv_obj_update_layout(u.reply_box);
    const int32_t content_h = lv_obj_get_height(u.reply_label);
    const int32_t line_h = pocket_cjk_20.line_height;
    pocket_model_reply_metrics(m, content_h, REPLY_H, line_h);
    lv_obj_scroll_to_y(u.reply_box, m->scroll_px, LV_ANIM_OFF);

    if (content_h > REPLY_H) {
        int32_t bar_h = REPLY_H * REPLY_H / content_h;
        if (bar_h < 16) bar_h = 16;
        int32_t max = pocket_model_scroll_max(m);
        int32_t top = REPLY_Y + (max ? (REPLY_H - bar_h) * m->scroll_px / max : 0);
        lv_obj_set_pos(u.scrollbar, SCROLLBAR_X, top);
        lv_obj_set_size(u.scrollbar, 3, bar_h);
        show(u.scrollbar, true);
    }
}

static void render_work(const pocket_model_t *m)
{
    const char *title = T(WORK_RECEIVED);
    if (m->work_phase == POCKET_WORK_THINKING) title = T(WORK_THINKING);
    else if (m->work_phase == POCKET_WORK_TOOL) title = T(WORK_TOOL);
    header(POCKET_MARK_THINKING, title, C_STRONG);
    lv_obj_update_layout(u.head_title);
    lv_obj_align_to(u.work_dots, u.head_title, LV_ALIGN_OUT_RIGHT_MID, 10, 2);
    show(u.work_dots, true);
    if (!m->work_active) work_dots_set(-1);
}

static const char *const s_setting_names_zh[POCKET_SET_COUNT] = {
    [POCKET_SET_PREROLL] = POCKET_STR_SET_PREROLL,
    [POCKET_SET_BRIGHTNESS] = POCKET_STR_SET_BRIGHTNESS,
    [POCKET_SET_SCREEN_OFF] = POCKET_STR_SET_SCREEN_OFF,
    [POCKET_SET_DEEP_SLEEP] = POCKET_STR_SET_DEEP_SLEEP,
    [POCKET_SET_ALERT_SCREEN] = POCKET_STR_SET_ALERT_SCREEN,
    [POCKET_SET_ALERT_TONE] = POCKET_STR_SET_ALERT_TONE,
    [POCKET_SET_REPAIR] = POCKET_STR_SET_REPAIR,
    [POCKET_SET_BACK] = POCKET_STR_SET_BACK,
};
static const char *const s_setting_names_en[POCKET_SET_COUNT] = {
    [POCKET_SET_PREROLL] = POCKET_STR_SET_PREROLL_EN,
    [POCKET_SET_BRIGHTNESS] = POCKET_STR_SET_BRIGHTNESS_EN,
    [POCKET_SET_SCREEN_OFF] = POCKET_STR_SET_SCREEN_OFF_EN,
    [POCKET_SET_DEEP_SLEEP] = POCKET_STR_SET_DEEP_SLEEP_EN,
    [POCKET_SET_ALERT_SCREEN] = POCKET_STR_SET_ALERT_SCREEN_EN,
    [POCKET_SET_ALERT_TONE] = POCKET_STR_SET_ALERT_TONE_EN,
    [POCKET_SET_REPAIR] = POCKET_STR_SET_REPAIR_EN,
    [POCKET_SET_BACK] = POCKET_STR_SET_BACK_EN,
};

// Value text of one setting into out.
static void setting_value(const pocket_model_t *m, int set, char *out, size_t cap)
{
    uint32_t v;
    switch (set) {
    case POCKET_SET_PREROLL:
        snprintf(out, cap, "%s", m->preroll_on ? T(ON) : T(OFF));
        break;
    case POCKET_SET_BRIGHTNESS:
        snprintf(out, cap, "%u%%", (unsigned)pocket_model_backlight_pct(m));
        break;
    case POCKET_SET_SCREEN_OFF:
        v = pocket_model_screen_off_s(m);
        if (v) snprintf(out, cap, "%u %s", (unsigned)v, T(SECONDS));
        else snprintf(out, cap, "%s", T(NEVER));
        break;
    case POCKET_SET_DEEP_SLEEP:
        v = pocket_model_deep_sleep_s(m);
        if (!v) snprintf(out, cap, "%s", T(NEVER));
        else if (v % 3600 == 0) snprintf(out, cap, "%u %s", (unsigned)(v / 3600), T(HOURS));
        else snprintf(out, cap, "%u %s", (unsigned)(v / 60), T(MINUTES));
        break;
    case POCKET_SET_ALERT_SCREEN:
        snprintf(out, cap, "%s", m->alert_screen ? T(ON) : T(OFF));
        break;
    case POCKET_SET_ALERT_TONE:
        snprintf(out, cap, "%s", m->alert_tone ? T(ON) : T(OFF));
        break;
    default:
        out[0] = 0;
        break;
    }
}

static void render_settings(const pocket_model_t *m)
{
    static char values[SET_ROWS_VISIBLE][16];
    show(u.set_title, true);
    const int sel = m->settings_sel;
    if (sel < u.set_top) u.set_top = (uint8_t)sel;
    if (sel >= u.set_top + SET_ROWS_VISIBLE) u.set_top = (uint8_t)(sel - SET_ROWS_VISIBLE + 1);
    for (int i = 0; i < SET_ROWS_VISIBLE; i++) {
        const int set = u.set_top + i;
        if (set >= POCKET_SET_COUNT) continue;
        const bool on = set == sel;
        set_text(u.row_label[i],
                 s_lang == POCKET_LANG_EN ? s_setting_names_en[set] : s_setting_names_zh[set]);
        setting_value(m, set, values[i], sizeof values[i]);
        lv_label_set_text(u.row_value[i], values[i]);
        show(u.rows[i], true);
        lv_obj_set_style_bg_opa(u.rows[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        show(u.row_bar[i], on);
        lv_obj_set_style_text_color(u.row_label[i], lv_color_hex(on ? C_STRONG : C_BODY), 0);
        lv_obj_set_style_text_color(u.row_value[i], lv_color_hex(on ? C_BRAND : C_MUTED), 0);
    }
    // Scroll indicator beside the rows when the list is longer than the view.
    const int32_t view_h = SET_ROWS_VISIBLE * SET_ROW_PITCH - (SET_ROW_PITCH - SET_ROW_H);
    const int32_t bar_h = view_h * SET_ROWS_VISIBLE / POCKET_SET_COUNT;
    const int32_t top = SET_ROW_Y + (view_h - bar_h) * u.set_top / (POCKET_SET_COUNT - SET_ROWS_VISIBLE);
    lv_obj_set_pos(u.scrollbar, SCROLLBAR_X, top);
    lv_obj_set_size(u.scrollbar, 3, bar_h);
    show(u.scrollbar, true);
    set_text(u.hint, T(SETTINGS_HINT));
    show(u.hint, true);
}

static void render_notice(pocket_notice_t n)
{
    switch (n) {
    case POCKET_NOTICE_ASR_FAILED:
        state_screen(POCKET_MARK_UNAVAILABLE, T(ASR_TITLE), T(ASR_SUB),
                     T(NOTICE_HINT), C_STRONG, C_MUTED);
        break;
    case POCKET_NOTICE_SEND_FAILED:
        state_screen(POCKET_MARK_UNAVAILABLE, T(SEND_TITLE), T(SEND_SUB),
                     T(NOTICE_HINT), C_STRONG, C_MUTED);
        break;
    case POCKET_NOTICE_NO_SERVER:
        state_screen(POCKET_MARK_UNAVAILABLE, T(SERVER_TITLE), T(SERVER_SUB),
                     T(NOTICE_HINT), C_STRONG, C_MUTED);
        break;
    case POCKET_NOTICE_EMPTY:
    default:
        state_screen(POCKET_MARK_UNAVAILABLE, T(EMPTY_TITLE), T(EMPTY_SUB),
                     T(NOTICE_HINT), C_STRONG, C_MUTED);
        break;
    }
}

void pocket_ui_render(pocket_model_t *m, const char *device_name)
{
    static char battery[8];
    static char code[16];
    if (m->lang != s_lang) {
        s_lang = m->lang;
        if (s_reply_cut) reply_label_update();  // the "full text on the phone" line
    }
    set_text(u.kicker, T(YOU_SAID));
    set_text(u.set_title, T(SETTINGS_TITLE));
    set_text(u.banner_label, T(BANNER_SERVER));
    hide_all();

    lv_obj_set_style_bg_color(u.dot, lv_color_hex(m->link.ready ? C_BRAND : C_FAINT), 0);
    if (m->battery_pct <= 100) {
        snprintf(battery, sizeof battery, "%u%%", (unsigned)m->battery_pct);
        lv_label_set_text(u.battery, battery);
        show(u.battery, true);
    } else {
        show(u.battery, false);
    }

    u.rec_active = m->screen == POCKET_SCR_RECORDING;
    // Timers run only while their screen is shown, so an idle LVGL task
    // sleeps and the chip can enter light sleep.
    if (u.rec_active) lv_timer_resume(u.rec_timer);
    else lv_timer_pause(u.rec_timer);
    if (m->screen == POCKET_SCR_THINKING && m->work_active) lv_timer_resume(u.work_timer);
    else lv_timer_pause(u.work_timer);
    switch (m->screen) {
    case POCKET_SCR_BOOT:
        state_screen(POCKET_MARK_IDLE, T(APP_NAME), T(BOOT), NULL,
                     C_STRONG, C_MUTED);
        break;
    case POCKET_SCR_PAIRING:
        state_screen(POCKET_MARK_UNAVAILABLE, T(PAIR_TITLE), T(PAIR_SUB), NULL,
                     C_STRONG, C_MUTED);
        set_text(u.device, device_name);
        show(u.device, true);
        break;
    case POCKET_SCR_PAIR_CONFIRM:
        show(u.col, true);
        lv_obj_set_width(u.col, TEXT_W_RAIL);
        lv_obj_align(u.col, LV_ALIGN_TOP_MID, 0, 182);
        if (m->passkey) {
            snprintf(code, sizeof code, "%03lu %03lu", (unsigned long)(m->passkey / 1000),
                     (unsigned long)(m->passkey % 1000));
            lv_label_set_text(u.code, code);
            show(u.code, true);
            set_text(u.title, T(PAIR_CODE_SUB));
            set_text(u.hint, T(PAIR_CODE_HINT));
            show(u.hint, true);
            rail(KEY_SLOT_OK, T(KEY_OK), C_BRAND);
            rail(KEY_SLOT_DOWN, T(KEY_DOWN), C_FAINT);
        } else {
            set_text(u.title, T(PAIR_WAIT));
        }
        lv_obj_set_style_text_color(u.title, lv_color_hex(C_STRONG), 0);
        break;
    case POCKET_SCR_OFFLINE:
        if (m->refused_flash) {
            state_screen(POCKET_MARK_UNAVAILABLE, T(OFFLINE_TITLE), T(OFFLINE_REFUSED),
                         NULL, C_ATTENTION, C_ATTENTION);
        } else {
            state_screen(POCKET_MARK_UNAVAILABLE, T(OFFLINE_TITLE), T(OFFLINE_SUB),
                         NULL, C_STRONG, C_MUTED);
        }
        break;
    case POCKET_SCR_HOME:
        state_screen(POCKET_MARK_IDLE, T(HOME_TITLE), T(HOME_SUB),
                     T(HINT_SETTINGS), C_STRONG, C_MUTED);
        break;
    case POCKET_SCR_RECORDING:
        state_screen(POCKET_MARK_HEARING, T(REC_TITLE), NULL, T(REC_HINT),
                     C_STRONG, C_MUTED);
        u.rec_start_ms = m->recording_started_ms;
        show(u.clock, true);
        show(u.bars, true);
        rec_timer_cb(NULL);
        break;
    case POCKET_SCR_SENDING:
        state_screen(POCKET_MARK_THINKING, T(SENDING_TITLE), NULL, NULL, C_STRONG, C_MUTED);
        break;
    case POCKET_SCR_THINKING:
        render_work(m);
        show(u.kicker, true);
        show(u.transcript, true);
        break;
    case POCKET_SCR_REPLY:
        render_reply(m);
        break;
    case POCKET_SCR_NOTICE:
        render_notice(m->notice);
        break;
    case POCKET_SCR_SETTINGS:
        render_settings(m);
        break;
    case POCKET_SCR_PROTO_MISMATCH:
        state_screen(POCKET_MARK_UNAVAILABLE, T(PROTO_TITLE), T(PROTO_SUB), NULL,
                     C_ATTENTION, C_MUTED);
        break;
    case POCKET_SCR_REPAIR_CONFIRM:
        show(u.col, true);
        lv_obj_set_width(u.col, TEXT_W_RAIL);
        lv_obj_align(u.col, LV_ALIGN_TOP_LEFT, REPLY_X, 72);
        set_text(u.title, T(REPAIR_TITLE));
        lv_obj_set_style_text_color(u.title, lv_color_hex(C_STRONG), 0);
        set_text(u.sub, T(REPAIR_SUB));
        lv_obj_set_style_text_color(u.sub, lv_color_hex(C_MUTED), 0);
        show(u.sub, true);
        set_text(u.hint, T(REPAIR_HINT));
        show(u.hint, true);
        rail(KEY_SLOT_OK, T(KEY_OK), C_ATTENTION);
        rail(KEY_SLOT_UP, T(KEY_UP), C_FAINT);
        break;
    default:
        break;
    }
    // Server banner over the bottom of the screens that would otherwise
    // look healthy.
    if (m->server_unreachable && m->link.ready &&
        (m->screen == POCKET_SCR_HOME || m->screen == POCKET_SCR_REPLY ||
         m->screen == POCKET_SCR_THINKING || m->screen == POCKET_SCR_SENDING)) {
        show(u.hint, false);
        show(u.banner, true);
    }
}

int pocket_ui_check_strings(void)
{
#define BOTH(id) POCKET_STR_##id, POCKET_STR_##id##_EN,
    static const char *const strs[] = {
        BOTH(APP_NAME) BOTH(BOOT) BOTH(PAIR_TITLE) BOTH(PAIR_SUB) BOTH(PAIR_CODE_TITLE)
        BOTH(PAIR_CODE_SUB) BOTH(PAIR_CODE_HINT) BOTH(PAIR_WAIT) BOTH(OFFLINE_TITLE)
        BOTH(OFFLINE_SUB) BOTH(OFFLINE_REFUSED) BOTH(HOME_TITLE) BOTH(HOME_SUB)
        BOTH(HINT_SETTINGS) BOTH(REC_TITLE) BOTH(REC_HINT) BOTH(SENDING_TITLE)
        BOTH(WORK_RECEIVED) BOTH(WORK_THINKING) BOTH(WORK_TOOL) BOTH(YOU_SAID) BOTH(REPLY_NAME)
        BOTH(REPLY_STREAMING) BOTH(REPLY_MORE) BOTH(EMPTY_TITLE) BOTH(EMPTY_SUB) BOTH(ASR_TITLE)
        BOTH(ASR_SUB) BOTH(SEND_TITLE) BOTH(SEND_SUB) BOTH(SERVER_TITLE) BOTH(SERVER_SUB)
        BOTH(NOTICE_HINT) BOTH(BANNER_SERVER) BOTH(SETTINGS_TITLE) BOTH(SET_PREROLL)
        BOTH(SET_BRIGHTNESS) BOTH(SET_SCREEN_OFF) BOTH(SECONDS) BOTH(NEVER) BOTH(SET_DEEP_SLEEP)
        BOTH(MINUTES) BOTH(HOURS) BOTH(SET_ALERT_SCREEN) BOTH(SET_ALERT_TONE) BOTH(SET_REPAIR)
        BOTH(SET_BACK) BOTH(ON) BOTH(OFF) BOTH(SETTINGS_HINT) BOTH(REPAIR_TITLE) BOTH(REPAIR_SUB)
        BOTH(REPAIR_HINT) BOTH(PROTO_TITLE) BOTH(PROTO_SUB)
        BOTH(KEY_UP) BOTH(KEY_OK) BOTH(KEY_DOWN)
        "0123456789:% ",
    };
#undef BOTH
    int missing = 0, total = 0;
    for (size_t i = 0; i < sizeof strs / sizeof strs[0]; i++) {
        const uint8_t *s = (const uint8_t *)strs[i];
        size_t len = strlen(strs[i]), pos = 0;
        while (pos < len) {
            uint32_t cp;
            if (!pocket_utf8_next(s, len, &pos, &cp)) {
                missing++;
                continue;
            }
            total++;
            if (!pocket_ui_has_glyph(cp)) {
                ESP_LOGE(TAG, "UI copy: no glyph for U+%04lX", (unsigned long)cp);
                missing++;
            }
        }
    }
    // Negative case: a code point outside the inventory must be reported.
    if (pocket_ui_has_glyph(0x9F98)) ESP_LOGE(TAG, "coverage check broken: U+9F98 present");
    ESP_LOGI(TAG, "UI copy glyph check: %d code points, %d missing", total, missing);
    return missing;
}
