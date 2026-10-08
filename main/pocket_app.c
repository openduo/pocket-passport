// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Application task: the only owner of the model. Button, BLE and timer
// events arrive through one queue; model effects are carried out here, and
// the screen is redrawn under the LVGL lock.
#include "pocket_app.h"

#include "pocket_audio.h"
#include "pocket_ble.h"
#include "pocket_config.h"
#include "pocket_hist.h"
#include "pocket_model.h"
#include "pocket_power.h"
#include "pocket_proto.h"
#include "pocket_stats.h"
#include "pocket_ui.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include <string.h>

static const char *TAG = "pocket_app";

// Pending events: the measured peak (debug evq_peak). Flow probe burst while
// the app task was blocked for 120 ms, about a full redraw of the 4 KB reply:
// 7 queued (five WORK, the reassembled REPLY, REPLY_DONE; the app had taken
// the RESULT before it blocked). Idle app: 1; a real press: 2. A reply is one
// event whatever its size, and only one large message can wait, as it holds
// the receive buffer. Overflow is logged and drops the event; nothing blocks.
#define EVENT_QUEUE_DEPTH 7
#define NVS_NAMESPACE "pocket"

#ifdef CONFIG_POCKET_DEBUG_STATS
#define DEBUG_STATS 1
#else
#define DEBUG_STATS 0
#endif

#ifdef CONFIG_POCKET_DEBUG_DEEP_SLEEP_S
#define DEBUG_DEEP_SLEEP_S CONFIG_POCKET_DEBUG_DEEP_SLEEP_S
#define DEBUG_DEEP_SLEEP_TIMER_S CONFIG_POCKET_DEBUG_DEEP_SLEEP_TIMER_S
#else
#define DEBUG_DEEP_SLEEP_S 0
#define DEBUG_DEEP_SLEEP_TIMER_S 0
#endif

#ifdef CONFIG_POCKET_DEBUG_IGNORE_USB_HOST
#define DEBUG_IGNORE_USB_HOST 1
#else
#define DEBUG_IGNORE_USB_HOST 0
#endif

#ifdef CONFIG_POCKET_PREROLL_DEFAULT
#define POCKET_PREROLL_DEFAULT_ON true
#else
#define POCKET_PREROLL_DEFAULT_ON false
#endif

typedef enum { EV_BTN, EV_LINK, EV_PASSKEY, EV_PAIR_DONE, EV_MSG, EV_DEBUG_WAIT } ev_kind_t;

typedef struct {
    ev_kind_t kind;
    uint32_t t_ms;
    int64_t t_us;               // buttons: event time for the latency diagnostic
    union {
        struct { pocket_btn_t btn; pocket_btn_ev_t ev; } btn;
        pocket_link_state_t link;
        uint32_t passkey;
        bool ok;
        // A phone message (pocket_rx_msg_pack fills every field: the union's
        // initializer only zeroes its first member).
        pocket_rx_msg_t msg;
    };
    bool injected;              // EV_MSG: from a debug probe, never stored
} event_t;

static QueueHandle_t s_q;
static pocket_model_t s_m;
static bool s_preroll_applied;

// Stored replies (pocket_hist.h) in the "history" partition.
static pocket_hist_t s_hist;
static bool s_hist_ok;
static pocket_hist_entry_t s_hist_cur;  // the shown stored reply, while s_m.hist_pos != 0
// Set once the key ladder is polled: from then on the app task can tell
// whether the key that woke the chip from deep sleep is still down.
static volatile bool s_buttons_ready;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

#if DEBUG_STATS
// Debug: most events ever waiting in the queue, to size EVENT_QUEUE_DEPTH.
static portMUX_TYPE s_q_peak_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_q_peak;

uint32_t pocket_app_debug_queue_peak(void)
{
    return s_q_peak;
}

void pocket_app_debug_queue_peak_reset(void)
{
    portENTER_CRITICAL(&s_q_peak_lock);
    s_q_peak = 0;
    portEXIT_CRITICAL(&s_q_peak_lock);
}

