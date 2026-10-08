// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Debug statistics (CONFIG_POCKET_DEBUG_STATS); no-ops when disabled.
#pragma once

#include "esp_err.h"

// Logs internal heap free/min/largest after an init stage.
void pocket_stats_heap_mark(const char *stage);
esp_err_t pocket_stats_start(void);

// Standby notes. The USB console stops while the chip is in light sleep, so
// what happens in the dark is summed here and logged as one "standby" line
// once the screen has been lit long enough for the console to return.
#include <stdbool.h>
#include <stdint.h>
void pocket_stats_note_screen(bool on);
void pocket_stats_note_link(bool ready);
// A key went down: screen dark before it, wake interrupt to key event (-1 if
// the button poll was running).
void pocket_stats_note_key(bool dark, int64_t wake_isr_to_event_us);
