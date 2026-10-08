// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Host tests: press state machine, reply wait/keepalive, reply replace and
// scroll, settings and pairing screens, standby (screen-off delay, wake keys,
// states that keep the screen on, events that light it).
#include "pocket_check.h"
#include "pocket_model.h"

#include <string.h>

static pocket_model_t m;

static void init_model(uint16_t first, bool preroll, uint8_t bright, uint8_t screen_off)
{
    pocket_settings_t st = { .preroll_on = preroll, .brightness_index = bright,
                             .screen_off_index = screen_off,
                             // "never": the standby tests time the screen only;
                             // test_deep_sleep covers the delay.
                             .deep_sleep_index = POCKET_DEEP_SLEEP_COUNT - 1,
                             .alert_screen = true, .alert_tone = true };
    pocket_model_init(&m, first, &st);
}

static pocket_fx_t link_to(bool bonded, bool connected, bool ready, uint32_t t)
{
    pocket_link_state_t l = { .bonded = bonded, .connected = connected, .ready = ready };
    pocket_fx_t fx = { 0 };
    pocket_model_link(&m, &l, t, &fx);
    return fx;
}

static pocket_fx_t btn(pocket_btn_t b, pocket_btn_ev_t ev, uint32_t t)
{
    pocket_fx_t fx = { 0 };
    pocket_model_button(&m, b, ev, t, &fx);
    return fx;
}

static pocket_fx_t tick(uint32_t t)
{
    pocket_fx_t fx = { 0 };
    pocket_model_tick(&m, t, &fx);
    return fx;
}

static pocket_fx_t result(uint16_t press_id, uint8_t code, const char *text)
{
    pocket_downlink_t d = { .type = POCKET_MSG_RESULT, .press_id = press_id, .code = code,
                            .text = (const uint8_t *)text, .text_len = strlen(text) };
    pocket_fx_t fx = { 0 };
    pocket_model_downlink(&m, &d, false, 0, &fx);
    return fx;
}

static pocket_fx_t reply(uint32_t id, bool final, const char *text)
{
    pocket_downlink_t d = { .type = POCKET_MSG_REPLY, .reply_id = id, .final = final,
                            .text = (const uint8_t *)text, .text_len = strlen(text) };
    pocket_fx_t fx = { 0 };
    pocket_model_downlink(&m, &d, false, 0, &fx);
    return fx;
}

static pocket_fx_t reply_done(uint32_t id)
{
    pocket_downlink_t d = { .type = POCKET_MSG_REPLY_DONE, .reply_id = id };
    pocket_fx_t fx = { 0 };
    pocket_model_downlink(&m, &d, false, 0, &fx);
    return fx;
}

static pocket_fx_t reply_stored(uint32_t id, const char *text)
{
    pocket_downlink_t d = { .type = POCKET_MSG_REPLY, .reply_id = id, .final = true,
                            .text = (const uint8_t *)text, .text_len = strlen(text) };
    pocket_fx_t fx = { 0 };
    pocket_model_downlink(&m, &d, true, 0, &fx);
    return fx;
}

static void fresh(bool preroll)
{
    init_model(100, preroll, 1, POCKET_SCREEN_OFF_DEFAULT_INDEX);
    link_to(true, true, true, 0);
}

static void test_boot_and_link(void)
{
    init_model(7, false, 9, 9);
    CHECK(m.screen == POCKET_SCR_BOOT);
    CHECK(m.brightness_index == POCKET_BRIGHTNESS_DEFAULT_INDEX);
    pocket_fx_t fx = link_to(false, false, false, 0);
    CHECK(m.screen == POCKET_SCR_PAIRING && (fx.flags & POCKET_FX_REDRAW));
    fx = link_to(true, true, true, 10);
    CHECK(m.screen == POCKET_SCR_HOME && (fx.flags & POCKET_FX_SEND_INFO));
    fx = link_to(true, false, false, 20);
    CHECK(m.screen == POCKET_SCR_OFFLINE);
}

static void test_tap_ignored(void)
{
    fresh(false);
    pocket_fx_t fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 1000);
    CHECK(fx.flags & POCKET_FX_AUDIO_ARM);
    CHECK((fx.flags & POCKET_FX_CAPTURE) && fx.capture_wanted);
    fx = tick(1000 + POCKET_TAP_THRESHOLD_MS - 1);
    CHECK(!(fx.flags & POCKET_FX_AUDIO_STREAM) && m.screen == POCKET_SCR_HOME);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 1000 + POCKET_TAP_THRESHOLD_MS - 1);
    CHECK(fx.flags & POCKET_FX_AUDIO_DISCARD);
    CHECK(!(fx.flags & (POCKET_FX_AUDIO_STREAM | POCKET_FX_AUDIO_FINISH)));
    CHECK((fx.flags & POCKET_FX_CAPTURE) && !fx.capture_wanted);
    CHECK(m.screen == POCKET_SCR_HOME && !m.waiting && !m.has_press);
}

static void test_hold_streams(void)
{
    fresh(false);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 1000);
    pocket_fx_t fx = tick(1000 + POCKET_TAP_THRESHOLD_MS);
    CHECK((fx.flags & POCKET_FX_AUDIO_STREAM) && fx.press_id == 100);
    CHECK(m.screen == POCKET_SCR_RECORDING && m.recording_started_ms == 1000);
    // UP/DOWN ignored while talking; no settings.
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 1500);
    CHECK(m.screen == POCKET_SCR_RECORDING);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 5000);
    CHECK((fx.flags & POCKET_FX_AUDIO_FINISH) && fx.press_id == 100);
    CHECK(m.screen == POCKET_SCR_SENDING && m.waiting && m.wait_press_id == 100);

    // Release between ticks but past the threshold: stream and finish at once.
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 6000);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 6000 + POCKET_TAP_THRESHOLD_MS);
    CHECK((fx.flags & POCKET_FX_AUDIO_STREAM) && (fx.flags & POCKET_FX_AUDIO_FINISH));
    CHECK(fx.press_id == 101 && m.wait_press_id == 101);
}

static void test_link_down_refuses(void)
{
    fresh(false);
    link_to(true, false, false, 0);
    pocket_fx_t fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 100);
    CHECK(!(fx.flags & POCKET_FX_AUDIO_ARM) && !(fx.flags & POCKET_FX_CAPTURE));
    CHECK(m.refused_flash && m.screen == POCKET_SCR_OFFLINE);
    fx = tick(2000);
    CHECK(!(fx.flags & POCKET_FX_AUDIO_STREAM));
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 2100);
    fx = link_to(true, true, true, 3000);
    CHECK(!m.refused_flash && m.screen == POCKET_SCR_HOME);

    // Link lost mid-press aborts the stream and stops the wait.
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 4000);
    tick(4400);
    CHECK(m.screen == POCKET_SCR_RECORDING);
    fx = link_to(true, false, false, 5000);
    CHECK(fx.flags & POCKET_FX_AUDIO_ABORT);
    CHECK(m.screen == POCKET_SCR_OFFLINE && m.refused_flash);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 5100);
    CHECK(!(fx.flags & POCKET_FX_AUDIO_FINISH));

    // Pre-roll keeps the mic on only while linked.
    init_model(1, true, 1, POCKET_SCREEN_OFF_DEFAULT_INDEX);
    fx = link_to(true, true, true, 0);
    CHECK((fx.flags & POCKET_FX_CAPTURE) && fx.capture_wanted);
    fx = link_to(true, false, false, 1);
    CHECK((fx.flags & POCKET_FX_CAPTURE) && !fx.capture_wanted);
}

