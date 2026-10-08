// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Capture pipeline: mic task (PCM I/O) -> encoder task (Opus; format in
// docs/pocket/README.md, "Audio") -> pre-roll/hold ring or the BLE link. The
// encoder task owns the press stream and emits PRESS_START, AUDIO and
// PRESS_END itself, so the three stay in order on the link. Control calls
// are non-blocking.
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t frames;           // PCM frames read
    uint32_t rx_overflow;      // I2S RX overflows (mic data lost)
    uint32_t mic_errors;
    uint32_t drop_enc_in;      // PCM frames dropped: encoder queue full
    uint32_t enc_errors;
    uint32_t enc_count;        // window: frames encoded
    uint32_t enc_us_sum;       // window
    uint32_t enc_us_max;       // window
    uint32_t enc_bytes;        // window
    uint32_t sent_packets;     // AUDIO messages accepted by the link
    uint32_t lost_packets;     // AUDIO messages the link refused
    uint32_t ring_evicted;     // pre-roll/hold packets evicted for space
    uint32_t wake_us_max;      // longest codec wake
    uint32_t last_wake_us;     // last codec wake
    uint32_t press_to_frame_us; // last press: button event to the first PCM frame read
    uint32_t press_frames;     // presses measured by press_to_frame_us
    uint32_t tones;            // new-reply tones played out
    uint32_t tone_errors;      // tone frames the codec refused
} pocket_audio_stats_t;

esp_err_t pocket_audio_start(void);

// Mic on/off. Off suspends the ES8311 and I2S (bsp_audio_sleep).
void pocket_audio_set_capture(bool on);
// Keep the last CONFIG_POCKET_PREROLL_MS of audio while idle.
void pocket_audio_set_preroll(bool on);

void pocket_audio_arm(void);                  // press down: hold audio
void pocket_audio_stream(uint16_t press_id);  // confirmed: start, flush, stream
void pocket_audio_finish(void);               // release: end the press
void pocket_audio_discard(void);              // tap: drop held audio
void pocket_audio_abort(void);                // link lost: drop, send nothing
// Diagnostic: esp_timer time of the button event that armed a press; the mic
// task measures from it to the first PCM frame (press_to_frame_us).
void pocket_audio_mark_press(int64_t press_us);

// Plays the new-reply tone through the ES8311 speaker output (wakes the
// codec for ~0.3 s if it sleeps). Non-blocking.
void pocket_audio_tone(void);

// Before deep sleep: stops capture and the tone and waits for the mic task to
// put the codec to sleep.
void pocket_audio_shutdown(void);

uint16_t pocket_audio_level(void);            // last mic frame peak, 0..32767

// Copies counters; window fields are reset when reset_window is true.
void pocket_audio_stats(pocket_audio_stats_t *out, bool reset_window);
