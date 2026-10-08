// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

#include "pocket_model.h"

#include <string.h>

static const uint8_t s_brightness[POCKET_BRIGHTNESS_COUNT] = POCKET_BRIGHTNESS_LEVELS;
static const uint8_t s_screen_off_s[POCKET_SCREEN_OFF_COUNT] = POCKET_SCREEN_OFF_LEVELS_S;
static const uint16_t s_deep_sleep_s[POCKET_DEEP_SLEEP_COUNT] = POCKET_DEEP_SLEEP_LEVELS_S;

// Wrap-safe "a is at or after b" for millisecond timestamps.
static bool time_reached(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}

void pocket_model_init(pocket_model_t *m, uint16_t first_press_id, const pocket_settings_t *s)
{
    memset(m, 0, sizeof(*m));
    m->next_press_id = first_press_id;
    m->preroll_on = s->preroll_on;
    m->brightness_index = s->brightness_index < POCKET_BRIGHTNESS_COUNT
                              ? s->brightness_index
                              : POCKET_BRIGHTNESS_DEFAULT_INDEX;
    m->screen_off_index = s->screen_off_index < POCKET_SCREEN_OFF_COUNT
                              ? s->screen_off_index
                              : POCKET_SCREEN_OFF_DEFAULT_INDEX;
    m->deep_sleep_index = s->deep_sleep_index < POCKET_DEEP_SLEEP_COUNT
                              ? s->deep_sleep_index
                              : POCKET_DEEP_SLEEP_DEFAULT_INDEX;
    m->alert_screen = s->alert_screen;
    m->alert_tone = s->alert_tone;
    m->lang = s->lang < POCKET_LANG_COUNT ? s->lang : POCKET_LANG_DEFAULT;
    m->screen_on = true;  // lit at boot; the delay counts from time 0
    m->battery_pct = POCKET_UNKNOWN_U8;
    m->charging = POCKET_UNKNOWN_U8;
    m->screen = POCKET_SCR_BOOT;
}

bool pocket_model_capture_wanted(const pocket_model_t *m)
{
    if (m->press == PRESS_ARMED || m->press == PRESS_STREAMING) return true;
    // Pre-roll stops while the screen is dark so the codec sleeps and the
    // chip can enter light sleep; a press from dark has no pre-roll.
    return m->preroll_on && m->link.ready && !m->proto_mismatch && m->screen_on;
}

uint8_t pocket_model_backlight_pct(const pocket_model_t *m)
{
    return s_brightness[m->brightness_index];
}

uint32_t pocket_model_screen_off_s(const pocket_model_t *m)
{
    return s_screen_off_s[m->screen_off_index];
}

uint32_t pocket_model_deep_sleep_s(const pocket_model_t *m)
{
    return s_deep_sleep_s[m->deep_sleep_index];
}

void pocket_model_settings(const pocket_model_t *m, pocket_settings_t *out)
{
    *out = (pocket_settings_t){ .preroll_on = m->preroll_on,
                                .brightness_index = m->brightness_index,
                                .screen_off_index = m->screen_off_index,
                                .deep_sleep_index = m->deep_sleep_index,
                                .alert_screen = m->alert_screen,
                                .alert_tone = m->alert_tone,
                                .lang = m->lang };
}

void pocket_model_boot_hold(pocket_model_t *m, bool held)
{
    m->boot_hold = held;
}

static pocket_screen_t base_screen(const pocket_model_t *m)
{
    if (!m->link.ready) return m->link.bonded ? POCKET_SCR_OFFLINE : POCKET_SCR_PAIRING;
    if (m->proto_mismatch) return POCKET_SCR_PROTO_MISMATCH;
    return m->has_reply ? POCKET_SCR_REPLY : POCKET_SCR_HOME;
}

static void set_screen(pocket_model_t *m, pocket_screen_t s, pocket_fx_t *fx)
{
    if (m->screen != s) {
        m->screen = s;
        fx->flags |= POCKET_FX_REDRAW;
    }
}

static bool in_menu(const pocket_model_t *m)
{
    return m->screen == POCKET_SCR_SETTINGS || m->screen == POCKET_SCR_REPAIR_CONFIRM;
}