static void test_wait_and_keepalive(void)
{
    fresh(false);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 0);
    tick(400);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 2000);
    pocket_fx_t fx = tick(2000 + POCKET_KEEPALIVE_INTERVAL_MS - 1);
    CHECK(!(fx.flags & POCKET_FX_SEND_KEEPALIVE));
    fx = tick(2000 + POCKET_KEEPALIVE_INTERVAL_MS);
    CHECK((fx.flags & POCKET_FX_SEND_KEEPALIVE) && fx.press_id == 100);

    // Stale RESULT ignored, current one shows the transcript.
    fx = result(99, 0, "old");
    CHECK(m.screen == POCKET_SCR_SENDING && m.transcript_version == 0);
    CHECK(!(fx.flags & POCKET_FX_TRANSCRIPT_TEXT));
    fx = result(100, 0, "hello");
    CHECK(m.screen == POCKET_SCR_THINKING && m.transcript_version == 1 && m.waiting);
    CHECK(fx.flags & POCKET_FX_TRANSCRIPT_TEXT);

    reply(7, false, "partial");
    CHECK(m.screen == POCKET_SCR_REPLY && !m.reply_final && m.waiting);
    reply_done(7);
    CHECK(!m.waiting && m.reply_final && m.reply_done);
    fx = tick(2000 + 3 * POCKET_KEEPALIVE_INTERVAL_MS);
    CHECK(!(fx.flags & POCKET_FX_SEND_KEEPALIVE));

    // No time cap: keepalives continue through a long brain turn.
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 100000);
    tick(100400);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 101000);
    CHECK(m.waiting);
    tick(101000 + 600000 - 1);
    fx = tick(101000 + 600000 + POCKET_KEEPALIVE_INTERVAL_MS);
    CHECK(m.waiting && (fx.flags & POCKET_FX_SEND_KEEPALIVE));
    // The next confirmed press ends it; a tap does not.
    uint16_t old = m.wait_press_id;
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 800000);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 800100);
    CHECK(m.waiting && m.wait_press_id == old);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 801000);
    tick(801400);
    CHECK(!m.waiting && m.screen == POCKET_SCR_RECORDING);
    fx = tick(801400 + POCKET_KEEPALIVE_INTERVAL_MS);
    CHECK(!(fx.flags & POCKET_FX_SEND_KEEPALIVE));
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 802000);
    CHECK(m.waiting && m.wait_press_id != old);

    // Failure codes show a notice and stop the wait; a button dismisses it.
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 300000);
    tick(300400);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 301000);
    result(m.press_id, POCKET_RESULT_EMPTY, "");
    CHECK(m.screen == POCKET_SCR_NOTICE && m.notice == POCKET_NOTICE_EMPTY && !m.waiting);
    btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 302000);
    CHECK(m.screen == POCKET_SCR_REPLY && m.notice == POCKET_NOTICE_NONE);
    result(m.press_id, 77, "");  // unknown future code: shown as a send failure
    CHECK(m.notice == POCKET_NOTICE_SEND_FAILED);
    result(m.press_id, POCKET_RESULT_NO_SERVER, "");
    CHECK(m.notice == POCKET_NOTICE_NO_SERVER);
}

static void test_reply_replace_and_scroll(void)
{
    fresh(false);
    pocket_fx_t fx = reply(1, false, "first");
    CHECK(m.screen == POCKET_SCR_REPLY && m.reply_version == 1);
    CHECK(fx.flags & POCKET_FX_REPLY_TEXT);
    pocket_model_reply_metrics(&m, 1000, 200, 25);
    fx = btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 0);
    CHECK(m.scroll_px == 175 && (fx.flags & POCKET_FX_REDRAW));
    for (int i = 0; i < 10; i++) btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 0);
    CHECK(m.scroll_px == 800);
    fx = btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 0);
    CHECK(!(fx.flags & POCKET_FX_REDRAW));
    btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 0);
    CHECK(m.scroll_px == 625);

    // Same id: text replaced, scroll kept (clamped by the new metrics).
    reply(1, true, "first, longer");
    CHECK(m.scroll_px == 625 && m.reply_final && m.reply_version == 2);
    pocket_model_reply_metrics(&m, 500, 200, 25);
    CHECK(m.scroll_px == 300);

    // New id: replaces the old reply and starts at the top.
    reply(2, false, "second");
    CHECK(m.reply_id == 2 && m.scroll_px == 0 && !m.reply_final);

    // Short content never scrolls.
    pocket_model_reply_metrics(&m, 100, 200, 25);
    btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 0);
    CHECK(m.scroll_px == 0);

    // A reply during recording is stored but does not cover the recording.
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 1000);
    tick(1300);
    fx = reply(3, true, "late");
    CHECK(m.screen == POCKET_SCR_RECORDING && m.reply_id == 3);
    CHECK(fx.flags & POCKET_FX_REPLY_TEXT);
}

static void test_settings(void)
{
    fresh(false);
    reply(1, true, "x");
    pocket_fx_t fx = btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 0);
    CHECK(m.screen == POCKET_SCR_SETTINGS && m.settings_sel == POCKET_SET_PREROLL);
    // OK in settings never starts a press.
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 10);
    CHECK(!(fx.flags & POCKET_FX_AUDIO_ARM));
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 20);
    CHECK(m.preroll_on && (fx.flags & POCKET_FX_SAVE_SETTINGS) && (fx.flags & POCKET_FX_SEND_INFO));
    CHECK((fx.flags & POCKET_FX_CAPTURE) && fx.capture_wanted);

    btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 30);
    CHECK(m.settings_sel == POCKET_SET_BRIGHTNESS);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 40);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 50);
    CHECK((fx.flags & POCKET_FX_BACKLIGHT) && fx.backlight_pct == 100 && m.brightness_index == 2);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 60);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 70);
    CHECK(fx.backlight_pct == 30);

    // A reply while in settings is shown after leaving.
    reply(2, true, "new");
    CHECK(m.screen == POCKET_SCR_SETTINGS);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 80);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 90);
    CHECK(m.settings_sel == POCKET_SET_BACK);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 100);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 110);
    CHECK(m.screen == POCKET_SCR_REPLY && m.reply_id == 2);

    // Re-pair needs a confirmation; UP cancels, OK deletes the bond.
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 200);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 210);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 220);
    CHECK(m.settings_sel == POCKET_SET_REPAIR);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 230);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 240);
    CHECK(m.screen == POCKET_SCR_REPAIR_CONFIRM);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 250);
    CHECK(m.screen == POCKET_SCR_SETTINGS);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 260);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 270);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 280);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 290);
    CHECK(fx.flags & POCKET_FX_DELETE_BONDS);
    link_to(false, false, false, 300);
    CHECK(m.screen == POCKET_SCR_PAIRING);

    // Long-press UP closes settings too.
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 400);
    CHECK(m.screen == POCKET_SCR_SETTINGS);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 500);
    CHECK(m.screen == POCKET_SCR_PAIRING);
}

static void test_pairing(void)
{
    init_model(1, false, 1, POCKET_SCREEN_OFF_DEFAULT_INDEX);
    link_to(false, true, false, 0);
    pocket_fx_t fx = { 0 };
    pocket_model_pair_request(&m, 123456, 0, &fx);
    CHECK(m.screen == POCKET_SCR_PAIR_CONFIRM && m.passkey == 123456);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 10);
    CHECK(!(fx.flags & POCKET_FX_AUDIO_ARM) && !m.refused_flash);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 20);
    CHECK((fx.flags & POCKET_FX_PAIR_ACCEPT) && m.passkey == 0);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 30);  // answered once only
    CHECK(!(fx.flags & POCKET_FX_PAIR_ACCEPT));
    memset(&fx, 0, sizeof fx);
    pocket_model_pair_finished(&m, &fx);
    CHECK(m.screen == POCKET_SCR_PAIRING);
    link_to(true, true, true, 40);
    CHECK(m.screen == POCKET_SCR_HOME);

    // DOWN rejects; disconnect leaves the confirm screen.
    link_to(false, true, false, 50);
    memset(&fx, 0, sizeof fx);
    pocket_model_pair_request(&m, 42, 0, &fx);
    fx = btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 60);
    CHECK(fx.flags & POCKET_FX_PAIR_REJECT);
    link_to(false, false, false, 70);
    CHECK(m.screen == POCKET_SCR_PAIRING);
}