// Debug: event kinds (and message types) in enqueue order with the queue
// length after each, while a trace is on.
#define Q_TRACE_MAX 32
static bool s_q_trace_on;
static uint8_t s_q_trace_n;
static struct { uint8_t kind, type, waiting; } s_q_trace[Q_TRACE_MAX];

void pocket_app_debug_queue_trace(bool on)
{
    portENTER_CRITICAL(&s_q_peak_lock);
    if (on) s_q_trace_n = 0;
    s_q_trace_on = on;
    portEXIT_CRITICAL(&s_q_peak_lock);
    if (on) return;
    for (unsigned i = 0; i < s_q_trace_n; i++) {
        ESP_LOGI(TAG, "queue trace %u: kind %u type 0x%02x waiting %u", i,
                 (unsigned)s_q_trace[i].kind, (unsigned)s_q_trace[i].type,
                 (unsigned)s_q_trace[i].waiting);
    }
}
#endif

static bool post(const event_t *e)
{
    if (xQueueSend(s_q, e, 0) == pdTRUE) {
#if DEBUG_STATS
        uint32_t n = (uint32_t)uxQueueMessagesWaiting(s_q);
        portENTER_CRITICAL(&s_q_peak_lock);
        if (n > s_q_peak) s_q_peak = n;
        if (s_q_trace_on && s_q_trace_n < Q_TRACE_MAX) {
            s_q_trace[s_q_trace_n].kind = (uint8_t)e->kind;
            s_q_trace[s_q_trace_n].type = e->kind == EV_MSG ? e->msg.type : 0;
            s_q_trace[s_q_trace_n].waiting = (uint8_t)n;
            s_q_trace_n++;
        }
        portEXIT_CRITICAL(&s_q_peak_lock);
#endif
        return true;
    }
    ESP_LOGE(TAG, "event queue full, dropped kind %d", e->kind);
    return false;
}

// --- Event sources (other tasks; must not block) -------------------------------

static void on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    int64_t t_us = esp_timer_get_time();
    event_t e = { .kind = EV_BTN, .t_ms = (uint32_t)(t_us / 1000), .t_us = t_us };
    e.btn.btn = btn == BSP_BTN_UP ? POCKET_BTN_UP : btn == BSP_BTN_DOWN ? POCKET_BTN_DOWN
                                                                       : POCKET_BTN_OK;
    switch (ev) {
    case BSP_BTN_PRESS: e.btn.ev = POCKET_BTN_EV_DOWN; break;
    case BSP_BTN_RELEASE: e.btn.ev = POCKET_BTN_EV_UP; break;
    case BSP_BTN_LONG: e.btn.ev = POCKET_BTN_EV_LONG; break;
    default: return;  // click/double click: the model times gestures itself
    }
    post(&e);
}

static void on_link(const pocket_link_state_t *link)
{
    event_t e = { .kind = EV_LINK, .t_ms = now_ms(), .link = *link };
    post(&e);
}

static void on_passkey(uint32_t passkey)
{
    event_t e = { .kind = EV_PASSKEY, .t_ms = now_ms(), .passkey = passkey };
    post(&e);
}

static void on_pair_done(bool ok)
{
    event_t e = { .kind = EV_PAIR_DONE, .t_ms = now_ms(), .ok = ok };
    post(&e);
}

// Small messages are copied into the event; a larger one (a reply or a
// transcript) is read in place from the BLE receive buffer, which the app
// task releases once the text is in the screen's buffer.
static bool on_message(uint8_t type, const uint8_t *payload, size_t len, bool truncated)
{
    event_t e = { .kind = EV_MSG, .t_ms = now_ms() };
#if POCKET_DEBUG_INJECTION
    e.injected = pocket_ble_debug_injecting();
#endif
    if (!pocket_rx_msg_pack(&e.msg, type, payload, len, truncated)) {
        post(&e);
        return false;
    }
    return post(&e);
}

