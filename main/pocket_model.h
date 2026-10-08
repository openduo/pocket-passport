// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Application state machine: press gesture, reply wait and keepalives, the
// latest reply with scrolling, settings and pairing screens. Pure C: inputs
// are events with millisecond timestamps, outputs are a view description and
// a set of effects the platform layer carries out. Host-tested.
#pragma once

#include "pocket_config.h"
#include "pocket_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    POCKET_SCR_BOOT = 0,
    POCKET_SCR_PAIRING,         // no bond: advertising for the phone
    POCKET_SCR_PAIR_CONFIRM,    // numeric comparison, OK accepts
    POCKET_SCR_OFFLINE,         // bonded, phone link down
    POCKET_SCR_HOME,            // linked, no reply yet
    POCKET_SCR_RECORDING,
    POCKET_SCR_SENDING,         // released, waiting for RESULT
    POCKET_SCR_THINKING,        // working state: transcript and phase, waiting for REPLY
    POCKET_SCR_REPLY,
    POCKET_SCR_NOTICE,          // a RESULT failure
    POCKET_SCR_SETTINGS,
    POCKET_SCR_REPAIR_CONFIRM,
    POCKET_SCR_PROTO_MISMATCH,  // APP_STATE 2: phone and device protocols differ
    POCKET_SCR_COUNT,
} pocket_screen_t;

typedef enum {
    POCKET_NOTICE_NONE = 0,
    POCKET_NOTICE_EMPTY,        // RESULT 1: didn't catch that
    POCKET_NOTICE_ASR_FAILED,   // RESULT 2
    POCKET_NOTICE_SEND_FAILED,  // RESULT 3
    POCKET_NOTICE_NO_SERVER,    // RESULT 4
} pocket_notice_t;

typedef enum {
    POCKET_SET_PREROLL = 0,
    POCKET_SET_BRIGHTNESS,
    POCKET_SET_SCREEN_OFF,
    POCKET_SET_DEEP_SLEEP,
    POCKET_SET_ALERT_SCREEN,
    POCKET_SET_ALERT_TONE,
    POCKET_SET_REPAIR,
    POCKET_SET_BACK,
    POCKET_SET_COUNT,
} pocket_setting_t;

typedef enum {
    POCKET_BTN_UP = 0,
    POCKET_BTN_DOWN,
    POCKET_BTN_OK,
} pocket_btn_t;

typedef enum {
    POCKET_BTN_EV_DOWN = 0,     // pressed
    POCKET_BTN_EV_UP,           // released
    POCKET_BTN_EV_LONG,         // held past the BSP long-press time
} pocket_btn_ev_t;

// Link as reported by the BLE layer.
typedef struct {
    bool bonded;                // a bond is stored
    bool connected;
    bool ready;                 // encrypted, authenticated, bonded, subscribed
} pocket_link_state_t;

// Effects (bit flags) requested by one model step.
enum {
    POCKET_FX_AUDIO_ARM      = 1u << 0,  // capture into the hold ring
    POCKET_FX_AUDIO_STREAM   = 1u << 1,  // press confirmed: stream press_id
    POCKET_FX_AUDIO_FINISH   = 1u << 2,  // release: end the press stream
    POCKET_FX_AUDIO_DISCARD  = 1u << 3,  // tap: drop held audio
    POCKET_FX_AUDIO_ABORT    = 1u << 4,  // link lost mid-press
    POCKET_FX_CAPTURE        = 1u << 5,  // capture_wanted changed
    POCKET_FX_SEND_KEEPALIVE = 1u << 6,
    POCKET_FX_SEND_INFO      = 1u << 7,
    POCKET_FX_SEND_STATUS    = 1u << 8,
    POCKET_FX_PAIR_ACCEPT    = 1u << 9,
    POCKET_FX_PAIR_REJECT    = 1u << 10,
    POCKET_FX_DELETE_BONDS   = 1u << 11,
    POCKET_FX_SAVE_SETTINGS  = 1u << 12,
    POCKET_FX_BACKLIGHT      = 1u << 13,
    POCKET_FX_REDRAW         = 1u << 14,  // view changed
    POCKET_FX_SCREEN         = 1u << 15,  // screen_on changed
    // The text of the message just handled becomes the shown reply or
    // transcript: the platform copies it into the screen's buffer. The
    // model keeps no copy of either text.
    POCKET_FX_REPLY_TEXT     = 1u << 16,
    POCKET_FX_TRANSCRIPT_TEXT = 1u << 17,
    // Stored replies (flash history, platform-owned).
    POCKET_FX_HIST_SAVE      = 1u << 18,  // the shown reply became final: store it
    POCKET_FX_HIST_PREV      = 1u << 19,  // show the stored reply before the shown one
    POCKET_FX_HIST_NEXT      = 1u << 20,  // show the stored reply after the shown one
    POCKET_FX_TONE           = 1u << 21,  // play the new-reply tone
    POCKET_FX_DEEP_SLEEP     = 1u << 22,  // no event for the deep-sleep delay: power down
    POCKET_FX_LINK_IDLE      = 1u << 23,  // link_idle changed: request link parameters
};