static void test_battery(void)
{
    fresh(false);
    pocket_fx_t fx = { 0 };
    pocket_model_battery(&m, 80, POCKET_UNKNOWN_U8, &fx);
    CHECK(fx.flags & POCKET_FX_SEND_STATUS);
    memset(&fx, 0, sizeof fx);
    pocket_model_battery(&m, 80, POCKET_UNKNOWN_U8, &fx);
    CHECK(fx.flags == 0);
    link_to(true, false, false, 1);
    pocket_model_battery(&m, 79, POCKET_UNKNOWN_U8, &fx);
    CHECK(!(fx.flags & POCKET_FX_SEND_STATUS));
}

static pocket_fx_t app_state(uint8_t code)
{
    pocket_downlink_t d = { .type = POCKET_MSG_APP_STATE, .code = code };
    pocket_fx_t fx = { 0 };
    pocket_model_downlink(&m, &d, false, 0, &fx);
    return fx;
}

static void test_reply_done_zero(void)
{
    // Phone gives up without a reply: keepalives stop, back to home.
    fresh(false);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 0);
    tick(400);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 1000);
    CHECK(m.screen == POCKET_SCR_SENDING && m.waiting);
    pocket_fx_t fx = reply_done(0);
    CHECK(!m.waiting && m.screen == POCKET_SCR_HOME && (fx.flags & POCKET_FX_REDRAW));
    fx = tick(1000 + 2 * POCKET_KEEPALIVE_INTERVAL_MS);
    CHECK(!(fx.flags & POCKET_FX_SEND_KEEPALIVE));

    // From the transcript screen with an older reply: back to that reply.
    reply(5, true, "old answer");
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 20000);
    tick(20400);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 21000);
    result(m.press_id, 0, "question");
    CHECK(m.screen == POCKET_SCR_THINKING);
    reply_done(0);
    CHECK(!m.waiting && m.screen == POCKET_SCR_REPLY && m.reply_id == 5 && !m.reply_done);

    // Not on a waiting screen: the screen stays.
    init_model(1, false, 1, POCKET_SCREEN_OFF_DEFAULT_INDEX);
    link_to(true, true, true, 0);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 10);
    reply_done(0);
    CHECK(m.screen == POCKET_SCR_SETTINGS);
}

static pocket_fx_t work(uint8_t phase)
{
    pocket_downlink_t d = { .type = POCKET_MSG_WORK, .code = phase };
    pocket_fx_t fx = { 0 };
    pocket_model_downlink(&m, &d, false, 0, &fx);
    return fx;
}

static void test_work(void)
{
    fresh(false);
    // Not waiting: WORK is ignored.
    pocket_fx_t fx = work(POCKET_WORK_TOOL);
    CHECK(fx.flags == 0 && m.screen == POCKET_SCR_HOME);

    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 0);
    tick(400);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 1000);
    CHECK(m.work_phase == POCKET_WORK_RECEIVED && m.work_active);
    // Before RESULT the phase is recorded but the sending screen stays.
    fx = work(POCKET_WORK_RECEIVED);
    CHECK(m.screen == POCKET_SCR_SENDING && !(fx.flags & POCKET_FX_REDRAW));
    result(m.press_id, 0, "question");
    CHECK(m.screen == POCKET_SCR_THINKING && m.work_phase == POCKET_WORK_RECEIVED);
    fx = work(POCKET_WORK_THINKING);
    CHECK(m.work_phase == POCKET_WORK_THINKING && (fx.flags & POCKET_FX_REDRAW));
    fx = work(POCKET_WORK_THINKING);
    CHECK(!(fx.flags & POCKET_FX_REDRAW));
    work(POCKET_WORK_TOOL);
    CHECK(m.work_phase == POCKET_WORK_TOOL && m.work_active);
    work(9);  // a future phase is shown as thinking
    CHECK(m.work_phase == POCKET_WORK_THINKING);
    // Idle stops the animation, keeps the label and the wait.
    fx = work(POCKET_WORK_IDLE);
    CHECK(!m.work_active && m.work_phase == POCKET_WORK_THINKING && m.waiting &&
          (fx.flags & POCKET_FX_REDRAW) && m.screen == POCKET_SCR_THINKING);
    fx = tick(1000 + POCKET_KEEPALIVE_INTERVAL_MS);
    CHECK(fx.flags & POCKET_FX_SEND_KEEPALIVE);
    work(POCKET_WORK_TOOL);
    CHECK(m.work_active);
    // The reply replaces the working screen; WORK during streaming keeps it.
    reply(3, false, "part");
    fx = work(POCKET_WORK_TOOL);
    CHECK(m.screen == POCKET_SCR_REPLY && !(fx.flags & POCKET_FX_REDRAW));
    reply_done(3);
    CHECK(!m.waiting);
    work(POCKET_WORK_THINKING);
    CHECK(m.screen == POCKET_SCR_REPLY);

    // Working state survives the settings menu.
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 10000);
    tick(10400);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 11000);
    result(m.press_id, 0, "again");
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 12000);
    work(POCKET_WORK_TOOL);
    CHECK(m.screen == POCKET_SCR_SETTINGS);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 13000);
    CHECK(m.screen == POCKET_SCR_THINKING && m.work_phase == POCKET_WORK_TOOL);

    // Link loss ends the wait; a late RESULT then shows nothing.
    uint16_t pid = m.press_id;
    link_to(true, false, false, 14000);
    CHECK(!m.waiting && m.screen == POCKET_SCR_OFFLINE);
    link_to(true, true, true, 15000);
    result(pid, 0, "late");
    CHECK(m.screen == POCKET_SCR_REPLY);
}

static void test_proto_mismatch(void)
{
    fresh(true);
    CHECK(pocket_model_capture_wanted(&m));
    pocket_fx_t fx = app_state(POCKET_APP_PROTO_MISMATCH);
    CHECK(m.screen == POCKET_SCR_PROTO_MISMATCH && m.proto_mismatch);
    CHECK((fx.flags & POCKET_FX_CAPTURE) && !fx.capture_wanted);
    // Presses are refused while mismatched.
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 100);
    CHECK(!(fx.flags & POCKET_FX_AUDIO_ARM) && m.refused_flash);
    fx = tick(1000);
    CHECK(!(fx.flags & POCKET_FX_AUDIO_STREAM));
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 1100);
    app_state(POCKET_APP_OK);
    CHECK(!m.proto_mismatch && m.screen == POCKET_SCR_HOME);

    // Mismatch mid-press aborts the stream.
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 2000);
    tick(2400);
    CHECK(m.screen == POCKET_SCR_RECORDING);
    fx = app_state(POCKET_APP_PROTO_MISMATCH);
    CHECK((fx.flags & POCKET_FX_AUDIO_ABORT) && m.screen == POCKET_SCR_PROTO_MISMATCH);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 3000);
    CHECK(!(fx.flags & POCKET_FX_AUDIO_FINISH));

    // Link loss clears it; reconnect goes to home.
    link_to(true, false, false, 4000);
    CHECK(!m.proto_mismatch && m.screen == POCKET_SCR_OFFLINE);
    link_to(true, true, true, 5000);
    CHECK(m.screen == POCKET_SCR_HOME);

    // Arriving while in settings: shown when settings close.
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 6000);
    app_state(POCKET_APP_PROTO_MISMATCH);
    CHECK(m.screen == POCKET_SCR_SETTINGS);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 7000);
    CHECK(m.screen == POCKET_SCR_PROTO_MISMATCH);
}

