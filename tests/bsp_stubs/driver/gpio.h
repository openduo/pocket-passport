// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0
#pragma once
#include "esp_err.h"
typedef int gpio_num_t;
typedef enum { GPIO_MODE_INPUT = 1 } gpio_mode_t;
typedef enum { GPIO_FLOATING = 3 } gpio_pull_mode_t;
typedef enum { GPIO_INTR_LOW_LEVEL = 4 } gpio_int_type_t;
typedef void (*gpio_isr_t)(void *);
esp_err_t gpio_set_direction(gpio_num_t, gpio_mode_t);
esp_err_t gpio_set_pull_mode(gpio_num_t, gpio_pull_mode_t);
esp_err_t gpio_set_intr_type(gpio_num_t, gpio_int_type_t);
esp_err_t gpio_install_isr_service(int);
esp_err_t gpio_isr_handler_add(gpio_num_t, gpio_isr_t, void *);
esp_err_t gpio_isr_handler_remove(gpio_num_t);
esp_err_t gpio_intr_enable(gpio_num_t);
esp_err_t gpio_intr_disable(gpio_num_t);
esp_err_t gpio_wakeup_enable(gpio_num_t, gpio_int_type_t);
esp_err_t gpio_wakeup_disable(gpio_num_t);