#if CONFIG_POCKET_DEBUG_MEMPROBE
void pocket_app_debug_key(int btn, int ev)
{
    int64_t t_us = esp_timer_get_time();
    event_t e = { .kind = EV_BTN, .t_ms = (uint32_t)(t_us / 1000), .t_us = t_us };
    e.btn.btn = (pocket_btn_t)btn;
    e.btn.ev = (pocket_btn_ev_t)ev;
    post(&e);
}
#endif

#if CONFIG_POCKET_DEBUG_FLOWPROBE
static volatile uint16_t s_debug_wait_id;

void pocket_app_debug_begin_wait(void)
{
    s_debug_wait_id = 0;
    event_t e = { .kind = EV_DEBUG_WAIT, .t_ms = now_ms() };
    post(&e);
}

uint16_t pocket_app_debug_wait_press_id(void)
{
    return s_debug_wait_id;
}

int pocket_app_debug_screen(void)
{
    return (int)s_m.screen;
}

int pocket_app_debug_lang(void)
{
    return (int)s_m.lang;
}

// The state finish_press leaves behind, without a recording.
static void debug_begin_wait(uint32_t t, pocket_fx_t *fx)
{
    s_m.press_id = s_m.next_press_id++;
    s_m.has_press = true;
    s_m.waiting = true;
    s_m.wait_press_id = s_m.press_id;
    s_m.work_phase = POCKET_WORK_RECEIVED;
    s_m.work_active = true;
    s_m.next_keepalive_ms = t + POCKET_KEEPALIVE_INTERVAL_MS;
    s_m.screen = POCKET_SCR_SENDING;
    fx->flags |= POCKET_FX_REDRAW;
    s_debug_wait_id = s_m.press_id ? s_m.press_id : 1;
}
#endif

// --- Settings -------------------------------------------------------------------

// NVS keys (namespace "pocket"); older images ignore the keys they lack.
static void settings_load(pocket_settings_t *st)
{
    *st = (pocket_settings_t){ .preroll_on = POCKET_PREROLL_DEFAULT_ON,
                               .brightness_index = POCKET_BRIGHTNESS_DEFAULT_INDEX,
                               .screen_off_index = POCKET_SCREEN_OFF_DEFAULT_INDEX,
                               .deep_sleep_index = POCKET_DEEP_SLEEP_DEFAULT_INDEX,
                               .alert_screen = POCKET_ALERT_SCREEN_DEFAULT,
                               .alert_tone = POCKET_ALERT_TONE_DEFAULT,
                               .lang = POCKET_LANG_DEFAULT };
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "preroll", &v) == ESP_OK) st->preroll_on = v != 0;
    if (nvs_get_u8(h, "bright", &v) == ESP_OK) st->brightness_index = v;
    if (nvs_get_u8(h, "scr_off", &v) == ESP_OK) st->screen_off_index = v;
    if (nvs_get_u8(h, "deep_slp", &v) == ESP_OK) st->deep_sleep_index = v;
    if (nvs_get_u8(h, "alert_scr", &v) == ESP_OK) st->alert_screen = v != 0;
    if (nvs_get_u8(h, "alert_tone", &v) == ESP_OK) st->alert_tone = v != 0;
    if (nvs_get_u8(h, "lang", &v) == ESP_OK) st->lang = v;
    nvs_close(h);
}

static void settings_save(const pocket_model_t *m)
{
    pocket_settings_t st;
    pocket_model_settings(m, &st);
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e == ESP_OK) e = nvs_set_u8(h, "preroll", st.preroll_on ? 1 : 0);
    if (e == ESP_OK) e = nvs_set_u8(h, "bright", st.brightness_index);
    if (e == ESP_OK) e = nvs_set_u8(h, "scr_off", st.screen_off_index);
    if (e == ESP_OK) e = nvs_set_u8(h, "deep_slp", st.deep_sleep_index);
    if (e == ESP_OK) e = nvs_set_u8(h, "alert_scr", st.alert_screen ? 1 : 0);
    if (e == ESP_OK) e = nvs_set_u8(h, "alert_tone", st.alert_tone ? 1 : 0);
    if (e == ESP_OK) e = nvs_set_u8(h, "lang", st.lang);
    if (e == ESP_OK) e = nvs_commit(h);
    if (e != ESP_OK) ESP_LOGE(TAG, "settings not saved: %s", esp_err_to_name(e));
    nvs_close(h);
}