static void test_app_state_resends_info(void)
{
    // A relaunched phone app re-subscribes without a link event; its
    // APP_STATE is answered with INFO, whatever the state value.
    fresh(true);
    pocket_fx_t fx = app_state(POCKET_APP_OK);
    CHECK(fx.flags & POCKET_FX_SEND_INFO);
    fx = app_state(POCKET_APP_OK);
    CHECK(fx.flags & POCKET_FX_SEND_INFO);
    fx = app_state(POCKET_APP_SERVER_UNREACHABLE);
    CHECK(fx.flags & POCKET_FX_SEND_INFO);
    // A press in progress keeps streaming.
    app_state(POCKET_APP_OK);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 100);
    tick(500);
    CHECK(m.screen == POCKET_SCR_RECORDING);
    fx = app_state(POCKET_APP_OK);
    CHECK((fx.flags & POCKET_FX_SEND_INFO) && !(fx.flags & POCKET_FX_AUDIO_ABORT));
    CHECK(m.screen == POCKET_SCR_RECORDING);
    // No INFO while the link is not ready.
    fresh(false);
    link_to(true, true, false, 1000);
    fx = app_state(POCKET_APP_OK);
    CHECK(!(fx.flags & POCKET_FX_SEND_INFO));
}

static pocket_fx_t app_state_lang(uint8_t code, bool has_lang, uint8_t lang)
{
    pocket_downlink_t d = { .type = POCKET_MSG_APP_STATE, .code = code, .has_lang = has_lang,
                            .lang = lang };
    pocket_fx_t fx = { 0 };
    pocket_model_downlink(&m, &d, false, 0, &fx);
    return fx;
}

static void test_language(void)
{
    // Default zh-Hans; the phone's language applies at once, redraws and is
    // saved; a missing byte (1.0/1.1 phone) or an unknown value keeps it.
    fresh(false);
    CHECK(m.lang == POCKET_LANG_ZH_HANS);
    pocket_fx_t fx = app_state_lang(POCKET_APP_OK, true, POCKET_LANG_EN);
    CHECK(m.lang == POCKET_LANG_EN);
    CHECK((fx.flags & POCKET_FX_SAVE_SETTINGS) && (fx.flags & POCKET_FX_REDRAW));
    pocket_settings_t st;
    pocket_model_settings(&m, &st);
    CHECK(st.lang == POCKET_LANG_EN);
    fx = app_state_lang(POCKET_APP_OK, true, POCKET_LANG_EN);  // unchanged: nothing to save
    CHECK(!(fx.flags & POCKET_FX_SAVE_SETTINGS));
    fx = app_state_lang(POCKET_APP_OK, false, 0);  // old phone: keep English
    CHECK(m.lang == POCKET_LANG_EN && !(fx.flags & POCKET_FX_SAVE_SETTINGS));
    fx = app_state_lang(POCKET_APP_OK, true, 2);  // unknown value: keep
    CHECK(m.lang == POCKET_LANG_EN && !(fx.flags & POCKET_FX_SAVE_SETTINGS));
    // The state byte still acts with a language byte.
    fx = app_state_lang(POCKET_APP_SERVER_UNREACHABLE, true, POCKET_LANG_ZH_HANS);
    CHECK(m.lang == POCKET_LANG_ZH_HANS && m.server_unreachable);
    CHECK(fx.flags & POCKET_FX_SEND_INFO);
    // Restored from settings; an out-of-range stored value falls back.
    pocket_settings_t s = { .lang = POCKET_LANG_EN };
    pocket_model_init(&m, 1, &s);
    CHECK(m.lang == POCKET_LANG_EN);
    s.lang = 9;
    pocket_model_init(&m, 1, &s);
    CHECK(m.lang == POCKET_LANG_DEFAULT);
}

// --- Standby ------------------------------------------------------------------

#define OFF_MS 30000u  // default screen-off delay (POCKET_SCREEN_OFF_DEFAULT_INDEX)

static pocket_fx_t app_state_at(uint8_t code, uint32_t t)
{
    pocket_downlink_t d = { .type = POCKET_MSG_APP_STATE, .code = code };
    pocket_fx_t fx = { 0 };
    pocket_model_downlink(&m, &d, false, t, &fx);
    return fx;
}

static pocket_fx_t reply_at(uint32_t id, const char *text, uint32_t t)
{
    pocket_downlink_t d = { .type = POCKET_MSG_REPLY, .reply_id = id, .final = true,
                            .text = (const uint8_t *)text, .text_len = strlen(text) };
    pocket_fx_t fx = { 0 };
    pocket_model_downlink(&m, &d, false, t, &fx);
    return fx;
}

static bool went_dark(pocket_fx_t fx)
{
    return (fx.flags & POCKET_FX_SCREEN) && !fx.screen_on;
}

static bool lit(pocket_fx_t fx)
{
    return (fx.flags & POCKET_FX_SCREEN) && fx.screen_on;
}

// Blanks the screen of a fresh linked model (activity at 0).
static void go_dark(void)
{
    CHECK(went_dark(tick(OFF_MS)) && !m.screen_on);
}

static void test_screen_off_delay(void)
{
    fresh(false);
    CHECK(m.screen_on && pocket_model_screen_off_s(&m) == 30);
    CHECK(pocket_model_tick_wait_ms(&m, 0) == OFF_MS);
    CHECK(pocket_model_tick_wait_ms(&m, 1000) == OFF_MS - 1000);
    CHECK(!(tick(OFF_MS - 1).flags & POCKET_FX_SCREEN) && m.screen_on);
    pocket_fx_t fx = tick(OFF_MS);
    CHECK(went_dark(fx) && !m.screen_on);
    CHECK(pocket_model_tick_wait_ms(&m, OFF_MS) == UINT32_MAX);
    CHECK(!(tick(OFF_MS * 3).flags & POCKET_FX_SCREEN));

    // A button restarts the delay.
    fresh(false);
    btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 10000);
    btn(POCKET_BTN_DOWN, POCKET_BTN_EV_UP, 10100);
    CHECK(!(tick(OFF_MS + 5000).flags & POCKET_FX_SCREEN));
    CHECK(pocket_model_tick_wait_ms(&m, OFF_MS + 5000) == 10100 + OFF_MS - (OFF_MS + 5000));
    CHECK(went_dark(tick(10100 + OFF_MS)));

    // New content restarts it; a battery change does not count.
    fresh(false);
    reply_at(1, "a", 20000);
    pocket_fx_t b = { 0 };
    pocket_model_battery(&m, 50, POCKET_UNKNOWN_U8, &b);
    CHECK(!(tick(OFF_MS + 1000).flags & POCKET_FX_SCREEN));
    CHECK(went_dark(tick(20000 + OFF_MS)));

    // "Never" (last level) holds the screen on; out-of-range index -> default.
    init_model(1, false, 1, POCKET_SCREEN_OFF_COUNT - 1);
    link_to(true, true, true, 0);
    CHECK(pocket_model_screen_off_s(&m) == 0);
    CHECK(pocket_model_tick_wait_ms(&m, 0) == UINT32_MAX);
    CHECK(!(tick(10u * OFF_MS).flags & POCKET_FX_SCREEN) && m.screen_on);
    init_model(1, false, 1, 200);
    CHECK(m.screen_off_index == POCKET_SCREEN_OFF_DEFAULT_INDEX);
}