// --- Standby ------------------------------------------------------------------
// DuoDuo is answering: from release (sending, RESULT 0, received/thinking/
// tool) while the brain is active, and while REPLY text is still streaming
// (final 0), until the wait ends (REPLY_DONE, a failed RESULT, the next
// press, link loss). WORK idle without a streaming reply counts as quiet.
static bool answering(const pocket_model_t *m)
{
    return m->waiting && (m->work_active || (m->has_reply && !m->reply_final));
}

// The screen stays on, with no countdown, while the user is talking, DuoDuo
// is answering, the pairing code is shown, the menu is open, a USB host is
// connected, or the delay is "never". The countdown starts when none of these
// holds any more.
static bool screen_held(const pocket_model_t *m)
{
    return pocket_model_screen_off_s(m) == 0 || m->usb_host || m->press == PRESS_ARMED ||
           m->press == PRESS_STREAMING || answering(m) || m->screen == POCKET_SCR_PAIR_CONFIRM ||
           in_menu(m);
}

// Lights the screen (if dark) and restarts the screen-off delay.
static void screen_wake(pocket_model_t *m, pocket_fx_t *fx)
{
    m->screen_touch_ms = m->now_ms;
    if (!m->screen_on) {
        m->screen_on = true;
        fx->flags |= POCKET_FX_SCREEN;
        fx->screen_on = true;
    }
}

// Shows s now, or after the settings menu closes if the user is in it.
// Recording and pairing screens are never replaced by a phone message.
static void present(pocket_model_t *m, pocket_screen_t s, pocket_fx_t *fx)
{
    if (in_menu(m)) {
        m->settings_return = s;
        return;
    }
    if (m->screen == POCKET_SCR_RECORDING || m->screen == POCKET_SCR_PAIR_CONFIRM) return;
    set_screen(m, s, fx);
}

typedef struct {
    bool capture;
    bool held;
    bool waiting;
} step_t;

static void begin(const pocket_model_t *m, step_t *st)
{
    st->capture = pocket_model_capture_wanted(m);
    st->held = screen_held(m);
    st->waiting = m->waiting;
}

static bool pressing(const pocket_model_t *m);

// Ends a model step: reports a capture or link-idle change, and counts as
// activity a step that held the screen (before or after), ended the reply
// wait, or changed what is shown.
static void finish(pocket_model_t *m, const step_t *st, pocket_fx_t *fx)
{
    bool now = pocket_model_capture_wanted(m);
    if (now != st->capture) {
        fx->flags |= POCKET_FX_CAPTURE;
        fx->capture_wanted = now;
    }
    // Low-power link parameters while nothing needs a quick downlink: the
    // screen is dark, no press, no reply wait. Anything else restores them.
    bool idle = m->link.ready && !m->screen_on && !pressing(m) && !m->waiting;
    if (idle != m->link_idle) {
        m->link_idle = idle;
        fx->flags |= POCKET_FX_LINK_IDLE;
        fx->link_idle = idle;
    }
    if (m->screen_on && (st->held || screen_held(m) || (st->waiting && !m->waiting) ||
                         (fx->flags & POCKET_FX_REDRAW))) {
        m->screen_touch_ms = m->now_ms;
    }
}

static void stop_wait(pocket_model_t *m)
{
    m->waiting = false;
}

static void confirm_press(pocket_model_t *m, pocket_fx_t *fx)
{
    // A new press ends the wait for the previous one.
    stop_wait(m);
    m->press = PRESS_STREAMING;
    m->press_id = m->next_press_id++;
    m->has_press = true;
    m->recording_started_ms = m->press_down_ms;
    fx->flags |= POCKET_FX_AUDIO_STREAM;
    fx->press_id = m->press_id;
    m->notice = POCKET_NOTICE_NONE;
    set_screen(m, POCKET_SCR_RECORDING, fx);
}

static void press_ended(pocket_model_t *m, pocket_fx_t *fx);

static void finish_press(pocket_model_t *m, uint32_t now_ms, pocket_fx_t *fx)
{
    m->press = PRESS_IDLE;
    press_ended(m, fx);
    fx->flags |= POCKET_FX_AUDIO_FINISH;
    fx->press_id = m->press_id;
    m->waiting = true;
    m->wait_press_id = m->press_id;
    m->work_phase = POCKET_WORK_RECEIVED;
    m->work_active = true;
    m->next_keepalive_ms = now_ms + POCKET_KEEPALIVE_INTERVAL_MS;
    set_screen(m, POCKET_SCR_SENDING, fx);
}