// --- Reply history ------------------------------------------------------------------

static bool part_read(void *ctx, uint32_t off, void *buf, size_t len)
{
    return esp_partition_read(ctx, off, buf, len) == ESP_OK;
}

static bool part_write(void *ctx, uint32_t off, const void *buf, size_t len)
{
    return esp_partition_write(ctx, off, buf, len) == ESP_OK;
}

static bool part_erase(void *ctx, uint32_t off)
{
    const esp_partition_t *p = ctx;
    return esp_partition_erase_range(p, off, p->erase_size) == ESP_OK;
}

static void hist_init(void)
{
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)POCKET_HIST_PARTITION_SUBTYPE,
        POCKET_HIST_PARTITION_LABEL);
    if (!p) {
        ESP_LOGE(TAG, "no \"%s\" partition: replies are not stored", POCKET_HIST_PARTITION_LABEL);
        return;
    }
    const pocket_hist_flash_t fl = { .ctx = (void *)p, .read = part_read, .write = part_write,
                                     .erase_sector = part_erase, .size = (uint32_t)p->size,
                                     .sector = (uint32_t)p->erase_size };
    int64_t t0 = esp_timer_get_time();
    s_hist_ok = pocket_hist_open(&s_hist, &fl, POCKET_HIST_BUDGET_BYTES);
    ESP_LOGI(TAG, "history ok=%d replies=%lu head=%lu scan_ms=%lu", s_hist_ok,
             (unsigned long)pocket_hist_count(&s_hist), (unsigned long)s_hist.head,
             (unsigned long)((esp_timer_get_time() - t0) / 1000));
}

static uint16_t hist_u16(uint32_t v)
{
    return v > UINT16_MAX ? UINT16_MAX : (uint16_t)v;
}

// The shown reply came from a debug probe: it is never written to the history
// partition, which a later normal image would show.
static bool s_reply_injected;

// Puts a stored reply's text into the screen's buffer and shows it.
static void hist_show(const pocket_hist_entry_t *e, bool at_end, pocket_fx_t *fx)
{
    s_reply_injected = false;  // a stored reply is a real one
    int n = -1;
    if (bsp_lvgl_lock(-1)) {
        size_t cap;
        char *b = pocket_ui_reply_buffer(&cap);
        n = pocket_hist_read_text(&s_hist, e, b, cap);
        pocket_ui_reply_loaded(n < 0 ? 0 : (size_t)n, (e->flags & POCKET_HIST_FLAG_CUT) != 0);
        bsp_lvgl_unlock();
    }
    if (n < 0) ESP_LOGE(TAG, "stored reply %lu unreadable", (unsigned long)e->reply_id);
    ESP_LOGI(TAG, "history show reply %lu (%d B) %lu/%lu", (unsigned long)e->reply_id, n,
             (unsigned long)pocket_hist_position(&s_hist, e),
             (unsigned long)pocket_hist_count(&s_hist));
    s_hist_cur = *e;
    pocket_model_show_stored(&s_m, e->reply_id, hist_u16(pocket_hist_position(&s_hist, e)),
                             hist_u16(pocket_hist_count(&s_hist)), at_end, fx);
}

static void hist_mark(const pocket_hist_entry_t *e, pocket_fx_t *fx)
{
    s_hist_cur = *e;
    pocket_model_hist_position(&s_m, hist_u16(pocket_hist_position(&s_hist, e)),
                               hist_u16(pocket_hist_count(&s_hist)), fx);
}