static void test_wake_keys(void)
{
    // OK from dark starts the press at once and lights the screen.
    fresh(false);
    go_dark();
    pocket_fx_t fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 40000);
    CHECK(lit(fx) && (fx.flags & POCKET_FX_AUDIO_ARM) && m.screen_on);
    CHECK((fx.flags & POCKET_FX_CAPTURE) && fx.capture_wanted);
    fx = tick(40000 + POCKET_TAP_THRESHOLD_MS);
    CHECK((fx.flags & POCKET_FX_AUDIO_STREAM) && m.screen == POCKET_SCR_RECORDING);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 41000);
    CHECK((fx.flags & POCKET_FX_AUDIO_FINISH) && m.waiting);

    // UP/DOWN from dark only light the screen: no scroll, no settings.
    fresh(false);
    reply_at(1, "long reply", 0);
    pocket_model_reply_metrics(&m, 1000, 200, 26);
    go_dark();
    fx = btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 40000);
    CHECK(lit(fx) && m.scroll_px == 0 && !(fx.flags & POCKET_FX_REDRAW));
    btn(POCKET_BTN_DOWN, POCKET_BTN_EV_UP, 40100);
    fx = btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 40200);
    CHECK(m.scroll_px > 0 && !(fx.flags & POCKET_FX_SCREEN));
    btn(POCKET_BTN_DOWN, POCKET_BTN_EV_UP, 40300);

    // The long press of a waking UP does not open settings.
    CHECK(went_dark(tick(40300 + OFF_MS)));
    fx = btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 80000);
    CHECK(lit(fx));
    fx = btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 80500);
    CHECK(m.screen == POCKET_SCR_REPLY && !(fx.flags & POCKET_FX_REDRAW));
    btn(POCKET_BTN_UP, POCKET_BTN_EV_UP, 81000);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 81100);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 81600);
    CHECK(m.screen == POCKET_SCR_SETTINGS);

    // OK from dark while the phone is away: lit, nothing recorded.
    fresh(false);
    link_to(true, false, false, 100);
    CHECK(went_dark(tick(100 + OFF_MS)));
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 50000);
    CHECK(lit(fx) && !(fx.flags & POCKET_FX_AUDIO_ARM) && m.refused_flash);
}

static void test_screen_held(void)
{
    // Recording keeps the screen on, however long.
    fresh(false);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 1000);
    for (uint32_t t = 1050; t < 5u * OFF_MS; t += 50) CHECK(!(tick(t).flags & POCKET_FX_SCREEN));
    CHECK(m.screen == POCKET_SCR_RECORDING && m.screen_on);

    // A pending reply keeps it on; the tick only wakes for keepalives.
    pocket_fx_t fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 5u * OFF_MS);
    CHECK(m.waiting);
    uint32_t t0 = 5u * OFF_MS;
    CHECK(pocket_model_tick_wait_ms(&m, t0) == POCKET_KEEPALIVE_INTERVAL_MS);
    for (uint32_t t = t0; t < t0 + 4u * OFF_MS; t += POCKET_KEEPALIVE_INTERVAL_MS) {
        fx = tick(t);
        CHECK(!(fx.flags & POCKET_FX_SCREEN));
    }
    // The delay counts from the end of the wait.
    uint32_t end = t0 + 4u * OFF_MS;
    fx = (pocket_fx_t){ 0 };
    pocket_downlink_t d = { .type = POCKET_MSG_REPLY_DONE, .reply_id = 0 };
    pocket_model_downlink(&m, &d, false, end, &fx);
    CHECK(!m.waiting && m.screen_on);
    CHECK(!(tick(end + OFF_MS - 1).flags & POCKET_FX_SCREEN));
    CHECK(went_dark(tick(end + OFF_MS)));

    // Settings keep it on.
    fresh(false);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 0);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 500);
    CHECK(m.screen == POCKET_SCR_SETTINGS);
    CHECK(pocket_model_tick_wait_ms(&m, 500) == UINT32_MAX);
    CHECK(!(tick(10u * OFF_MS).flags & POCKET_FX_SCREEN));
    btn(POCKET_BTN_UP, POCKET_BTN_EV_UP, 10u * OFF_MS);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 10u * OFF_MS + 10);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 10u * OFF_MS + 510);
    CHECK(m.screen != POCKET_SCR_SETTINGS);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_UP, 10u * OFF_MS + 600);
    CHECK(went_dark(tick(10u * OFF_MS + 600 + OFF_MS)));

    // The pairing code keeps it on and a pairing request lights it.
    init_model(1, false, 1, POCKET_SCREEN_OFF_DEFAULT_INDEX);
    link_to(false, false, false, 0);
    CHECK(went_dark(tick(OFF_MS)) && m.screen == POCKET_SCR_PAIRING);
    link_to(false, true, false, 50000);
    CHECK(!m.screen_on);
    fx = (pocket_fx_t){ 0 };
    pocket_model_pair_request(&m, 123456, 60000, &fx);
    CHECK(lit(fx) && m.screen == POCKET_SCR_PAIR_CONFIRM);
    CHECK(!(tick(60000 + 5u * OFF_MS).flags & POCKET_FX_SCREEN));
    fx = (pocket_fx_t){ 0 };
    pocket_model_pair_finished(&m, &fx);
    CHECK(m.screen == POCKET_SCR_PAIRING);
}

static void test_wake_events(void)
{
    // A reply lights the screen and restarts the delay.
    fresh(false);
    go_dark();
    pocket_fx_t fx = reply_at(7, "hi", 50000);
    CHECK(lit(fx) && m.screen == POCKET_SCR_REPLY);
    CHECK(pocket_model_tick_wait_ms(&m, 50000) == OFF_MS);

    // Link loss lights it; the link coming back does not.
    fresh(false);
    go_dark();
    fx = link_to(true, false, false, 40000);
    CHECK(lit(fx) && m.screen == POCKET_SCR_OFFLINE);
    CHECK(went_dark(tick(40000 + OFF_MS)));
    fx = link_to(true, true, true, 90000);
    CHECK(!(fx.flags & POCKET_FX_SCREEN) && !m.screen_on && m.screen == POCKET_SCR_HOME);

    // Errors: protocol mismatch and an unreachable server light it; a
    // healthy APP_STATE and a battery change do not.
    fresh(false);
    go_dark();
    fx = app_state_at(POCKET_APP_OK, 40000);
    CHECK(!(fx.flags & POCKET_FX_SCREEN));
    pocket_fx_t b = { 0 };
    pocket_model_battery(&m, 40, POCKET_UNKNOWN_U8, &b);
    CHECK(!(b.flags & POCKET_FX_SCREEN) && !m.screen_on);
    fx = app_state_at(POCKET_APP_SERVER_UNREACHABLE, 41000);
    CHECK(lit(fx));
    CHECK(went_dark(tick(41000 + OFF_MS)));
    fx = app_state_at(POCKET_APP_PROTO_MISMATCH, 80000);
    CHECK(lit(fx) && m.screen == POCKET_SCR_PROTO_MISMATCH);
}

static void test_screen_off_setting_and_preroll(void)
{
    fresh(true);
    CHECK(pocket_model_capture_wanted(&m));
    // Pre-roll stops while dark and resumes when the screen lights.
    pocket_fx_t fx = tick(OFF_MS);
    CHECK(went_dark(fx) && (fx.flags & POCKET_FX_CAPTURE) && !fx.capture_wanted);
    fx = btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 40000);
    CHECK(lit(fx) && (fx.flags & POCKET_FX_CAPTURE) && fx.capture_wanted);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_UP, 40100);

    // The settings row cycles 30 s -> 60 s -> never -> 15 s -> 30 s.
    btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 41000);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 41500);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_UP, 41600);
    btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 41700);
    btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 41800);
    CHECK(m.settings_sel == POCKET_SET_SCREEN_OFF);
    static const uint32_t expect[] = { 60, 0, 15, 30 };
    for (int i = 0; i < 4; i++) {
        btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 42000 + (uint32_t)i * 100);
        fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 42050 + (uint32_t)i * 100);
        CHECK((fx.flags & POCKET_FX_SAVE_SETTINGS) && (fx.flags & POCKET_FX_REDRAW));
        CHECK(pocket_model_screen_off_s(&m) == expect[i]);
    }
}

