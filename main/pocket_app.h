// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Pocket application: settings, UI, audio, BLE, buttons and the app task.
// Call once after the display, LVGL and NVS are initialised.
#pragma once

#include "esp_err.h"

esp_err_t pocket_app_start(void);

#include "sdkconfig.h"
#if CONFIG_POCKET_DEBUG_STATS
#include <stdbool.h>
#include <stdint.h>
// Debug statistics: the most events that have waited in the app queue.
uint32_t pocket_app_debug_queue_peak(void);
void pocket_app_debug_queue_peak_reset(void);
// Records enqueued events while on; turning it off logs them (kind, message
// type, queue length after the enqueue).
void pocket_app_debug_queue_trace(bool on);
#endif

#if CONFIG_POCKET_DEBUG_FLOWPROBE
// Debug flow probe: enters a reply wait as a released press would (no audio),
// and returns its press id once the app task has done it (0 while pending).
void pocket_app_debug_begin_wait(void);
uint16_t pocket_app_debug_wait_press_id(void);
// Screen the model shows (pocket_screen_t).
int pocket_app_debug_screen(void);
// UI language the model holds (pocket_lang_t).
int pocket_app_debug_lang(void);
#endif

#if CONFIG_POCKET_DEBUG_MEMPROBE
// Debug memory probe: posts a key event (pocket_btn_t, pocket_btn_ev_t) to the
// app task as if the button driver had reported it.
void pocket_app_debug_key(int btn, int ev);
#endif
