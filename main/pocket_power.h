// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Standby platform layer: ESP-IDF power management (frequency scaling and
// automatic light sleep while the BLE link stays up), the screen on/off
// sequence with its power-management lock, and an LVGL clock that needs no
// periodic timer. The model decides when; this layer only acts. Button wake
// is bsp_button_enable_sleep_wake().
#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

// After bsp_lvgl_init(), before the BLE stack starts.
esp_err_t pocket_power_init(void);

// Lights the screen (lock, panel wake, backlight) or blanks it (backlight
// off, panel sleep, unlock). Blocks ~100 ms for the panel. Application task;
// takes the LVGL lock itself.
void pocket_power_screen(bool on, uint8_t backlight_pct);

// Holds the screen's power lock without waking the panel; for the start of a
// model step that lights the screen, so audio wakes at full clock speed.
void pocket_power_screen_lock(void);

// Deep sleep (no return): stops the codec, fuel gauge, buses and panel in
// the order the BSP requires, arms the key-ladder wake and powers down. The
// BLE link drops; a key press boots the firmware again. timer_wake_s > 0
// also wakes after that many seconds (debug test images only).
void pocket_power_deep_sleep(uint32_t timer_wake_s);

// True if a key woke the chip from deep sleep (this boot).
bool pocket_power_woke_by_key(void);
// Logs how the chip woke: deep-sleep count and time asleep, kept in RTC
// memory. Call once the console is up.
void pocket_power_log_wake(void);