static pocket_fx_t down_at(pocket_downlink_t d, uint32_t t)
{
    pocket_fx_t fx = { 0 };
    pocket_model_downlink(&m, &d, false, t, &fx);
    return fx;
}

static pocket_fx_t work_at(uint8_t phase, uint32_t t)
{
    return down_at((pocket_downlink_t){ .type = POCKET_MSG_WORK, .code = phase }, t);
}

static pocket_fx_t part_at(uint32_t id, bool final, const char *text, uint32_t t)
{
    return down_at((pocket_downlink_t){ .type = POCKET_MSG_REPLY, .reply_id = id, .final = final,
                                        .text = (const uint8_t *)text,
                                        .text_len = strlen(text) }, t);
}

static pocket_fx_t done_at(uint32_t id, uint32_t t)
{
    return down_at((pocket_downlink_t){ .type = POCKET_MSG_REPLY_DONE, .reply_id = id }, t);
}

// Talk from t to t + 1000 ms; the wait for press_id starts at t + 1000.
static uint16_t talk(uint32_t t)
{
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, t);
    tick(t + POCKET_TAP_THRESHOLD_MS);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, t + 1000);
    CHECK(m.waiting);
    return m.wait_press_id;
}

// No screen change on any keepalive tick in [from, to).
static void quiet_ticks(uint32_t from, uint32_t to)
{
    for (uint32_t t = from; t < to; t += 1000) CHECK(!(tick(t).flags & POCKET_FX_SCREEN));
}

static void test_answer_hold(void)
{
    // Sending, RESULT 0, received / thinking / tool: on, no countdown.
    fresh(false);
    uint16_t id = talk(1000);
    CHECK(pocket_model_tick_wait_ms(&m, 2000) == POCKET_KEEPALIVE_INTERVAL_MS);
    quiet_ticks(2000, 2000 + 3 * OFF_MS);
    CHECK(m.screen == POCKET_SCR_SENDING && m.screen_on);
    down_at((pocket_downlink_t){ .type = POCKET_MSG_RESULT, .press_id = id, .code = 0,
                                 .text = (const uint8_t *)"q", .text_len = 1 }, 100000);
    CHECK(m.screen == POCKET_SCR_THINKING);
    work_at(POCKET_WORK_THINKING, 110000);
    work_at(POCKET_WORK_TOOL, 120000);
    quiet_ticks(120000, 120000 + 3 * OFF_MS);

    // REPLY streaming (final 0) holds, even after WORK idle.
    part_at(9, false, "par", 300000);
    work_at(POCKET_WORK_IDLE, 301000);
    quiet_ticks(301000, 301000 + 3 * OFF_MS);
    // Final text and the brain idle: quiet, the countdown runs from the text.
    part_at(9, true, "part", 500000);
    CHECK(pocket_model_tick_wait_ms(&m, 500000) <= OFF_MS);
    CHECK(!(tick(500000 + OFF_MS - 1).flags & POCKET_FX_SCREEN));
    // REPLY_DONE restarts it (the wait ends), whatever its reply id.
    done_at(77, 510000);
    CHECK(!m.waiting);
    CHECK(!(tick(500000 + OFF_MS).flags & POCKET_FX_SCREEN));
    CHECK(went_dark(tick(510000 + OFF_MS)));

    // Final text while the brain is still active: on until REPLY_DONE, then
    // 30 s from REPLY_DONE.
    fresh(false);
    talk(1000);
    part_at(3, true, "answer", 50000);
    quiet_ticks(50000, 50000 + 3 * OFF_MS);
    done_at(3, 150000);
    CHECK(!m.waiting);
    CHECK(!(tick(150000 + OFF_MS - 1).flags & POCKET_FX_SCREEN));
    CHECK(went_dark(tick(150000 + OFF_MS)));
    // A new REPLY lights it and restarts the countdown; so does a key.
    CHECK(lit(part_at(4, true, "more", 200000)));
    btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 220000);
    btn(POCKET_BTN_DOWN, POCKET_BTN_EV_UP, 220100);
    CHECK(!(tick(200000 + OFF_MS).flags & POCKET_FX_SCREEN));
    CHECK(went_dark(tick(220100 + OFF_MS)));

    // WORK idle without text: the countdown starts at WORK idle, stops when
    // the brain resumes, and runs again from the next WORK idle.
    fresh(false);
    talk(1000);
    work_at(POCKET_WORK_IDLE, 10000);
    CHECK(m.waiting && pocket_model_tick_wait_ms(&m, 10000) <= POCKET_KEEPALIVE_INTERVAL_MS);
    work_at(POCKET_WORK_THINKING, 20000);
    quiet_ticks(20000, 20000 + 2 * OFF_MS);
    work_at(POCKET_WORK_IDLE, 90000);
    CHECK(!(tick(90000 + OFF_MS - 1).flags & POCKET_FX_SCREEN));
    CHECK(went_dark(tick(90000 + OFF_MS)) && m.waiting);
    // Keepalives continue while dark.
    CHECK(tick(90000 + OFF_MS + POCKET_KEEPALIVE_INTERVAL_MS).flags & POCKET_FX_SEND_KEEPALIVE);
    // The reply lights the screen; streaming holds it until REPLY_DONE 0.
    CHECK(lit(part_at(5, false, "a", 200000)));
    quiet_ticks(200000, 200000 + 2 * OFF_MS);
    done_at(0, 300000);
    CHECK(!(tick(300000 + OFF_MS - 1).flags & POCKET_FX_SCREEN));
    CHECK(went_dark(tick(300000 + OFF_MS)));
}

static pocket_fx_t usb(bool connected, uint32_t t)
{
    pocket_fx_t fx = { 0 };
    pocket_model_usb_host(&m, connected, t, &fx);
    return fx;
}

static void test_usb_host(void)
{
    // A USB host holds the screen on with no countdown.
    fresh(false);
    pocket_fx_t fx = usb(true, 1000);
    CHECK(!(fx.flags & POCKET_FX_SCREEN) && m.screen_on);
    CHECK(pocket_model_tick_wait_ms(&m, 1000) == UINT32_MAX);
    CHECK(!(tick(10u * OFF_MS).flags & POCKET_FX_SCREEN));
    CHECK(m.screen_on);
    // A repeated report is not activity.
    fx = usb(true, 10u * OFF_MS);
    CHECK(fx.flags == 0);

    // On disconnect the full delay runs from the disconnect.
    uint32_t off = 20u * OFF_MS;
    usb(false, off);
    CHECK(pocket_model_tick_wait_ms(&m, off) == OFF_MS);
    CHECK(!(tick(off + OFF_MS - 1).flags & POCKET_FX_SCREEN));
    CHECK(went_dark(tick(off + OFF_MS)));

    // A host seen while dark lights the screen and holds it.
    fx = usb(true, off + 2u * OFF_MS);
    CHECK((fx.flags & POCKET_FX_SCREEN) && fx.screen_on && m.screen_on);
    CHECK(!(tick(off + 5u * OFF_MS).flags & POCKET_FX_SCREEN));

    // Keys still work normally while held.
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, off + 5u * OFF_MS);
    CHECK(fx.flags & POCKET_FX_AUDIO_ARM);
}