typedef struct {
    uint32_t flags;
    uint16_t press_id;          // STREAM, FINISH, KEEPALIVE
    bool capture_wanted;        // CAPTURE: mic should run
    uint8_t backlight_pct;      // BACKLIGHT
    bool screen_on;             // SCREEN: light (true) or blank (false) the screen
    bool link_idle;             // LINK_IDLE: low-power (true) or responsive link parameters
} pocket_fx_t;

// User settings, persisted by the platform. Indices select from the level
// tables in pocket_config.h; out-of-range values fall back to the defaults.
typedef struct {
    bool preroll_on;
    uint8_t brightness_index;
    uint8_t screen_off_index;
    uint8_t deep_sleep_index;
    bool alert_screen;          // a new reply lights the screen
    bool alert_tone;            // a new reply plays a short tone
    uint8_t lang;               // UI language (pocket_lang_t), set by the phone
} pocket_settings_t;

typedef struct {
    // Press gesture.
    enum { PRESS_IDLE = 0, PRESS_ARMED, PRESS_STREAMING, PRESS_REFUSED } press;
    uint32_t press_down_ms;
    uint16_t next_press_id;
    uint16_t press_id;          // current or last confirmed press
    bool has_press;             // press_id is valid
    bool ok_down;               // OK physically held (any screen)
    uint32_t ok_down_ms;

    // Reply wait: from release until REPLY_DONE, a failed RESULT, the next
    // confirmed press or link loss; no time cap.
    bool waiting;
    uint16_t wait_press_id;
    uint32_t next_keepalive_ms;
    // Brain work (WORK messages) while waiting. work_phase is the shown
    // label (RECEIVED, THINKING or TOOL); work_active is false after IDLE.
    uint8_t work_phase;
    bool work_active;

    // Link and phone app.
    pocket_link_state_t link;
    bool server_unreachable;    // APP_STATE 1
    bool proto_mismatch;        // APP_STATE 2; cleared by APP_STATE 0/1 or link loss
    bool refused_flash;         // OK pressed while the link was down

    // Pairing.
    uint32_t passkey;           // numeric comparison value

    // Latest reply. Its text lives in the screen's buffer (POCKET_FX_REPLY_TEXT).
    bool has_reply;
    uint32_t reply_id;
    bool reply_final;
    // Test hooks: written by the model, read only by the host tests.
    bool reply_done;
    uint32_t reply_version;     // bumps on every text change

    // Position of the shown reply in the stored history: 1 = newest, 0 = not
    // stored (still streaming). UP at the top and DOWN at the bottom of a
    // stored reply step to its neighbours.
    uint16_t hist_pos;
    uint16_t hist_count;
    // A final reply that arrived during a press is stored when the press ends:
    // flash erase and write stall the CPU and would cost press audio.
    bool hist_save_pending;

    // Scroll of the reply view, in pixels, with metrics from the UI.
    bool scroll_to_end;         // the next metrics put the view at the end
    int32_t scroll_px;
    int32_t content_h;
    int32_t view_h;
    int32_t line_h;

    // Transcript of the last press (RESULT code 0); the text lives in the
    // screen's buffer (POCKET_FX_TRANSCRIPT_TEXT).
    uint32_t transcript_version;  // test hook: read only by the host tests

    pocket_notice_t notice;

    // Settings.
    bool preroll_on;
    uint8_t brightness_index;
    uint8_t screen_off_index;   // POCKET_SCREEN_OFF_LEVELS_S
    uint8_t deep_sleep_index;   // POCKET_DEEP_SLEEP_LEVELS_S
    bool alert_screen;
    bool alert_tone;
    uint8_t lang;               // pocket_lang_t; only the UI's own copy follows it
    uint8_t settings_sel;
    pocket_screen_t settings_return;

    // Battery as last sent.
    uint8_t battery_pct;
    uint8_t charging;

    // Standby: the screen goes dark after the screen-off delay without a
    // button or new content, unless something holds it on (pocket_model.c).
    bool screen_on;
    uint32_t screen_touch_ms;   // last activity
    uint8_t swallow;            // bit per button: the rest of this press only woke the screen
    bool usb_host;              // a USB host is connected: the screen stays on

    // Idle power. An event is a button edge, a REPLY, WORK or RESULT from
    // the phone, or a USB host plugged or unplugged; deep sleep follows the
    // deep-sleep delay without one, unless a press, a reply wait, the
    // pairing code or a USB host holds the device awake.
    uint32_t event_ms;
    bool deep_sleep_sent;       // DEEP_SLEEP requested; nothing follows
    uint32_t deep_sleep_test_s; // debug: delay override, also on a USB host (0 = off)
    // Woken from deep sleep by a key that is still down: the rest of that
    // press is ignored (it only wakes the device; it never records).
    bool boot_hold;
    // Link parameters: low power while dark and quiet (no press, no wait).
    bool link_idle;

    pocket_screen_t screen;
    uint32_t recording_started_ms;
    uint32_t now_ms;
} pocket_model_t;