void pocket_model_link(pocket_model_t *m, const pocket_link_state_t *link, uint32_t now_ms,
                       pocket_fx_t *fx)
{
    step_t st;
    begin(m, &st);
    m->now_ms = now_ms;
    const bool was_ready = m->link.ready;
    const bool was_bonded = m->link.bonded;
    m->link = *link;

    if (was_ready && !link->ready) {
        if (m->press == PRESS_ARMED) {
            fx->flags |= POCKET_FX_AUDIO_DISCARD;
        } else if (m->press == PRESS_STREAMING) {
            // The press cannot reach the phone; say so instead of pretending.
            fx->flags |= POCKET_FX_AUDIO_ABORT;
            m->refused_flash = true;
        }
        m->press = m->ok_down ? PRESS_REFUSED : PRESS_IDLE;
        press_ended(m, fx);
        stop_wait(m);
        m->server_unreachable = false;
        m->proto_mismatch = false;
        screen_wake(m, fx);  // the user must see that the phone is gone
    }
    if (!was_ready && link->ready) {
        m->refused_flash = false;
        fx->flags |= POCKET_FX_SEND_INFO;
    }
    if (m->screen == POCKET_SCR_PAIR_CONFIRM && !link->connected) {
        set_screen(m, base_screen(m), fx);
    }
    if (in_menu(m)) {
        if (m->settings_return != POCKET_SCR_SETTINGS) m->settings_return = base_screen(m);
    } else if (m->screen != POCKET_SCR_PAIR_CONFIRM) {
        // Any screen that depends on the phone falls back when the link drops,
        // and the offline/pairing screens give way when it comes up.
        if (!link->ready || m->screen == POCKET_SCR_BOOT || m->screen == POCKET_SCR_OFFLINE ||
            m->screen == POCKET_SCR_PAIRING || m->screen == POCKET_SCR_PROTO_MISMATCH) {
            set_screen(m, base_screen(m), fx);
        }
    }
    if (was_ready != link->ready || was_bonded != link->bonded) fx->flags |= POCKET_FX_REDRAW;
    finish(m, &st, fx);
}

void pocket_model_pair_request(pocket_model_t *m, uint32_t passkey, uint32_t now_ms,
                               pocket_fx_t *fx)
{
    step_t st;
    begin(m, &st);
    m->now_ms = now_ms;
    m->passkey = passkey;
    if (in_menu(m)) m->settings_return = base_screen(m);
    m->screen = POCKET_SCR_PAIR_CONFIRM;
    fx->flags |= POCKET_FX_REDRAW;
    screen_wake(m, fx);
    finish(m, &st, fx);
}

void pocket_model_pair_finished(pocket_model_t *m, pocket_fx_t *fx)
{
    m->passkey = 0;
    if (m->screen == POCKET_SCR_PAIR_CONFIRM) set_screen(m, base_screen(m), fx);
}

static void scroll_by(pocket_model_t *m, int dir, pocket_fx_t *fx)
{
    int32_t step = m->view_h - m->line_h;
    if (step < m->line_h) step = m->line_h;
    if (step <= 0) return;
    int32_t next = m->scroll_px + dir * step;
    int32_t max = pocket_model_scroll_max(m);
    if (next < 0) next = 0;
    if (next > max) next = max;
    if (next != m->scroll_px) {
        m->scroll_px = next;
        fx->flags |= POCKET_FX_REDRAW;
        return;
    }
    // At an end of a stored reply: step to the older (UP) or newer (DOWN) one.
    if (m->hist_pos == 0) return;
    if (dir < 0 && m->hist_pos < m->hist_count) fx->flags |= POCKET_FX_HIST_PREV;
    if (dir > 0 && m->hist_pos > 1) fx->flags |= POCKET_FX_HIST_NEXT;
}

static bool pressing(const pocket_model_t *m)
{
    return m->press == PRESS_ARMED || m->press == PRESS_STREAMING;
}

// The shown reply became final: store it now, or when the press ends.
static void save_reply(pocket_model_t *m, pocket_fx_t *fx)
{
    if (pressing(m)) m->hist_save_pending = true;
    else fx->flags |= POCKET_FX_HIST_SAVE;
}

static void press_ended(pocket_model_t *m, pocket_fx_t *fx)
{
    if (m->hist_save_pending) {
        m->hist_save_pending = false;
        fx->flags |= POCKET_FX_HIST_SAVE;
    }
}