static void test_history(void)
{
    fresh(false);
    // Boot with stored replies: the newest is shown once linked.
    init_model(100, false, 1, POCKET_SCREEN_OFF_DEFAULT_INDEX);
    pocket_fx_t fx = { 0 };
    pocket_model_show_stored(&m, 30, 1, 3, false, &fx);
    CHECK(m.has_reply && m.reply_final && m.hist_pos == 1 && m.hist_count == 3);
    link_to(true, true, true, 0);
    CHECK(m.screen == POCKET_SCR_REPLY);

    // Scroll inside the reply first, then step at the ends.
    pocket_model_reply_metrics(&m, 500, 200, 25);
    fx = btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 0);
    CHECK(m.scroll_px == 175 && !(fx.flags & POCKET_FX_HIST_NEXT));
    fx = btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 0);
    CHECK(m.scroll_px == 0 && !(fx.flags & POCKET_FX_HIST_PREV));
    fx = btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 0);
    CHECK(fx.flags & POCKET_FX_HIST_PREV);
    // Newest at the bottom: nothing newer.
    btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 0);
    btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 0);
    fx = btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 0);
    CHECK(m.scroll_px == 300 && !(fx.flags & (POCKET_FX_HIST_NEXT | POCKET_FX_REDRAW)));

    // The platform shows the older reply at its end.
    fx = (pocket_fx_t){ 0 };
    pocket_model_show_stored(&m, 29, 2, 3, true, &fx);
    CHECK(fx.flags & POCKET_FX_REDRAW);
    pocket_model_reply_metrics(&m, 700, 200, 25);
    CHECK(m.scroll_px == 500);
    fx = btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 0);
    CHECK(fx.flags & POCKET_FX_HIST_NEXT);
    // The oldest at its top: nothing older.
    fx = (pocket_fx_t){ 0 };
    pocket_model_show_stored(&m, 28, 3, 3, false, &fx);
    pocket_model_reply_metrics(&m, 100, 200, 25);
    fx = btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 0);
    CHECK(!(fx.flags & POCKET_FX_HIST_PREV));

    // A new reply streams unstored: no stepping until it is final and stored.
    fx = reply(31, false, "streaming");
    CHECK(m.hist_pos == 0 && !(fx.flags & POCKET_FX_HIST_SAVE) && m.scroll_px == 0);
    pocket_model_reply_metrics(&m, 100, 200, 25);
    fx = btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 0);
    CHECK(!(fx.flags & POCKET_FX_HIST_PREV));
    fx = reply(31, true, "streaming done");
    CHECK(fx.flags & POCKET_FX_HIST_SAVE);
    fx = reply_done(31);
    CHECK(!(fx.flags & POCKET_FX_HIST_SAVE));
    fx = (pocket_fx_t){ 0 };
    pocket_model_hist_position(&m, 1, 4, &fx);
    CHECK(fx.flags & POCKET_FX_REDRAW);
    fx = btn(POCKET_BTN_UP, POCKET_BTN_EV_DOWN, 0);
    CHECK(fx.flags & POCKET_FX_HIST_PREV);

    // Partial texts closed by REPLY_DONE are stored at REPLY_DONE.
    reply(32, false, "part");
    fx = reply_done(32);
    CHECK((fx.flags & POCKET_FX_HIST_SAVE) && m.reply_final);

    // An answer already stored is shown, not stored again.
    fx = reply_stored(30, "old");
    CHECK(m.reply_id == 30 && !(fx.flags & POCKET_FX_HIST_SAVE) && (fx.flags & POCKET_FX_REPLY_TEXT));

    // A final reply during a press is stored when the press ends.
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 1000);
    tick(1400);
    fx = reply(33, true, "during press");
    CHECK(!(fx.flags & POCKET_FX_HIST_SAVE) && m.hist_save_pending);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 2000);
    CHECK((fx.flags & POCKET_FX_HIST_SAVE) && !m.hist_save_pending);
    // ... and also when the press was only a tap.
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 3000);
    fx = reply(34, true, "during tap");
    CHECK(!(fx.flags & POCKET_FX_HIST_SAVE));
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 3100);
    CHECK(fx.flags & POCKET_FX_HIST_SAVE);

    // Re-pairing forgets the stored replies.
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 4000);
    m.settings_sel = POCKET_SET_REPAIR;
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 4100);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 4200);
    CHECK(m.screen == POCKET_SCR_REPAIR_CONFIRM);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 4300);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 4400);
    CHECK((fx.flags & POCKET_FX_DELETE_BONDS) && !m.has_reply && m.hist_count == 0);
}

static void settings_default(pocket_settings_t *st)
{
    *st = (pocket_settings_t){ .preroll_on = false, .brightness_index = 1,
                               .screen_off_index = POCKET_SCREEN_OFF_DEFAULT_INDEX,
                               .deep_sleep_index = POCKET_DEEP_SLEEP_DEFAULT_INDEX,
                               .alert_screen = POCKET_ALERT_SCREEN_DEFAULT,
                               .alert_tone = POCKET_ALERT_TONE_DEFAULT };
}

#define HOUR_MS 3600000u

static void test_deep_sleep(void)
{
    pocket_settings_t st;
    settings_default(&st);
    pocket_model_init(&m, 1, &st);
    CHECK(pocket_model_deep_sleep_s(&m) == 3600);
    link_to(true, true, true, 0);
    // The screen goes dark first; deep sleep is the next deadline.
    tick(OFF_MS);
    CHECK(pocket_model_tick_wait_ms(&m, OFF_MS) == HOUR_MS - OFF_MS);
    CHECK(!(tick(HOUR_MS - 1).flags & POCKET_FX_DEEP_SLEEP));
    pocket_fx_t fx = tick(HOUR_MS);
    CHECK(fx.flags & POCKET_FX_DEEP_SLEEP);
    CHECK(!(tick(HOUR_MS + 10).flags & POCKET_FX_DEEP_SLEEP));  // requested once

    // Events restart the delay: a key, a REPLY, a WORK, a RESULT.
    pocket_model_init(&m, 1, &st);
    link_to(true, true, true, 0);
    btn(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN, 1000);
    CHECK(!(tick(HOUR_MS).flags & POCKET_FX_DEEP_SLEEP));
    CHECK(tick(HOUR_MS + 1000).flags & POCKET_FX_DEEP_SLEEP);
    pocket_model_init(&m, 1, &st);
    link_to(true, true, true, 0);
    reply_at(5, "x", 2000);
    CHECK(!(tick(HOUR_MS + 1999).flags & POCKET_FX_DEEP_SLEEP));
    CHECK(tick(HOUR_MS + 2000).flags & POCKET_FX_DEEP_SLEEP);
    // A link change or a battery reading is not an event.
    pocket_model_init(&m, 1, &st);
    link_to(true, true, true, 0);
    link_to(true, false, false, 5000);
    link_to(true, true, true, 6000);
    pocket_fx_t b = { 0 };
    pocket_model_battery(&m, 40, POCKET_UNKNOWN_U8, &b);
    CHECK(tick(HOUR_MS).flags & POCKET_FX_DEEP_SLEEP);

    // A reply wait, a press and a USB host hold it off.
    pocket_model_init(&m, 1, &st);
    link_to(true, true, true, 0);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 0);
    tick(400);
    CHECK(!(tick(2 * HOUR_MS).flags & POCKET_FX_DEEP_SLEEP));  // still talking
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 2 * HOUR_MS);
    CHECK(m.waiting && !(tick(4 * HOUR_MS).flags & POCKET_FX_DEEP_SLEEP));
    reply_done(0);
    CHECK(!m.waiting && (tick(4 * HOUR_MS).flags & POCKET_FX_DEEP_SLEEP));
    pocket_model_init(&m, 1, &st);
    pocket_fx_t u = { 0 };
    pocket_model_usb_host(&m, true, 0, &u);
    CHECK(!(tick(2 * HOUR_MS).flags & POCKET_FX_DEEP_SLEEP));
    CHECK(pocket_model_tick_wait_ms(&m, 2 * HOUR_MS) == UINT32_MAX);
    pocket_model_usb_host(&m, false, 2 * HOUR_MS, &u);  // unplugging is an event
    CHECK(tick(3 * HOUR_MS).flags & POCKET_FX_DEEP_SLEEP);
    // The debug delay also runs on a USB host.
    pocket_model_init(&m, 1, &st);
    m.deep_sleep_test_s = 20;
    pocket_model_usb_host(&m, true, 0, &u);
    CHECK(tick(20000).flags & POCKET_FX_DEEP_SLEEP);

    // The setting cycles 30 min -> 1 h -> 4 h -> never; never never sleeps.
    pocket_model_init(&m, 1, &st);
    link_to(true, true, true, 0);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 0);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_UP, 10);
    m.settings_sel = POCKET_SET_DEEP_SLEEP;
    static const uint32_t want[] = { 14400, 0, 1800, 3600 };
    for (int i = 0; i < 4; i++) {
        btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 100);
        fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 110);
        CHECK((fx.flags & POCKET_FX_SAVE_SETTINGS) && pocket_model_deep_sleep_s(&m) == want[i]);
    }
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 200);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 210);
    CHECK(pocket_model_deep_sleep_s(&m) == 14400);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 300);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 310);
    CHECK(pocket_model_deep_sleep_s(&m) == 0);
    CHECK(!(tick(100 * HOUR_MS).flags & POCKET_FX_DEEP_SLEEP));
    pocket_settings_t out;
    pocket_model_settings(&m, &out);
    CHECK(out.deep_sleep_index == 3 && out.alert_screen && out.alert_tone);
}

