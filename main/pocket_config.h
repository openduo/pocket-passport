// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Named constants of the pocket firmware that are not build-time tunables.
// Every value states its basis; "pending data" marks a value to revisit when
// the named measurement exists. Runtime tunables live in Kconfig.projbuild.
#pragma once

// --- Audio format (docs/pocket/README.md, "Audio") --------------------------
// Opus 16 kHz mono, 20 ms frames, complexity 0, VBR, DTX off, FEC off.
#define POCKET_SAMPLE_RATE_HZ   16000
#define POCKET_FRAME_MS         20
#define POCKET_FRAME_SAMPLES    (POCKET_SAMPLE_RATE_HZ / 1000 * POCKET_FRAME_MS)
#define POCKET_FRAME_BYTES      (POCKET_FRAME_SAMPLES * 2)
#define POCKET_OPUS_COMPLEXITY  0

// Largest Opus packet for one frame: RFC 6716 §3.4 (1275 bytes per frame).
#define POCKET_OPUS_MAX_PACKET  1275

// --- Press gesture ----------------------------------------------------------
// A press shorter than this is a tap and is ignored (docs/pocket/README.md,
// "Controls and screens"). Precedent: an earlier push-to-talk design; tune
// after trials.
#define POCKET_TAP_THRESHOLD_MS 300

// --- Link -------------------------------------------------------------------
// Largest reassembled phone -> device message. Basis: a REPLY of ~1365 CJK
// characters (3 B each), ~35 screens of 20 px text; the pocket-room prompt
// asks for short answers and the phone keeps the full text. Longer messages
// are cut at a character boundary and shown with an ellipsis. The phone app
// uses the same limit (docs/pocket/README.md, "BLE link").
#define POCKET_MAX_MSG_BYTES    4096

// Kept transcript bytes for the "you said" line. Basis: >= 5 screens of
// 20 px CJK text; the full transcript is on the phone.
#define POCKET_TRANSCRIPT_MAX_BYTES 1024

// An unencrypted connection that has not become secure in this time is
// dropped so a stranger cannot hold the single connection slot. Basis:
// Bluetooth Core SMP timeout (30 s), the longest a pairing may legally take.
#define POCKET_SECURE_TIMEOUT_MS 30000

// --- Reply history ---------------------------------------------------------
// Text bytes of stored replies; the oldest are evicted beyond it. User
// decision: 16 KB in flash, at least four full replies
// of POCKET_MAX_MSG_BYTES (4 x 4,091 text bytes = 16,364).
#define POCKET_HIST_BUDGET_BYTES 16384
// Flash partition of the history (partitions.csv): data subtype 0x40, the
// first of the range ESP-IDF leaves to applications (0x40-0xFE).
#define POCKET_HIST_PARTITION_SUBTYPE 0x40
#define POCKET_HIST_PARTITION_LABEL "history"

// --- Reply wait ---------------------------------------------------------------
// KEEPALIVE period while a reply is pending. Basis: iOS gives a
// bluetooth-central app ~10 s of runtime per notification (Apple
// CoreBluetooth background guide); 5 s keeps the app awake with 2x margin.
// Pending data: a measurement of the actual iOS wake window.
#define POCKET_KEEPALIVE_INTERVAL_MS 5000

// There is no keepalive cap: keepalives run until REPLY_DONE, a failed
// RESULT, the next press or link loss (docs/pocket/README.md, "BLE link").
// Replies taking 168 s have been observed.

// Working-state animation step: the three dots after the phase label light
// one at a time. Aesthetic; each step redraws three 6 px dots only.
#define POCKET_WORK_ANIM_STEP_MS 400

// --- Battery ------------------------------------------------------------------
// Fuel-gauge poll period. Basis: SOC moves ~1 % per several minutes on the
// 520 mAh cell; 30 s makes a STATUS change visible quickly at negligible I2C
// cost.
#define POCKET_BATTERY_POLL_MS  30000

// --- USB host -----------------------------------------------------------------
// How often the app task reads the USB host state (usb_serial_jtag_is_connected)
// while the screen is lit or a host is connected. Basis: one second is well
// inside the shortest screen-off delay (15 s), so plugging or unplugging takes
// effect before any countdown matters; while lit the screen lock already keeps
// the CPU awake, so the poll adds no sleep cost. While dark and unplugged there
// is no poll: the next wake (key, phone message, battery poll) reads the state.
#define POCKET_USB_POLL_MS 1000

// --- UI -----------------------------------------------------------------------
// Model tick for gesture timing, the recording clock and keepalives. Basis:
// resolves the 300 ms tap threshold to within 1/6 and refreshes the clock
// faster than a visible second.
#define POCKET_TICK_MS          50

// Backlight levels offered in settings (percent) and the default index.
// Aesthetic preference; default pending power data.
#define POCKET_BRIGHTNESS_LEVELS { 30, 60, 100 }
#define POCKET_BRIGHTNESS_COUNT  3
#define POCKET_BRIGHTNESS_DEFAULT_INDEX 1

// Screen-off delays offered in settings, in seconds; 0 means never. User
// decision: 15/30/60 s and "never", default 30 s. Inactivity is no button and
// no new on-screen content; recording, a pending reply, the pairing code and
// the settings menu keep the screen on.
#define POCKET_SCREEN_OFF_LEVELS_S { 15, 30, 60, 0 }
#define POCKET_SCREEN_OFF_COUNT    4
#define POCKET_SCREEN_OFF_DEFAULT_INDEX 1

// Deep-sleep delays offered in settings, in seconds; 0 means never. User
// decision: 30 min / 1 h / 4 h / never, default 1 h. Measured from the last
// event (pocket_model_t.event_ms).
#define POCKET_DEEP_SLEEP_LEVELS_S { 1800, 3600, 14400, 0 }
#define POCKET_DEEP_SLEEP_COUNT    4
#define POCKET_DEEP_SLEEP_DEFAULT_INDEX 1

// New-reply alerts (screen on, tone): user decision, both on by default.
#define POCKET_ALERT_SCREEN_DEFAULT true
#define POCKET_ALERT_TONE_DEFAULT   true

// UI language until the phone sends one (APP_STATE, protocol 1.2): the
// product's first language. Values: pocket_lang_t.
#define POCKET_LANG_DEFAULT 0  // POCKET_LANG_ZH_HANS