static void close_settings(pocket_model_t *m, pocket_fx_t *fx)
{
    pocket_screen_t s = m->settings_return;
    // Only screens that are still meaningful are restored.
    if (!m->link.ready || s == POCKET_SCR_SETTINGS || s == POCKET_SCR_REPAIR_CONFIRM ||
        s == POCKET_SCR_RECORDING || s == POCKET_SCR_PAIR_CONFIRM || s == POCKET_SCR_BOOT ||
        ((s == POCKET_SCR_SENDING || s == POCKET_SCR_THINKING) && !m->waiting) ||
        (s == POCKET_SCR_NOTICE && m->notice == POCKET_NOTICE_NONE) ||
        (s == POCKET_SCR_PROTO_MISMATCH && !m->proto_mismatch)) {
        s = base_screen(m);
    }
    set_screen(m, s, fx);
}

static void activate_setting(pocket_model_t *m, pocket_fx_t *fx)
{
    switch ((pocket_setting_t)m->settings_sel) {
    case POCKET_SET_PREROLL:
        m->preroll_on = !m->preroll_on;
        fx->flags |= POCKET_FX_SAVE_SETTINGS | POCKET_FX_REDRAW;
        if (m->link.ready) fx->flags |= POCKET_FX_SEND_INFO;
        break;
    case POCKET_SET_BRIGHTNESS:
        m->brightness_index = (uint8_t)((m->brightness_index + 1) % POCKET_BRIGHTNESS_COUNT);
        fx->flags |= POCKET_FX_SAVE_SETTINGS | POCKET_FX_BACKLIGHT | POCKET_FX_REDRAW;
        fx->backlight_pct = pocket_model_backlight_pct(m);
        break;
    case POCKET_SET_SCREEN_OFF:
        m->screen_off_index = (uint8_t)((m->screen_off_index + 1) % POCKET_SCREEN_OFF_COUNT);
        fx->flags |= POCKET_FX_SAVE_SETTINGS | POCKET_FX_REDRAW;
        break;
    case POCKET_SET_DEEP_SLEEP:
        m->deep_sleep_index = (uint8_t)((m->deep_sleep_index + 1) % POCKET_DEEP_SLEEP_COUNT);
        fx->flags |= POCKET_FX_SAVE_SETTINGS | POCKET_FX_REDRAW;
        break;
    case POCKET_SET_ALERT_SCREEN:
        m->alert_screen = !m->alert_screen;
        fx->flags |= POCKET_FX_SAVE_SETTINGS | POCKET_FX_REDRAW;
        break;
    case POCKET_SET_ALERT_TONE:
        m->alert_tone = !m->alert_tone;
        fx->flags |= POCKET_FX_SAVE_SETTINGS | POCKET_FX_REDRAW;
        // Turning it on plays it once, so the user hears what it sounds like.
        if (m->alert_tone) fx->flags |= POCKET_FX_TONE;
        break;
    case POCKET_SET_REPAIR:
        set_screen(m, POCKET_SCR_REPAIR_CONFIRM, fx);
        break;
    case POCKET_SET_BACK:
    default:
        close_settings(m, fx);
        break;
    }
}

static void open_settings(pocket_model_t *m, pocket_fx_t *fx)
{
    m->settings_return = m->screen;
    m->settings_sel = 0;
    set_screen(m, POCKET_SCR_SETTINGS, fx);
}