static void test_boot_hold(void)
{
    pocket_settings_t st;
    settings_default(&st);
    pocket_model_init(&m, 1, &st);
    pocket_model_boot_hold(&m, true);
    link_to(true, true, true, 900);
    // The waking OK press: its edges do nothing, nothing records.
    pocket_fx_t fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 1000);
    CHECK(!(fx.flags & POCKET_FX_AUDIO_ARM) && m.press == PRESS_IDLE);
    tick(1500);
    CHECK(m.screen == POCKET_SCR_HOME);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 2000);
    CHECK(!(fx.flags & (POCKET_FX_AUDIO_STREAM | POCKET_FX_AUDIO_FINISH)) && !m.boot_hold);
    // The next press records.
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 3000);
    CHECK(fx.flags & POCKET_FX_AUDIO_ARM);
    // Released before the buttons started: the platform clears the hold.
    pocket_model_init(&m, 1, &st);
    pocket_model_boot_hold(&m, true);
    pocket_model_boot_hold(&m, false);
    link_to(true, true, true, 0);
    CHECK(btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 100).flags & POCKET_FX_AUDIO_ARM);
}

static void test_alerts(void)
{
    pocket_settings_t st;
    settings_default(&st);
    pocket_model_init(&m, 1, &st);
    link_to(true, true, true, 0);
    go_dark();
    // A new answer, proactive: screen on and a tone, once per reply_id.
    pocket_fx_t fx = part_at(10, false, "a", OFF_MS + 100);
    CHECK(lit(fx) && (fx.flags & POCKET_FX_TONE));
    fx = part_at(10, false, "ab", OFF_MS + 200);
    CHECK(!(fx.flags & POCKET_FX_TONE));
    fx = part_at(10, true, "abc", OFF_MS + 300);
    CHECK(!(fx.flags & POCKET_FX_TONE) && (fx.flags & POCKET_FX_HIST_SAVE));
    // An answer already stored, sent again while dark: shown, no alert.
    tick(OFF_MS * 3);
    CHECK(!m.screen_on);
    fx = reply_stored(9, "old");
    CHECK(!(fx.flags & (POCKET_FX_TONE | POCKET_FX_SCREEN)) && m.reply_id == 9);

    // Screen alert off: the tone plays, the screen stays dark.
    st.alert_screen = false;
    pocket_model_init(&m, 1, &st);
    link_to(true, true, true, 0);
    go_dark();
    fx = part_at(11, true, "x", OFF_MS + 100);
    CHECK(!(fx.flags & POCKET_FX_SCREEN) && !m.screen_on && (fx.flags & POCKET_FX_TONE));
    // Tone off too: quiet and dark, the reply is still the shown one.
    st.alert_tone = false;
    pocket_model_init(&m, 1, &st);
    link_to(true, true, true, 0);
    go_dark();
    fx = part_at(12, true, "y", OFF_MS + 100);
    CHECK(!(fx.flags & (POCKET_FX_SCREEN | POCKET_FX_TONE)) && m.screen == POCKET_SCR_REPLY);

    // No tone into a press (the mic would record it); the screen still lights.
    settings_default(&st);
    pocket_model_init(&m, 1, &st);
    link_to(true, true, true, 0);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 0);
    tick(400);
    fx = part_at(13, true, "z", 500);
    CHECK(!(fx.flags & POCKET_FX_TONE));

    // The settings rows toggle; turning the tone on plays it once.
    pocket_model_init(&m, 1, &st);
    link_to(true, true, true, 0);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_LONG, 0);
    btn(POCKET_BTN_UP, POCKET_BTN_EV_UP, 10);
    m.settings_sel = POCKET_SET_ALERT_SCREEN;
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 20);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 30);
    CHECK(!m.alert_screen && (fx.flags & POCKET_FX_SAVE_SETTINGS));
    m.settings_sel = POCKET_SET_ALERT_TONE;
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 40);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 50);
    CHECK(!m.alert_tone && !(fx.flags & POCKET_FX_TONE));
    btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, 60);
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, 70);
    CHECK(m.alert_tone && (fx.flags & POCKET_FX_TONE));
}

static void test_link_idle(void)
{
    pocket_settings_t st;
    settings_default(&st);
    pocket_model_init(&m, 1, &st);
    pocket_fx_t fx = link_to(true, true, true, 0);
    CHECK(!(fx.flags & POCKET_FX_LINK_IDLE));  // lit: the phone's own parameters stay
    fx = tick(OFF_MS);
    CHECK((fx.flags & POCKET_FX_LINK_IDLE) && fx.link_idle);
    // A press from dark restores them at once.
    fx = btn(POCKET_BTN_OK, POCKET_BTN_EV_DOWN, OFF_MS + 1000);
    CHECK((fx.flags & POCKET_FX_LINK_IDLE) && !fx.link_idle);
    tick(OFF_MS + 1400);
    btn(POCKET_BTN_OK, POCKET_BTN_EV_UP, OFF_MS + 3000);
    // Waiting keeps them responsive even after the screen goes dark.
    reply_done(0);  // the wait ends; the screen is still lit
    fx = tick(OFF_MS * 3);
    CHECK((fx.flags & POCKET_FX_LINK_IDLE) && fx.link_idle);
    // A dark reply with the screen alert off keeps the link idle.
    m.alert_screen = false;
    fx = part_at(20, true, "q", OFF_MS * 3 + 10);
    CHECK(!(fx.flags & POCKET_FX_LINK_IDLE) && m.link_idle);
    // Link loss: idle is off; when the link returns dark, idle is asked again.
    fx = link_to(true, false, false, OFF_MS * 3 + 20);
    CHECK(!m.link_idle);
    tick(OFF_MS * 5);
    fx = link_to(true, true, true, OFF_MS * 6);
    CHECK((fx.flags & POCKET_FX_LINK_IDLE) && fx.link_idle == !m.screen_on);
}

int main(void)
{
    test_boot_and_link();
    test_tap_ignored();
    test_hold_streams();
    test_link_down_refuses();
    test_wait_and_keepalive();
    test_reply_replace_and_scroll();
    test_settings();
    test_pairing();
    test_battery();
    test_reply_done_zero();
    test_proto_mismatch();
    test_work();
    test_app_state_resends_info();
    test_language();
    test_screen_off_delay();
    test_wake_keys();
    test_screen_held();
    test_wake_events();
    test_screen_off_setting_and_preroll();
    test_answer_hold();
    test_usb_host();
    test_history();
    test_deep_sleep();
    test_boot_hold();
    test_alerts();
    test_link_idle();
    CHECK_DONE("test_pocket_model");
    return 0;
}
