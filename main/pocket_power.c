// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

#include "pocket_power.h"

#include "pocket_stats.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_attr.h"
#include "esp_rtc_time.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "pocket_audio.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "sdkconfig.h"

static const char *TAG = "pocket_power";

#if CONFIG_PM_ENABLE
// Lit screen: no light sleep (the backlight LEDC and the LCD SPI run from
// clocks that stop in light sleep) and full CPU clock for LVGL rendering.
static esp_pm_lock_handle_t s_screen_lock;
#endif
static bool s_locked;

// LVGL time from esp_timer: the port's 5 ms tick timer would wake the chip
// 200 times a second; with a clock callback it can stop.
static uint32_t lvgl_clock_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

esp_err_t pocket_power_init(void)
{
    if (!bsp_lvgl_lock(-1)) return ESP_ERR_INVALID_STATE;
    lv_tick_set_cb(lvgl_clock_ms);
    // lvgl_port_stop() stops the tick timer and disables LVGL timers; turn
    // the timers back on so the task keeps rendering. An idle LVGL task then
    // sleeps up to the port's task_max_sleep_ms.
    esp_err_t e = lvgl_port_stop();
    lv_timer_enable(true);
    bsp_lvgl_unlock();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "LVGL tick timer not stopped: %s", esp_err_to_name(e));
        return e;
    }

#if CONFIG_PM_ENABLE
    // Max: the CPU clock the firmware was measured at. Min: the 40 MHz
    // crystal, the lowest clock that needs no PLL. Light sleep whenever no
    // lock is held; the BLE controller keeps the link with modem sleep.
    const esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        .light_sleep_enable = true,
    };
    e = esp_pm_configure(&pm);
    if (e == ESP_OK) e = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "pocket_screen", &s_screen_lock);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "power management not configured: %s", esp_err_to_name(e));
        return e;
    }
    // The screen is lit at boot.
    pocket_power_screen_lock();
    ESP_LOGI(TAG, "pm: %d..%d MHz, auto light sleep", pm.min_freq_mhz, pm.max_freq_mhz);
#else
    ESP_LOGW(TAG, "CONFIG_PM_ENABLE off: no light sleep");
#endif
    return ESP_OK;
}

void pocket_power_screen_lock(void)
{
    if (s_locked) return;
#if CONFIG_PM_ENABLE
    if (s_screen_lock) esp_pm_lock_acquire(s_screen_lock);
#endif
    s_locked = true;
}

static void screen_unlock(void)
{
    if (!s_locked) return;
#if CONFIG_PM_ENABLE
    if (s_screen_lock) esp_pm_lock_release(s_screen_lock);
#endif
    s_locked = false;
}

void pocket_power_screen(bool on, uint8_t backlight_pct)
{
    if (on) {
        pocket_power_screen_lock();
        if (bsp_lvgl_lock(-1)) {
            bsp_display_sleep(false);
            bsp_lvgl_unlock();
        }
        bsp_display_backlight(backlight_pct);
    } else {
        bsp_display_backlight(0);
        if (bsp_lvgl_lock(-1)) {
            bsp_display_sleep(true);
            bsp_lvgl_unlock();
        }
        screen_unlock();
    }
    pocket_stats_note_screen(on);
    ESP_LOGI(TAG, "screen %s", on ? "on" : "off");
}

// Deep-sleep record in RTC slow memory, kept through deep sleep.
#define DEEP_SLEEP_MAGIC 0x50445331u  // "PDS1"
static RTC_DATA_ATTR uint32_t s_ds_magic;
static RTC_DATA_ATTR uint32_t s_ds_count;
static RTC_DATA_ATTR uint64_t s_ds_enter_us;

static void step(const char *what, esp_err_t e)
{
    if (e != ESP_OK) ESP_LOGW(TAG, "deep sleep continues: %s failed: %s", what, esp_err_to_name(e));
}

void pocket_power_deep_sleep(uint32_t timer_wake_s)
{
    ESP_LOGI(TAG, "deep sleep #%lu", (unsigned long)(s_ds_magic == DEEP_SLEEP_MAGIC ? s_ds_count + 1 : 1));
    // The mic task owns the codec: it puts it to sleep before the BSP
    // releases the pins.
    pocket_audio_shutdown();
    // Order of the BSP deep-sleep contract (demo_low_power.c): the fuel
    // gauge and the codec share I2C, then I2S and I2C pins, then the panel.
    step("CW2017 suspend", bsp_battery_sleep());
    step("ES8311 suspend", bsp_audio_sleep());
    step("I2S pin release", bsp_audio_prepare_deep_sleep());
    step("shared I2C pin release", bsp_i2c_prepare_deep_sleep());
    // The LVGL lock is never released: no flush may follow the panel's sleep.
    // 1000 ms: the BSP's deep-sleep sequence (demo_low_power.c) uses it; the
    // longest measured hold, a full redraw of the 4 KB reply, is ~104 ms.
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "LVGL busy before deep sleep; restarting instead");
        esp_restart();
    }
    step("ST7789 suspend", bsp_display_prepare_deep_sleep());
    step("key wake", bsp_button_enable_deep_sleep_wake());
    if (timer_wake_s) step("timer wake", esp_sleep_enable_timer_wakeup((uint64_t)timer_wake_s * 1000000));
    if (s_ds_magic != DEEP_SLEEP_MAGIC) s_ds_count = 0;
    s_ds_magic = DEEP_SLEEP_MAGIC;
    s_ds_count++;
    s_ds_enter_us = esp_rtc_get_time_us();
    esp_deep_sleep_start();
    // Not reached; the peripherals cannot be resumed in this run.
    esp_restart();
}

bool pocket_power_woke_by_key(void)
{
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO;
}

void pocket_power_log_wake(void)
{
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (s_ds_magic == DEEP_SLEEP_MAGIC && cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "woke from deep sleep #%lu by %s after %.1f s", (unsigned long)s_ds_count,
                 cause == ESP_SLEEP_WAKEUP_GPIO    ? "key"
                 : cause == ESP_SLEEP_WAKEUP_TIMER ? "timer"
                                                   : "other",
                 (double)(esp_rtc_get_time_us() - s_ds_enter_us) / 1e6);
    } else {
        ESP_LOGI(TAG, "cold boot (no deep sleep before)");
    }
}