static void on_ok(pocket_model_t *m, pocket_btn_ev_t ev, uint32_t now_ms, pocket_fx_t *fx)
{
    if (ev == POCKET_BTN_EV_DOWN) {
        m->ok_down = true;
        m->ok_down_ms = now_ms;
        if (m->screen == POCKET_SCR_PAIR_CONFIRM || in_menu(m)) return;
        if (!m->link.ready || m->proto_mismatch) {
            m->press = PRESS_REFUSED;
            m->refused_flash = true;
            fx->flags |= POCKET_FX_REDRAW;
            return;
        }
        if (m->press == PRESS_IDLE) {
            m->press = PRESS_ARMED;
            m->press_down_ms = now_ms;
            fx->flags |= POCKET_FX_AUDIO_ARM;
        }
        return;
    }
    if (ev != POCKET_BTN_EV_UP) return;
    m->ok_down = false;

    if (m->screen == POCKET_SCR_PAIR_CONFIRM) {
        if (m->passkey) fx->flags |= POCKET_FX_PAIR_ACCEPT;
        m->passkey = 0;  // answered; the screen waits for the outcome
        fx->flags |= POCKET_FX_REDRAW;
        return;
    }
    if (m->screen == POCKET_SCR_SETTINGS) {
        activate_setting(m, fx);
        return;
    }
    if (m->screen == POCKET_SCR_REPAIR_CONFIRM) {
        // The stored replies belong to the phone that is unpaired: the
        // platform erases them with the bond.
        fx->flags |= POCKET_FX_DELETE_BONDS;
        m->has_reply = false;
        m->hist_pos = m->hist_count = 0;
        m->hist_save_pending = false;
        m->link.bonded = false;
        m->settings_return = base_screen(m);
        close_settings(m, fx);
        return;
    }
    switch (m->press) {
    case PRESS_ARMED:
        if (time_reached(now_ms, m->press_down_ms + POCKET_TAP_THRESHOLD_MS)) {
            confirm_press(m, fx);
            finish_press(m, now_ms, fx);
        } else {
            m->press = PRESS_IDLE;
            fx->flags |= POCKET_FX_AUDIO_DISCARD;
            press_ended(m, fx);
        }
        break;
    case PRESS_STREAMING:
        finish_press(m, now_ms, fx);
        break;
    case PRESS_REFUSED:
    case PRESS_IDLE:
    default:
        m->press = PRESS_IDLE;
        break;
    }
}

void pocket_model_button(pocket_model_t *m, pocket_btn_t btn, pocket_btn_ev_t ev,
                         uint32_t now_ms, pocket_fx_t *fx)
{
    step_t st;
    begin(m, &st);
    m->now_ms = now_ms;
    m->event_ms = now_ms;
    if (m->boot_hold) {
        // The key that woke the device from deep sleep: ignored to its release.
        if (ev == POCKET_BTN_EV_UP) m->boot_hold = false;
        finish(m, &st, fx);
        return;
    }

    // Standby: OK from dark acts at once (press to talk needs no wake press);
    // UP/DOWN from dark only light the screen, and the rest of that press
    // (long press, release) is ignored.
    const uint8_t bit = (uint8_t)(1u << btn);
    if (ev == POCKET_BTN_EV_DOWN) {
        m->swallow &= (uint8_t)~bit;
        if (!m->screen_on && btn != POCKET_BTN_OK) {
            screen_wake(m, fx);
            m->swallow |= bit;
            finish(m, &st, fx);
            return;
        }
    } else if (m->swallow & bit) {
        if (ev == POCKET_BTN_EV_UP) m->swallow &= (uint8_t)~bit;
        finish(m, &st, fx);
        return;
    }
    screen_wake(m, fx);

    if (btn == POCKET_BTN_OK) {
        on_ok(m, ev, now_ms, fx);
        finish(m, &st, fx);
        return;
    }
    if (m->press == PRESS_ARMED || m->press == PRESS_STREAMING) {
        // UP/DOWN do nothing while talking.
        finish(m, &st, fx);
        return;
    }
    const int dir = btn == POCKET_BTN_UP ? -1 : 1;

    if (ev == POCKET_BTN_EV_LONG) {
        if (btn == POCKET_BTN_UP) {
            if (in_menu(m)) close_settings(m, fx);
            else if (m->screen != POCKET_SCR_PAIR_CONFIRM && m->screen != POCKET_SCR_BOOT)
                open_settings(m, fx);
        }
        finish(m, &st, fx);
        return;
    }
    if (ev != POCKET_BTN_EV_DOWN) {
        finish(m, &st, fx);
        return;
    }
    switch (m->screen) {
    case POCKET_SCR_SETTINGS:
        m->settings_sel = (uint8_t)((m->settings_sel + POCKET_SET_COUNT + dir) % POCKET_SET_COUNT);
        fx->flags |= POCKET_FX_REDRAW;
        break;
    case POCKET_SCR_REPAIR_CONFIRM:
        set_screen(m, POCKET_SCR_SETTINGS, fx);
        break;
    case POCKET_SCR_PAIR_CONFIRM:
        if (btn == POCKET_BTN_DOWN && m->passkey) {
            fx->flags |= POCKET_FX_PAIR_REJECT | POCKET_FX_REDRAW;
            m->passkey = 0;
        }
        break;
    case POCKET_SCR_REPLY:
        scroll_by(m, dir, fx);
        break;
    case POCKET_SCR_NOTICE:
        m->notice = POCKET_NOTICE_NONE;
        set_screen(m, base_screen(m), fx);
        break;
    default:
        break;
    }
    finish(m, &st, fx);
}

