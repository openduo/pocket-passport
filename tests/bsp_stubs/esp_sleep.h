// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0
#pragma once
#include "esp_err.h"
#include <stdint.h>
esp_err_t esp_sleep_enable_gpio_wakeup(void);
typedef enum { ESP_GPIO_WAKEUP_GPIO_LOW = 0, ESP_GPIO_WAKEUP_GPIO_HIGH = 1 } esp_deepsleep_gpio_wake_up_mode_t;
esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t gpio_pin_mask, esp_deepsleep_gpio_wake_up_mode_t mode);