// Stores the shown reply (from the screen's buffer) unless it is stored.
static void hist_save(pocket_fx_t *fx)
{
    if (!s_hist_ok) return;
#if POCKET_DEBUG_INJECTION
    if (s_reply_injected) {
        ESP_LOGI(TAG, "reply %lu injected by a probe: not stored", (unsigned long)s_m.reply_id);
        return;
    }
#endif
    pocket_hist_entry_t e;
    if (!pocket_hist_find(&s_hist, s_m.reply_id, &e)) {
        size_t len;
        bool cut;
        const char *t = pocket_ui_reply_text(&len, &cut);
        int64_t t0 = esp_timer_get_time();
        if (!pocket_hist_append(&s_hist, s_m.reply_id, t, len, cut ? POCKET_HIST_FLAG_CUT : 0,
                                &e)) {
            ESP_LOGE(TAG, "reply %lu not stored", (unsigned long)s_m.reply_id);
            return;
        }
        ESP_LOGI(TAG, "reply %lu stored: %u B, %lu replies, %lu ms", (unsigned long)e.reply_id,
                 (unsigned)len, (unsigned long)pocket_hist_count(&s_hist),
                 (unsigned long)((esp_timer_get_time() - t0) / 1000));
    }
    hist_mark(&e, fx);
}

static void hist_step(bool older, pocket_fx_t *fx)
{
    pocket_hist_entry_t e;
    bool ok = older ? pocket_hist_prev(&s_hist, &s_hist_cur, &e)
                    : pocket_hist_next(&s_hist, &s_hist_cur, &e);
    if (ok) hist_show(&e, older, fx);
}

// --- Effects ----------------------------------------------------------------------

static void send_info(void)
{
    // Largest INFO payload: 2 + 1 + 255 (fw version) + 3 = 261 bytes.
    uint8_t b[300];
    const esp_app_desc_t *app = esp_app_get_description();
    pocket_info_t info = { .fw_version = app->version, .battery_pct = s_m.battery_pct,
                           .charging = s_m.charging, .preroll_on = s_m.preroll_on };
    size_t n = pocket_msg_info(b, sizeof b, &info);
    bool sent = n && pocket_ble_send(POCKET_MSG_INFO, b, n);
    ESP_LOGI(TAG, "INFO sent=%d", sent);
}