// Deep sleep counts down unless it is off, already requested, or something
// keeps the device awake: a press (or the key that woke it), a reply wait,
// the pairing code, or a USB host (as for the screen; a debug delay ignores
// the host so the path can be tested on the console).
static bool deep_sleep_armed(const pocket_model_t *m, uint32_t *delay_ms)
{
    uint32_t s = m->deep_sleep_test_s ? m->deep_sleep_test_s : pocket_model_deep_sleep_s(m);
    if (s == 0 || m->deep_sleep_sent || pressing(m) || m->ok_down || m->boot_hold ||
        m->waiting || m->screen == POCKET_SCR_PAIR_CONFIRM ||
        (m->usb_host && !m->deep_sleep_test_s)) {
        return false;
    }
    *delay_ms = s * 1000u;
    return true;
}

void pocket_model_tick(pocket_model_t *m, uint32_t now_ms, pocket_fx_t *fx)
{
    step_t st;
    begin(m, &st);
    m->now_ms = now_ms;
    if (m->press == PRESS_ARMED &&
        time_reached(now_ms, m->press_down_ms + POCKET_TAP_THRESHOLD_MS)) {
        confirm_press(m, fx);
    }
    if (m->waiting && time_reached(now_ms, m->next_keepalive_ms)) {
        fx->flags |= POCKET_FX_SEND_KEEPALIVE;
        fx->press_id = m->wait_press_id;
        m->next_keepalive_ms = now_ms + POCKET_KEEPALIVE_INTERVAL_MS;
    }
    if (m->screen_on && !st.held && !screen_held(m) &&
        time_reached(now_ms, m->screen_touch_ms + pocket_model_screen_off_s(m) * 1000u)) {
        m->screen_on = false;
        fx->flags |= POCKET_FX_SCREEN;
        fx->screen_on = false;
    }
    uint32_t sleep_ms;
    if (deep_sleep_armed(m, &sleep_ms) && time_reached(now_ms, m->event_ms + sleep_ms)) {
        m->deep_sleep_sent = true;
        fx->flags |= POCKET_FX_DEEP_SLEEP;
    }
    finish(m, &st, fx);
}

static uint32_t until(uint32_t now_ms, uint32_t at_ms)
{
    return time_reached(now_ms, at_ms) ? 0 : at_ms - now_ms;
}

uint32_t pocket_model_tick_wait_ms(const pocket_model_t *m, uint32_t now_ms)
{
    if (m->press == PRESS_ARMED || m->press == PRESS_STREAMING) return POCKET_TICK_MS;
    uint32_t wait = UINT32_MAX;
    if (m->waiting) wait = until(now_ms, m->next_keepalive_ms);
    if (m->screen_on && !screen_held(m)) {
        uint32_t off = until(now_ms, m->screen_touch_ms + pocket_model_screen_off_s(m) * 1000u);
        if (off < wait) wait = off;
    }
    uint32_t sleep_ms;
    if (deep_sleep_armed(m, &sleep_ms)) {
        uint32_t ds = until(now_ms, m->event_ms + sleep_ms);
        if (ds < wait) wait = ds;
    }
    return wait;
}

static pocket_notice_t notice_for(uint8_t code)
{
    switch (code) {
    case POCKET_RESULT_EMPTY: return POCKET_NOTICE_EMPTY;
    case POCKET_RESULT_ASR_FAILED: return POCKET_NOTICE_ASR_FAILED;
    case POCKET_RESULT_NO_SERVER: return POCKET_NOTICE_NO_SERVER;
    case POCKET_RESULT_SEND_FAILED:
    default: return POCKET_NOTICE_SEND_FAILED;
    }
}

