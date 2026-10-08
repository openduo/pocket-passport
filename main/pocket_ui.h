// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// LVGL screens of the pocket application (layout: docs/pocket/mockups). All
// functions must be called with the LVGL lock held (bsp_lvgl_lock), from the
// application task.
#pragma once

#include "esp_err.h"
#include "pocket_model.h"

#include <stdbool.h>
#include <stdint.h>

esp_err_t pocket_ui_init(void);

// Brings the screen in line with the model. Feeds the reply view metrics
// back into the model (pocket_model_reply_metrics) and applies the clamped
// scroll offset, so the model must only be touched by the calling task.
void pocket_ui_render(pocket_model_t *m, const char *device_name);

// Sanitizes a reply's raw UTF-8 into the reply view's buffer, the only RAM
// copy of the shown reply; truncated adds the "full text on the phone" line.
void pocket_ui_set_reply(const uint8_t *text, size_t len, bool truncated);
// Loading a stored (already sanitized) reply: fill the buffer, up to cap
// bytes, then report its length and whether it was cut.
char *pocket_ui_reply_buffer(size_t *cap);
void pocket_ui_reply_loaded(size_t len, bool cut);
// The shown reply as sanitized text (without that line) and whether it was cut.
const char *pocket_ui_reply_text(size_t *len, bool *cut);
// Sanitizes the transcript into the working screen's buffer.
void pocket_ui_set_transcript(const uint8_t *text, size_t len);

// True if the CJK UI font can draw the code point (placeholders excluded).
bool pocket_ui_has_glyph(uint32_t codepoint);

// Coverage self-test of the UI copy (pocket_strings.h) against the font.
// Logs every missing code point as U+XXXX; returns the number missing.
int pocket_ui_check_strings(void);