static void apply(pocket_fx_t *fx)
{
    uint8_t b[8];  // STATUS (2 B) and KEEPALIVE (2 B) payloads
    size_t n;
    // History first: it can change what the redraw below shows.
    if (fx->flags & POCKET_FX_HIST_SAVE) hist_save(fx);
    if ((fx->flags & (POCKET_FX_HIST_PREV | POCKET_FX_HIST_NEXT)) && s_hist_ok) {
        hist_step((fx->flags & POCKET_FX_HIST_PREV) != 0, fx);
    }
    // Lighting the screen takes its power lock first, so a press from dark
    // wakes the codec at full clock; the panel follows after the audio.
    const bool screen = (fx->flags & POCKET_FX_SCREEN) != 0;
    if (screen && fx->screen_on) pocket_power_screen_lock();
    if (s_m.preroll_on != s_preroll_applied) {
        pocket_audio_set_preroll(s_m.preroll_on);
        s_preroll_applied = s_m.preroll_on;
    }
    if ((fx->flags & POCKET_FX_CAPTURE) && fx->capture_wanted) pocket_audio_set_capture(true);
    if (fx->flags & POCKET_FX_AUDIO_ARM) pocket_audio_arm();
    if (fx->flags & POCKET_FX_AUDIO_STREAM) pocket_audio_stream(fx->press_id);
    if (fx->flags & POCKET_FX_AUDIO_FINISH) pocket_audio_finish();
    if (fx->flags & POCKET_FX_AUDIO_DISCARD) pocket_audio_discard();
    if (fx->flags & POCKET_FX_AUDIO_ABORT) pocket_audio_abort();
    if ((fx->flags & POCKET_FX_CAPTURE) && !fx->capture_wanted) pocket_audio_set_capture(false);

    if (fx->flags & POCKET_FX_SEND_INFO) send_info();
    if (fx->flags & POCKET_FX_SEND_STATUS) {
        n = pocket_msg_status(b, sizeof b, s_m.battery_pct, s_m.charging);
        pocket_ble_send(POCKET_MSG_STATUS, b, n);
    }
    if (fx->flags & POCKET_FX_SEND_KEEPALIVE) {
        n = pocket_msg_keepalive(b, sizeof b, fx->press_id);
        pocket_ble_send(POCKET_MSG_KEEPALIVE, b, n);
    }
    if (fx->flags & POCKET_FX_PAIR_ACCEPT) pocket_ble_pair_reply(true);
    if (fx->flags & POCKET_FX_PAIR_REJECT) pocket_ble_pair_reply(false);
    if (fx->flags & POCKET_FX_DELETE_BONDS) {
        pocket_ble_delete_bonds();
        if (s_hist_ok && !pocket_hist_clear(&s_hist)) ESP_LOGE(TAG, "history not erased");
    }
    if (fx->flags & POCKET_FX_SAVE_SETTINGS) settings_save(&s_m);
    if (fx->flags & POCKET_FX_BACKLIGHT) bsp_display_backlight(fx->backlight_pct);
    if (fx->flags & POCKET_FX_REDRAW) {
        if (bsp_lvgl_lock(-1)) {
            pocket_ui_render(&s_m, pocket_ble_name());
            bsp_lvgl_unlock();
        }
    }
    if (screen) pocket_power_screen(fx->screen_on, pocket_model_backlight_pct(&s_m));
    if (fx->flags & POCKET_FX_TONE) {
        ESP_LOGI(TAG, "new-reply tone");
        pocket_audio_tone();
    }
    if (fx->flags & POCKET_FX_LINK_IDLE) pocket_ble_set_link_idle(fx->link_idle);
    if (fx->flags & POCKET_FX_DEEP_SLEEP) {
        ESP_LOGI(TAG, "no event for %lu s: deep sleep",
                 (unsigned long)(s_m.deep_sleep_test_s ? s_m.deep_sleep_test_s
                                                       : pocket_model_deep_sleep_s(&s_m)));
        pocket_power_deep_sleep(DEBUG_DEEP_SLEEP_TIMER_S);
    }
}

static void read_battery(void)
{
    int soc = bsp_battery_soc();
    uint8_t pct = soc >= 0 && soc <= 100 ? (uint8_t)soc : POCKET_UNKNOWN_U8;
    // The board has no charge-status signal (bsp_pins.h); report unknown.
    pocket_fx_t fx = { 0 };
    pocket_model_battery(&s_m, pct, POCKET_UNKNOWN_U8, &fx);
    apply(&fx);
}