void pocket_model_downlink(pocket_model_t *m, const pocket_downlink_t *msg, bool stored,
                           uint32_t now_ms, pocket_fx_t *fx)
{
    step_t st;
    begin(m, &st);
    m->now_ms = now_ms;
    if (msg->type == POCKET_MSG_REPLY || msg->type == POCKET_MSG_WORK ||
        msg->type == POCKET_MSG_RESULT) {
        m->event_ms = now_ms;
    }
    switch (msg->type) {
    case POCKET_MSG_RESULT:
        // Results for anything but the last confirmed press are stale.
        if (!m->has_press || msg->press_id != m->press_id) break;
        if (msg->code == POCKET_RESULT_TRANSCRIBED) {
            // The working state needs a live wait; a transcript for a wait
            // that already ended (REPLY_DONE 0, a later press) is not shown.
            if (!m->waiting || m->wait_press_id != msg->press_id) break;
            fx->flags |= POCKET_FX_TRANSCRIPT_TEXT;
            m->transcript_version++;
            m->notice = POCKET_NOTICE_NONE;
            present(m, POCKET_SCR_THINKING, fx);
            fx->flags |= POCKET_FX_REDRAW;
            screen_wake(m, fx);
        } else {
            stop_wait(m);
            m->notice = notice_for(msg->code);
            present(m, POCKET_SCR_NOTICE, fx);
            fx->flags |= POCKET_FX_REDRAW;
            screen_wake(m, fx);
        }
        break;
    case POCKET_MSG_REPLY: {
        const bool fresh = !m->has_reply || msg->reply_id != m->reply_id;
        if (fresh) {
            if (m->hist_save_pending) {
                // Two answers within one press: the earlier one's text is
                // replaced before it could be stored.
                m->hist_save_pending = false;
            }
            m->reply_id = msg->reply_id;
            m->scroll_px = 0;
            m->scroll_to_end = false;
            m->reply_done = false;
            m->reply_final = false;
            m->hist_pos = 0;  // the platform sets it once the reply is stored
        }
        if (msg->final && !m->reply_final && !stored) save_reply(m, fx);
        fx->flags |= POCKET_FX_REPLY_TEXT;
        m->reply_final = msg->final;
        m->has_reply = true;
        m->reply_version++;
        m->notice = POCKET_NOTICE_NONE;
        present(m, POCKET_SCR_REPLY, fx);
        fx->flags |= POCKET_FX_REDRAW;
        // Alerts, once per reply_id: the first text of an answer not stored
        // yet. Later texts of the same answer keep a lit screen lit; an
        // answer sent again is shown quietly. No tone into a press.
        if (fresh && !stored) {
            if (m->alert_screen) screen_wake(m, fx);
            if (m->alert_tone && !pressing(m)) fx->flags |= POCKET_FX_TONE;
        } else if (m->screen_on) {
            screen_wake(m, fx);
        }
        break;
    }
    case POCKET_MSG_REPLY_DONE:
        if (msg->reply_id == 0) {
            // The phone stopped waiting without a reply: leave the
            // sending/thinking screens for the base screen.
            bool was_waiting_screen = m->screen == POCKET_SCR_SENDING ||
                                      m->screen == POCKET_SCR_THINKING;
            stop_wait(m);
            if (was_waiting_screen) set_screen(m, base_screen(m), fx);
            break;
        }
        if (m->has_reply && msg->reply_id == m->reply_id) {
            if (!m->reply_final && m->hist_pos == 0) save_reply(m, fx);
            m->reply_done = true;
            m->reply_final = true;
            fx->flags |= POCKET_FX_REDRAW;
        }
        stop_wait(m);
        break;
    case POCKET_MSG_WORK: {
        if (!m->waiting) break;
        uint8_t phase = msg->code;
        bool active = phase != POCKET_WORK_IDLE;
        if (phase > POCKET_WORK_TOOL) phase = POCKET_WORK_THINKING;
        if (!active) phase = m->work_phase;  // idle keeps the last label, stops the animation
        if (phase != m->work_phase || active != m->work_active) {
            m->work_phase = phase;
            m->work_active = active;
            if (m->screen == POCKET_SCR_THINKING) fx->flags |= POCKET_FX_REDRAW;
        }
        break;
    }
    case POCKET_MSG_APP_STATE: {
        // A relaunched phone app can re-subscribe on a connection that stays
        // up; the CCCD does not change, so the link raises no event. The phone
        // sends APP_STATE after its link is ready, so INFO answers it.
        if (m->link.ready) fx->flags |= POCKET_FX_SEND_INFO;
        // The phone's UI language: applied at once and kept. A missing byte
        // (1.0/1.1 phone) or an unknown value keeps the current one.
        if (msg->has_lang && msg->lang < POCKET_LANG_COUNT && msg->lang != m->lang) {
            m->lang = msg->lang;
            fx->flags |= POCKET_FX_SAVE_SETTINGS | POCKET_FX_REDRAW;
        }
        bool mismatch = msg->code == POCKET_APP_PROTO_MISMATCH;
        bool unreachable = msg->code == POCKET_APP_SERVER_UNREACHABLE;
        if (unreachable != m->server_unreachable) {
            m->server_unreachable = unreachable;
            fx->flags |= POCKET_FX_REDRAW;
            if (unreachable) screen_wake(m, fx);
        }
        if (mismatch != m->proto_mismatch) {
            m->proto_mismatch = mismatch;
            if (mismatch) {
                stop_wait(m);
                if (m->press == PRESS_ARMED) fx->flags |= POCKET_FX_AUDIO_DISCARD;
                else if (m->press == PRESS_STREAMING) fx->flags |= POCKET_FX_AUDIO_ABORT;
                if (m->press != PRESS_IDLE) m->press = m->ok_down ? PRESS_REFUSED : PRESS_IDLE;
                press_ended(m, fx);
                if (in_menu(m)) m->settings_return = POCKET_SCR_PROTO_MISMATCH;
                else set_screen(m, POCKET_SCR_PROTO_MISMATCH, fx);
                screen_wake(m, fx);
            } else if (m->screen == POCKET_SCR_PROTO_MISMATCH) {
                set_screen(m, base_screen(m), fx);
            }
            fx->flags |= POCKET_FX_REDRAW;
        }
        break;
    }
    default:
        break;
    }
    finish(m, &st, fx);
}