void pocket_model_init(pocket_model_t *m, uint16_t first_press_id, const pocket_settings_t *s);
// Current settings, for the platform to persist after POCKET_FX_SAVE_SETTINGS.
void pocket_model_settings(const pocket_model_t *m, pocket_settings_t *out);
// Deep-sleep delay in seconds; 0 = never.
uint32_t pocket_model_deep_sleep_s(const pocket_model_t *m);
// Booted by a key that is still down (held) or was released (not held).
void pocket_model_boot_hold(pocket_model_t *m, bool held);

// The mic should run: during a press, and continuously when pre-roll is on,
// the phone is linked and the screen is on.
bool pocket_model_capture_wanted(const pocket_model_t *m);
uint8_t pocket_model_backlight_pct(const pocket_model_t *m);
// Screen-off delay in seconds; 0 = never.
uint32_t pocket_model_screen_off_s(const pocket_model_t *m);

// Milliseconds until pocket_model_tick has work (press timing, keepalive,
// screen-off), or UINT32_MAX when nothing is timed. Events in between are
// handled by the other entry points; recompute after each of them.
uint32_t pocket_model_tick_wait_ms(const pocket_model_t *m, uint32_t now_ms);

void pocket_model_link(pocket_model_t *m, const pocket_link_state_t *link, uint32_t now_ms,
                       pocket_fx_t *fx);
void pocket_model_pair_request(pocket_model_t *m, uint32_t passkey, uint32_t now_ms,
                               pocket_fx_t *fx);
void pocket_model_pair_finished(pocket_model_t *m, pocket_fx_t *fx);
void pocket_model_button(pocket_model_t *m, pocket_btn_t btn, pocket_btn_ev_t ev,
                         uint32_t now_ms, pocket_fx_t *fx);
void pocket_model_tick(pocket_model_t *m, uint32_t now_ms, pocket_fx_t *fx);
// stored: for a REPLY, its reply_id is already in the stored history (an
// answer sent again); it is shown but neither stored again nor announced.
void pocket_model_downlink(pocket_model_t *m, const pocket_downlink_t *msg, bool stored,
                           uint32_t now_ms, pocket_fx_t *fx);
// A stored reply is now the shown one (at boot, or after HIST_PREV/NEXT):
// the platform has put its text into the screen's buffer. at_end shows its
// end (stepping back from the top of the next reply).
void pocket_model_show_stored(pocket_model_t *m, uint32_t reply_id, uint16_t pos, uint16_t count,
                              bool at_end, pocket_fx_t *fx);
// Position of the shown reply after the history changed (stored, evicted).
void pocket_model_hist_position(pocket_model_t *m, uint16_t pos, uint16_t count, pocket_fx_t *fx);
void pocket_model_battery(pocket_model_t *m, uint8_t pct, uint8_t charging, pocket_fx_t *fx);
// A USB host (not a charger) connected or went away. While connected the
// screen is held on; on disconnect the screen-off delay starts again.
void pocket_model_usb_host(pocket_model_t *m, bool connected, uint32_t now_ms, pocket_fx_t *fx);

// Reply view metrics from the UI after layout; clamps the scroll offset.
void pocket_model_reply_metrics(pocket_model_t *m, int32_t content_h, int32_t view_h,
                                int32_t line_h);
// Largest scroll offset for the current metrics.
int32_t pocket_model_scroll_max(const pocket_model_t *m);