static void handle(event_t *e)
{
    pocket_fx_t fx = { 0 };
    switch (e->kind) {
    case EV_BTN: {
        const bool dark = !s_m.screen_on;
        pocket_model_button(&s_m, e->btn.btn, e->btn.ev, e->t_ms, &fx);
        if (fx.flags & POCKET_FX_AUDIO_ARM) pocket_audio_mark_press(e->t_us);
        apply(&fx);
        // Diagnostics after the effects: a console write can block while
        // the USB host re-enumerates the port after light sleep.
        if (DEBUG_STATS && e->btn.ev == POCKET_BTN_EV_DOWN) {
            // Wake interrupt (first edge after the button poll went idle) to
            // the decoded key event, and event to effects applied.
            int64_t wake = bsp_button_last_wake_us();
            int64_t isr_us = wake && e->t_us >= wake && e->t_us - wake < 1000000 ? e->t_us - wake
                                                                                   : -1;
            ESP_LOGI(TAG, "key %d down dark=%d wake_isr_to_event_us=%lld event_to_applied_us=%lld",
                     (int)e->btn.btn, dark, (long long)isr_us,
                     (long long)(esp_timer_get_time() - e->t_us));
            pocket_stats_note_key(dark, isr_us);
        }
        return;
    }
    case EV_LINK:
        pocket_stats_note_link(e->link.ready);
        pocket_model_link(&s_m, &e->link, e->t_ms, &fx);
        break;
    case EV_PASSKEY:
        pocket_model_pair_request(&s_m, e->passkey, e->t_ms, &fx);
        break;
    case EV_PAIR_DONE:
        pocket_model_pair_finished(&s_m, &fx);
        break;
    case EV_MSG: {
        pocket_downlink_t d;
        const uint8_t *data = pocket_rx_msg_data(&e->msg);
        if (pocket_msg_parse_downlink(e->msg.type, data, e->msg.len, &d)) {
            if (d.type == POCKET_MSG_APP_STATE) ESP_LOGI(TAG, "APP_STATE %u", (unsigned)d.code);
            pocket_hist_entry_t found;
            const bool stored = d.type == POCKET_MSG_REPLY && s_hist_ok &&
                                pocket_hist_find(&s_hist, d.reply_id, &found);
            pocket_model_downlink(&s_m, &d, stored, e->t_ms, &fx);
            if (d.type == POCKET_MSG_REPLY && d.reply_id == s_m.reply_id) {
                s_reply_injected = e->injected;
            }
            if (stored && d.reply_id == s_m.reply_id) hist_mark(&found, &fx);
            if ((fx.flags & (POCKET_FX_REPLY_TEXT | POCKET_FX_TRANSCRIPT_TEXT)) &&
                bsp_lvgl_lock(-1)) {
                if (fx.flags & POCKET_FX_REPLY_TEXT) {
                    pocket_ui_set_reply(d.text, d.text_len, e->msg.truncated);
                }
                if (fx.flags & POCKET_FX_TRANSCRIPT_TEXT) pocket_ui_set_transcript(d.text, d.text_len);
                bsp_lvgl_unlock();
            }
        } else {
            ESP_LOGW(TAG, "ignored message type 0x%02x len %u", e->msg.type, (unsigned)e->msg.len);
        }
        if (e->msg.kept) pocket_ble_rx_release();
        if (DEBUG_STATS) {
            ESP_LOGI(TAG, "rx type 0x%02x len %u kept=%d -> screen %d waiting=%d", e->msg.type,
                     (unsigned)e->msg.len, e->msg.kept != NULL, (int)s_m.screen, s_m.waiting);
        }
        break;
    }
    case EV_DEBUG_WAIT:
#if CONFIG_POCKET_DEBUG_FLOWPROBE
        debug_begin_wait(e->t_ms, &fx);
#endif
        break;
    }
    apply(&fx);
}

// Sleeps until an event, the model's next timed step or the battery poll:
// no periodic wake while dark and unplugged, so the chip can stay in light
// sleep. While lit or on a USB host the USB state is polled as well.
static void app_task(void *arg)
{
    (void)arg;
    uint32_t next_battery = now_ms();
    event_t e;
    for (;;) {
        uint32_t now = now_ms();
        uint32_t wait = pocket_model_tick_wait_ms(&s_m, now);
        uint32_t battery = (int32_t)(next_battery - now) > 0 ? next_battery - now : 0;
        if (battery < wait) wait = battery;
        if ((s_m.screen_on || s_m.usb_host) && POCKET_USB_POLL_MS < wait) wait = POCKET_USB_POLL_MS;
        if (xQueueReceive(s_q, &e, pdMS_TO_TICKS(wait)) == pdTRUE) handle(&e);
        now = now_ms();
        pocket_fx_t fx = { 0 };
        // USB Serial/JTAG reports a host by its start-of-frame packets; a
        // charger sends none and is not seen.
        pocket_model_usb_host(&s_m, !DEBUG_IGNORE_USB_HOST && usb_serial_jtag_is_connected(), now,
                              &fx);
        // The waking key was released before the button poll saw it.
        if (s_m.boot_hold && s_buttons_ready && !bsp_button_any_down()) {
            pocket_model_boot_hold(&s_m, false);
        }
        // The model acts only on deadlines it has reached.
        pocket_model_tick(&s_m, now, &fx);
        apply(&fx);
        if ((int32_t)(now - next_battery) >= 0) {
            read_battery();
            next_battery = now + POCKET_BATTERY_POLL_MS;
        }
    }
}