void pocket_model_usb_host(pocket_model_t *m, bool connected, uint32_t now_ms, pocket_fx_t *fx)
{
    if (connected == m->usb_host) return;
    step_t st;
    begin(m, &st);
    m->now_ms = now_ms;
    m->event_ms = now_ms;
    m->usb_host = connected;
    if (connected) screen_wake(m, fx);
    finish(m, &st, fx);
}

void pocket_model_battery(pocket_model_t *m, uint8_t pct, uint8_t charging, pocket_fx_t *fx)
{
    if (pct == m->battery_pct && charging == m->charging) return;
    m->battery_pct = pct;
    m->charging = charging;
    fx->flags |= POCKET_FX_REDRAW;
    if (m->link.ready) fx->flags |= POCKET_FX_SEND_STATUS;
}

int32_t pocket_model_scroll_max(const pocket_model_t *m)
{
    int32_t max = m->content_h - m->view_h;
    return max > 0 ? max : 0;
}

void pocket_model_show_stored(pocket_model_t *m, uint32_t reply_id, uint16_t pos, uint16_t count,
                              bool at_end, pocket_fx_t *fx)
{
    m->has_reply = true;
    m->reply_id = reply_id;
    m->reply_final = true;
    m->reply_done = true;
    m->reply_version++;
    m->hist_pos = pos;
    m->hist_count = count;
    m->scroll_px = 0;
    m->scroll_to_end = at_end;
    if (m->screen == POCKET_SCR_HOME) set_screen(m, POCKET_SCR_REPLY, fx);
    if (m->screen == POCKET_SCR_REPLY) fx->flags |= POCKET_FX_REDRAW;
}

void pocket_model_hist_position(pocket_model_t *m, uint16_t pos, uint16_t count, pocket_fx_t *fx)
{
    if (pos == m->hist_pos && count == m->hist_count) return;
    m->hist_pos = pos;
    m->hist_count = count;
    if (m->screen == POCKET_SCR_REPLY) fx->flags |= POCKET_FX_REDRAW;
}

void pocket_model_reply_metrics(pocket_model_t *m, int32_t content_h, int32_t view_h,
                                int32_t line_h)
{
    m->content_h = content_h;
    m->view_h = view_h;
    m->line_h = line_h;
    int32_t max = pocket_model_scroll_max(m);
    if (m->scroll_to_end) {
        m->scroll_to_end = false;
        m->scroll_px = max;
    }
    if (m->scroll_px > max) m->scroll_px = max;
    if (m->scroll_px < 0) m->scroll_px = 0;
}
