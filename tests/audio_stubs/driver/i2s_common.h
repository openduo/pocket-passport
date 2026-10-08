// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0
#pragma once
// Host stub for the diagnostic I2S callback hook in bsp_audio.h.
#include "esp_err.h"
#include <stdbool.h>
typedef struct test_channel *i2s_chan_handle_t;
typedef struct { int unused; } i2s_event_data_t;
typedef bool (*i2s_isr_callback_t)(i2s_chan_handle_t, i2s_event_data_t *, void *);
typedef struct {
    i2s_isr_callback_t on_recv, on_recv_q_ovf, on_sent, on_send_q_ovf;
} i2s_event_callbacks_t;
static inline esp_err_t i2s_channel_register_event_callback(
    i2s_chan_handle_t handle, const i2s_event_callbacks_t *callbacks, void *user) {
    (void)handle; (void)callbacks; (void)user;
    return ESP_OK;
}