esp_err_t pocket_app_start(void)
{
    pocket_settings_t st;
    settings_load(&st);
    // A random first press id per boot keeps a late RESULT for a press of the
    // previous boot from matching a new press.
    pocket_model_init(&s_m, (uint16_t)esp_random(), &st);
    s_m.deep_sleep_test_s = DEBUG_DEEP_SLEEP_S;
    // Woken by a key: that press only boots the device. The app task ends
    // the hold when the key is up (event or ADC reading).
    pocket_model_boot_hold(&s_m, pocket_power_woke_by_key());
    s_preroll_applied = !st.preroll_on;  // force the first apply()
    bsp_display_backlight(pocket_model_backlight_pct(&s_m));

    s_q = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(event_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    pocket_stats_heap_mark("app_queue");

    esp_err_t e = ESP_OK;
    if (bsp_lvgl_lock(-1)) {
        e = pocket_ui_init();
        if (e == ESP_OK) pocket_ui_check_strings();
        bsp_lvgl_unlock();
        if (e != ESP_OK) return e;
    }
    // The newest stored reply is on screen again after a reboot.
    hist_init();
    pocket_fx_t boot_fx = { 0 };
    if (s_hist_ok && s_hist.any) hist_show(&s_hist.newest, false, &boot_fx);
    if (bsp_lvgl_lock(-1)) {
        pocket_ui_render(&s_m, "");
        bsp_lvgl_unlock();
    }
    pocket_stats_heap_mark("ui_render");
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "fuel gauge unavailable; battery unknown");
    // Before the BLE stack starts: its controller reads the sleep settings.
    e = pocket_power_init();
    if (e != ESP_OK) ESP_LOGE(TAG, "standby unavailable: %s", esp_err_to_name(e));
    pocket_stats_heap_mark("battery_power");

    const pocket_ble_callbacks_t cb = { .on_link = on_link, .on_passkey = on_passkey,
                                        .on_pair_done = on_pair_done, .on_message = on_message };
    e = pocket_audio_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "audio start failed: %s", esp_err_to_name(e));
        return e;
    }
    if (xTaskCreate(app_task, "pocket_app", CONFIG_POCKET_APP_TASK_STACK, NULL,
                    CONFIG_POCKET_PRIO_APP, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    pocket_stats_heap_mark("app_task");
    e = pocket_ble_start(&cb);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "BLE start failed: %s", esp_err_to_name(e));
        return e;
    }
    pocket_stats_heap_mark("ble");
    e = bsp_button_init(on_button, NULL);
    s_buttons_ready = e == ESP_OK;
    if (e != ESP_OK) ESP_LOGE(TAG, "buttons unavailable: %s", esp_err_to_name(e));
    else if ((e = bsp_button_enable_sleep_wake()) != ESP_OK) {
        // Without the wake interrupt the button poll keeps running and the
        // chip stays out of light sleep: buttons still work.
        ESP_LOGE(TAG, "button wake unavailable: %s", esp_err_to_name(e));
    }
    ESP_LOGI(TAG, "pocket %s ready, preroll=%d screen_off_s=%u deep_sleep_s=%u alert=%d/%d lang=%u",
             esp_app_get_description()->version, st.preroll_on,
             (unsigned)pocket_model_screen_off_s(&s_m), (unsigned)pocket_model_deep_sleep_s(&s_m),
             st.alert_screen, st.alert_tone, (unsigned)s_m.lang);
    pocket_power_log_wake();
    return ESP_OK;
}
