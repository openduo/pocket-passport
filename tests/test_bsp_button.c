// Execute the real BSP with public-interface stubs and a fake ADC (no SDK needed).
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "../components/bsp/src/bsp_button.c"

struct button_dev_t { button_driver_t *driver; bool live; };
static struct button_dev_t buttons[BSP_BTN_COUNT];
static int adc_token, cal_token, adc_live, cal_live, live_buttons;
static int create_calls, callback_calls, fail_create, fail_callback;
static int fail_adc, fail_channel, fail_cal, fail_read, fail_convert, fail_delete;
static int raw_mv, reads, events;
static int64_t clock_us;

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg, adc_oneshot_unit_handle_t *h) {
    assert(cfg->unit_id == BSP_BTN_ADC_UNIT && !adc_live);
    if (fail_adc) return ESP_ERR_NO_MEM;
    adc_live = 1; *h = &adc_token; return ESP_OK;
}
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t h) {
    assert(h == &adc_token && adc_live && !live_buttons);
    adc_live = 0; return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, int channel, const adc_oneshot_chan_cfg_t *cfg) {
    assert(h == &adc_token && channel == BSP_BTN_ADC_CHANNEL && cfg->atten == ADC_ATTEN_DB_12);
    return fail_channel ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, int channel, int *raw) {
    assert(h == &adc_token && adc_live && channel == BSP_BTN_ADC_CHANNEL);
    ++reads;
    if (fail_read) return ESP_FAIL;
    *raw = raw_mv; return ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *cfg, adc_cali_handle_t *h) {
    assert(adc_live && !cal_live && cfg->atten == ADC_ATTEN_DB_12);
    if (fail_cal) return ESP_ERR_NO_MEM;
    cal_live = 1; *h = &cal_token; return ESP_OK;
}
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t h) {
    assert(h == &cal_token && cal_live && !live_buttons);
    cal_live = 0; return ESP_OK;
}
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *mv) {
    assert(h == &cal_token && cal_live);
    if (fail_convert) { *mv = 0; return ESP_FAIL; }
    *mv = raw; return ESP_OK;
}
int64_t esp_timer_get_time(void) { return clock_us; }
// GPIO wake stubs: one pad, its interrupt, wake enable and ISR.
static int pad_input, intr_on, wake_on, isr_added, sleep_gpio_wake, wakeup_isr_calls;
static gpio_isr_t pad_isr;
esp_err_t gpio_set_direction(gpio_num_t n, gpio_mode_t m) { assert(n == BSP_BTN_GPIO && m == GPIO_MODE_INPUT); pad_input = 1; return ESP_OK; }
esp_err_t gpio_set_pull_mode(gpio_num_t n, gpio_pull_mode_t p) { assert(n == BSP_BTN_GPIO && p == GPIO_FLOATING); return ESP_OK; }
esp_err_t gpio_set_intr_type(gpio_num_t n, gpio_int_type_t t) { assert(n == BSP_BTN_GPIO && t == GPIO_INTR_LOW_LEVEL); return ESP_OK; }
esp_err_t gpio_install_isr_service(int f) { (void)f; return ESP_ERR_INVALID_STATE; }
esp_err_t gpio_isr_handler_add(gpio_num_t n, gpio_isr_t isr, void *a) { (void)a; assert(n == BSP_BTN_GPIO && pad_input); pad_isr = isr; isr_added = 1; intr_on = 1; return ESP_OK; }
esp_err_t gpio_isr_handler_remove(gpio_num_t n) { assert(n == BSP_BTN_GPIO); isr_added = 0; pad_isr = NULL; return ESP_OK; }
esp_err_t gpio_intr_enable(gpio_num_t n) { assert(n == BSP_BTN_GPIO && isr_added); intr_on = 1; return ESP_OK; }
esp_err_t gpio_intr_disable(gpio_num_t n) { assert(n == BSP_BTN_GPIO); intr_on = 0; return ESP_OK; }
esp_err_t gpio_wakeup_enable(gpio_num_t n, gpio_int_type_t t) { assert(n == BSP_BTN_GPIO && t == GPIO_INTR_LOW_LEVEL); wake_on = 1; return ESP_OK; }
esp_err_t gpio_wakeup_disable(gpio_num_t n) { assert(n == BSP_BTN_GPIO); wake_on = 0; return ESP_OK; }
esp_err_t esp_sleep_enable_gpio_wakeup(void) { sleep_gpio_wake = 1; return ESP_OK; }
esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t mask, esp_deepsleep_gpio_wake_up_mode_t mode) {
    assert(mask == (1ULL << BSP_BTN_GPIO) && mode == ESP_GPIO_WAKEUP_GPIO_LOW);
    return ESP_OK;
}
void iot_button_power_save_wakeup_isr(uint32_t gpio) { assert(gpio == BSP_BTN_GPIO); ++wakeup_isr_calls; intr_on = 0; }
esp_err_t iot_button_create(const button_config_t *cfg, const button_driver_t *driver, button_handle_t *h) {
    // 判定门限必须由 BSP 显式下发(bsp_pins.h),不能退回组件默认的 180 / 1500ms。
    assert(cfg->short_press_time == BSP_BTN_SHORT_PRESS_MS);
    assert(cfg->long_press_time == BSP_BTN_LONG_PRESS_MS);
    if (++create_calls == fail_create) return ESP_ERR_NO_MEM;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        if (buttons[i].live) continue;
        buttons[i] = (struct button_dev_t){ .driver = (button_driver_t *)driver, .live = true };
        *h = &buttons[i]; ++live_buttons;
        assert(driver->get_key_level((button_driver_t *)driver) == BUTTON_INACTIVE);
        return ESP_OK;
    }
    assert(false); return ESP_FAIL;
}
esp_err_t iot_button_delete(button_handle_t h) {
    assert(h && h->live && adc_live && cal_live);
    if (fail_delete) return ESP_FAIL;
    assert(h->driver->del(h->driver) == ESP_OK);
    h->live = false; --live_buttons; return ESP_OK;
}
esp_err_t iot_button_register_cb(button_handle_t h, button_event_t ev, button_event_args_t *args, button_cb_t cb, void *u) {
    (void)args; (void)ev;
    assert(h->live);
    cb(h, u); // No user callbacks may escape a partial initialization.
    return ++callback_calls == fail_callback ? ESP_ERR_NO_MEM : ESP_OK;
}
static void event_cb(bsp_btn_t btn, bsp_btn_ev_t ev, void *u) {
    assert(btn == BSP_BTN_OK && ev == BSP_BTN_CLICK && u == &events);
    ++events;
}
static void reset_faults(void) {
    fail_adc = fail_channel = fail_cal = fail_read = fail_convert = fail_delete = 0;
    fail_create = fail_callback = create_calls = callback_calls = 0;
}
static void assert_clean(void) {
    assert(!adc_live && !cal_live && !live_buttons && !s_ready && !s_adc && !s_cali);
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!s_btn[i]);
}
static void retry_success(void) {
    assert_clean(); reset_faults();
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    assert(create_calls == BSP_BTN_COUNT && live_buttons == BSP_BTN_COUNT);
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    assert(create_calls == BSP_BTN_COUNT);
    button_cleanup(); assert_clean();
}
static void check_voltage(int mv, int expected) {
    raw_mv = mv; clock_us += 2000;
    const int before = reads;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        assert(s_drivers[i].base.get_key_level(&s_drivers[i].base) == (i == expected));
    }
    assert(reads - before == CONFIG_ADC_BUTTON_SAMPLE_TIMES);
}
static void check_sleep_wake(void) {
    assert(bsp_button_enable_sleep_wake() == ESP_ERR_INVALID_STATE);  // before init
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    assert(bsp_button_enable_sleep_wake() == ESP_OK);
    assert(pad_input && isr_added && !intr_on && sleep_gpio_wake && !wake_on);
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        button_driver_t *d = &s_drivers[i].base;
        assert(d->enable_power_save && d->get_gpio_num(d) == BSP_BTN_GPIO);
    }
    assert(bsp_button_enable_sleep_wake() == ESP_OK);  // idempotent
    // Idle poll: each of the three drivers is asked to enter; armed once.
    for (int i = 0; i < BSP_BTN_COUNT; ++i) {
        assert(s_drivers[i].base.enter_power_save(&s_drivers[i].base) == ESP_OK);
    }
    assert(intr_on && wake_on);
    // A key pulls the pad low: the ISR stamps the time and resumes polling.
    clock_us = 123456;
    pad_isr(NULL);
    assert(wakeup_isr_calls == 1 && !intr_on && bsp_button_last_wake_us() == 123456);
    assert(s_drivers[0].base.exit_power_save(&s_drivers[0].base) == ESP_OK);
    assert(!wake_on && !intr_on);
    // Polling decodes the key from the ADC as before.
    check_voltage(595, BSP_BTN_OK);
    assert(s_drivers[1].base.enter_power_save(&s_drivers[1].base) == ESP_OK);
    assert(intr_on && wake_on);
    // Teardown removes the interrupt; a fresh init starts without wake.
    button_cleanup();
    assert(!isr_added && !intr_on && !wake_on);
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    assert(!s_drivers[0].base.enable_power_save);
    button_cleanup();
}
int main(void) {
    for (int i = 1; i <= BSP_BTN_COUNT; ++i) {
        reset_faults(); fail_create = i;
        assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    }
    for (int i = 1; i <= BSP_BTN_COUNT * 5; ++i) {
        reset_faults(); fail_callback = i;
        assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    }
    reset_faults(); fail_adc = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    fail_channel = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    fail_cal = 1;
    assert(bsp_button_init(event_cb, &events) != ESP_OK); retry_success();
    assert(events == 0);
    assert(bsp_button_init(event_cb, &events) == ESP_OK);
    cb_click(NULL, (void *)(intptr_t)BSP_BTN_OK); assert(events == 1);
    check_voltage(0, BSP_BTN_UP); check_voltage(149, BSP_BTN_UP);
    check_voltage(150, BSP_BTN_DOWN); check_voltage(446, BSP_BTN_DOWN);
    check_voltage(447, BSP_BTN_OK); check_voltage(1899, BSP_BTN_OK);
    check_voltage(1900, -1); check_voltage(3300, -1);
    assert(bsp_button_read_mv() == 3300);
    assert(!bsp_button_any_down());
    raw_mv = 595; assert(bsp_button_any_down());
    raw_mv = 0; assert(bsp_button_any_down());
    raw_mv = 1900; assert(!bsp_button_any_down());
    raw_mv = 3300;
    assert(bsp_button_enable_deep_sleep_wake() == ESP_OK);
    fail_read = 1; clock_us += 2000;
    assert(!bsp_button_any_down());
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!button_level(&s_drivers[i].base));
    assert(bsp_button_read_mv() == -1);
    fail_read = 0; fail_convert = 1; clock_us += 2000;
    for (int i = 0; i < BSP_BTN_COUNT; ++i) assert(!button_level(&s_drivers[i].base));
    assert(bsp_button_read_mv() == -1);
    fail_delete = 1; button_cleanup();
    assert(adc_live && cal_live && live_buttons == BSP_BTN_COUNT);
    assert(bsp_button_init(event_cb, &events) == ESP_ERR_INVALID_STATE);
    fail_delete = 0; button_cleanup(); retry_success();
    check_sleep_wake();
    puts("BSP button fault-injection tests: PASS");
}
