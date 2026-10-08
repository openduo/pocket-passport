// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Pocket v1 firmware entry: DuoDuo Pocket push-to-talk accessory.
// Hold OK to talk; the phone app relays the voice note and shows replies here.
#include "pocket_app.h"
#include "pocket_stats.h"

#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "pocket";

void app_main(void)
{
    pocket_stats_heap_mark("boot");
    // Bonds, settings and PHY calibration live in NVS. Never erase it on an
    // init error: that would silently drop the pairing.
    esp_err_t e = nvs_flash_init();
    if (e != ESP_OK) ESP_LOGE(TAG, "nvs_flash_init failed: %s (not erased)", esp_err_to_name(e));

    pocket_stats_heap_mark("nvs");
    bsp_i2c_init();
    pocket_stats_heap_mark("i2c");
    const bool display_ok = bsp_display_init() == ESP_OK;
    pocket_stats_heap_mark("display");
    if (!display_ok || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed; stopping");
        return;
    }
    pocket_stats_heap_mark("display_lvgl");

    e = pocket_app_start();
    if (e != ESP_OK) ESP_LOGE(TAG, "pocket start failed: %s", esp_err_to_name(e));
    pocket_stats_heap_mark("app");
    if (pocket_stats_start() != ESP_OK) ESP_LOGE(TAG, "stats task not started");
    pocket_stats_heap_mark("stats_task");
}
