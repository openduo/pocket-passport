// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// Debug statistics, compiled only with CONFIG_POCKET_DEBUG_STATS (debug
// images; a release image gets the no-op stubs at the end). Heap per init
// stage, then every period one "POCKET:" line (heap, encoder time, CPU,
// light-sleep residency, press latency, audio and link counters, LVGL pool)
// and every 5th period one line per task (priority, stack high-water mark,
// CPU share).
#include "pocket_stats.h"

#include "sdkconfig.h"

#if CONFIG_POCKET_DEBUG_STATS

#include "pocket_app.h"
#include "pocket_audio.h"
#include "pocket_ble.h"
#include "pocket_config.h"

#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "POCKET";

// Task table every 5th report: 10 s at the 2 s period.
#define TASK_TABLE_EVERY 5
// The standby line waits this long after the screen lights. Basis: after a
// light-sleep period macOS re-enumerated the USB Serial/JTAG port within
// ~1-2 s on this board (measured on the device); earlier lines are lost.
#define STANDBY_REPORT_DELAY_US 3000000
#define INTERNAL_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

// Init-stage marks kept for the memory probe, which replays them once the
// console is attached (boot lines are lost while the USB port re-enumerates
// after a reset). Basis: 22 call sites in pocket_*.c, with spare entries.
#define HEAP_MARKS_MAX 32
typedef struct {
    const char *stage;
    uint32_t free, min, largest;
} heap_mark_t;
static heap_mark_t s_marks[HEAP_MARKS_MAX];
static uint32_t s_mark_count;

void pocket_stats_heap_mark(const char *stage)
{
    heap_mark_t m = { stage, (uint32_t)heap_caps_get_free_size(INTERNAL_CAPS),
                      (uint32_t)heap_caps_get_minimum_free_size(INTERNAL_CAPS),
                      (uint32_t)heap_caps_get_largest_free_block(INTERNAL_CAPS) };
    if (s_mark_count < HEAP_MARKS_MAX) s_marks[s_mark_count++] = m;
    ESP_LOGI(TAG, "heap after %-14s free=%u min=%u largest=%u", stage, (unsigned)m.free,
             (unsigned)m.min, (unsigned)m.largest);
}

typedef struct {
    TaskHandle_t handle;
    configRUN_TIME_COUNTER_TYPE runtime;
} prev_rt_t;

// Light-sleep time and count, from the PM exit callback (IDLE task context).
static portMUX_TYPE s_sleep_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_sleep_us;
static uint32_t s_sleeps;

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
static IRAM_ATTR esp_err_t on_light_sleep_exit(int64_t slept_us, void *arg)
{
    (void)arg;
    portENTER_CRITICAL_SAFE(&s_sleep_lock);
    s_sleep_us += (uint64_t)slept_us;
    s_sleeps++;
    portEXIT_CRITICAL_SAFE(&s_sleep_lock);
    return ESP_OK;
}
#endif

// Standby notes (pocket_stats.h), written by the app task.
static portMUX_TYPE s_note_lock = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    bool dark;
    int64_t changed_us;         // last screen change
    int64_t dark_us;            // length of the last dark period
    uint64_t sleep_at_dark;     // light-sleep time when it began
    uint32_t sleeps_at_dark;
    uint64_t dark_sleep_us;     // light sleep during the last dark period
    uint32_t dark_sleeps;
    uint32_t link_drops;        // ready -> not ready while dark
    bool link_ready;
    bool key_seen;              // first key after the dark period
    bool key_dark;
    int64_t key_isr_us;
    bool pending;               // a dark period ended and is not reported yet
} standby_note_t;
static standby_note_t s_note;

void pocket_stats_note_screen(bool on)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_sleep_lock);
    uint64_t sleep_us = s_sleep_us;
    uint32_t sleeps = s_sleeps;
    portEXIT_CRITICAL(&s_sleep_lock);
    portENTER_CRITICAL(&s_note_lock);
    if (!on && !s_note.dark) {
        s_note.dark = true;
        s_note.sleep_at_dark = sleep_us;
        s_note.sleeps_at_dark = sleeps;
        s_note.link_drops = 0;
        s_note.key_seen = false;
        s_note.changed_us = now;
    } else if (on && s_note.dark) {
        s_note.dark = false;
        s_note.dark_us = now - s_note.changed_us;
        s_note.dark_sleep_us = sleep_us - s_note.sleep_at_dark;
        s_note.dark_sleeps = sleeps - s_note.sleeps_at_dark;
        s_note.changed_us = now;
        s_note.pending = true;
    }
    portEXIT_CRITICAL(&s_note_lock);
}

void pocket_stats_note_link(bool ready)
{
    portENTER_CRITICAL(&s_note_lock);
    if (s_note.dark && s_note.link_ready && !ready) s_note.link_drops++;
    s_note.link_ready = ready;
    portEXIT_CRITICAL(&s_note_lock);
}

void pocket_stats_note_key(bool dark, int64_t wake_isr_to_event_us)
{
    portENTER_CRITICAL(&s_note_lock);
    if (!s_note.key_seen) {
        s_note.key_seen = true;
        s_note.key_dark = dark;
        s_note.key_isr_us = wake_isr_to_event_us;
    }
    portEXIT_CRITICAL(&s_note_lock);
}

// Logs the last dark period once the console is back.
static void report_standby(const pocket_audio_stats_t *a)
{
    portENTER_CRITICAL(&s_note_lock);
    bool due = s_note.pending && !s_note.dark &&
               esp_timer_get_time() - s_note.changed_us >= STANDBY_REPORT_DELAY_US;
    standby_note_t n = s_note;
    if (due) s_note.pending = false;
    portEXIT_CRITICAL(&s_note_lock);
    if (!due) return;
    ESP_LOGI(TAG, "standby dark_s=%.1f sleep=%.1f%% sleeps=%lu link_drops=%lu link_ready=%d "
                  "first_key=%s wake_isr_to_event_ms=%.1f press_to_frame_ms=%.1f/%lu",
             n.dark_us / 1e6, n.dark_us > 0 ? 100.0 * (double)n.dark_sleep_us / (double)n.dark_us : 0.0,
             (unsigned long)n.dark_sleeps, (unsigned long)n.link_drops, n.link_ready,
             !n.key_seen ? "none" : n.key_dark ? "dark" : "lit", n.key_isr_us / 1000.0,
             a->press_to_frame_us / 1000.0f, (unsigned long)a->press_frames);
}

static TaskStatus_t *s_tasks;
static prev_rt_t *s_prev;
static UBaseType_t s_cap, s_prev_n;
static configRUN_TIME_COUNTER_TYPE s_prev_total;

static configRUN_TIME_COUNTER_TYPE prev_runtime(TaskHandle_t h)
{
    for (UBaseType_t i = 0; i < s_prev_n; i++) {
        if (s_prev[i].handle == h) return s_prev[i].runtime;
    }
    return 0;
}

// CPU busy percent over the period; prints the task table if asked.
static float sample_tasks(bool print_table)
{
    configRUN_TIME_COUNTER_TYPE total = 0;
    UBaseType_t want = uxTaskGetNumberOfTasks();
    if (want > s_cap) {
        TaskStatus_t *t = realloc(s_tasks, want * sizeof(*t));
        if (t) s_tasks = t;
        prev_rt_t *p = realloc(s_prev, want * sizeof(*p));
        if (p) s_prev = p;
        if (!t || !p) return -1.0f;
        s_cap = want;
    }
    UBaseType_t n = uxTaskGetSystemState(s_tasks, s_cap, &total);
    if (n == 0) return -1.0f;
    configRUN_TIME_COUNTER_TYPE dt = total - s_prev_total;
    configRUN_TIME_COUNTER_TYPE idle = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        configRUN_TIME_COUNTER_TYPE d = s_tasks[i].ulRunTimeCounter - prev_runtime(s_tasks[i].xHandle);
        if (strncmp(s_tasks[i].pcTaskName, "IDLE", 4) == 0) idle += d;
        if (print_table) {
            ESP_LOGI(TAG, "task %-14s prio=%2u stack_free_min=%5u B cpu=%5.1f%%",
                     s_tasks[i].pcTaskName, (unsigned)s_tasks[i].uxCurrentPriority,
                     (unsigned)s_tasks[i].usStackHighWaterMark,
                     dt ? 100.0 * (double)d / (double)dt : 0.0);
        }
    }
    for (UBaseType_t i = 0; i < n; i++) {
        s_prev[i].handle = s_tasks[i].xHandle;
        s_prev[i].runtime = s_tasks[i].ulRunTimeCounter;
    }
    s_prev_n = n;
    s_prev_total = total;
    return dt ? 100.0f * (1.0f - (float)idle / (float)dt) : -1.0f;
}

#if CONFIG_POCKET_DEBUG_MEMPROBE
#include "esp_system.h"
#include "pocket_app.h"

// Display refresh time: LVGL refresh start to ready (render plus SPI flush of
// every invalidated area), last and maximum since the previous snapshot.
static int64_t s_refr_start_us;
static uint32_t s_refr_last_us, s_refr_max_us, s_refr_count;

static void refr_event(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        s_refr_start_us = now;
    } else if (s_refr_start_us) {
        uint32_t us = (uint32_t)(now - s_refr_start_us);
        s_refr_start_us = 0;
        s_refr_last_us = us;
        if (us > s_refr_max_us) s_refr_max_us = us;
        s_refr_count++;
    }
}

static void refr_watch(void)
{
    if (!bsp_lvgl_lock(1000)) return;
    lv_display_t *d = lv_display_get_default();
    if (d) {
        lv_display_add_event_cb(d, refr_event, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(d, refr_event, LV_EVENT_REFR_READY, NULL);
    }
    bsp_lvgl_unlock();
}

// History walk: periods of UP presses, then as many of DOWN presses. Five
// key presses (ten events) per period stay below the app's 16-event queue;
// 18 periods (90 presses) cross five 4 KB replies at ~21 pages each.
#define MEMPROBE_WALK_PRESSES 5
#define MEMPROBE_WALK_STEPS   18
#define MEMPROBE_RESTART_MAGIC 0x5052424fu
// Survives the probe's own software restart, so it restarts once.
static RTC_NOINIT_ATTR uint32_t s_probe_restarted;

// One labelled snapshot: heap per region, LVGL pool, refresh time, task stacks.
static void memprobe_snapshot(const char *label)
{
    lv_mem_monitor_t lv = { 0 };
    uint32_t refr_last = 0, refr_max = 0, refr_n = 0;
    if (bsp_lvgl_lock(100)) {
        lv_mem_monitor(&lv);
        refr_last = s_refr_last_us;
        refr_max = s_refr_max_us;
        refr_n = s_refr_count;
        s_refr_max_us = 0;
        s_refr_count = 0;
        bsp_lvgl_unlock();
    }
    ESP_LOGI(TAG, "memprobe %s: heap total=%u free=%u min=%u largest=%u lv_total=%u lv_used=%u "
                  "lv_max=%u lv_blk=%u lv_frag=%u%% refr_ms=%.1f/%.1f/%u",
             label, (unsigned)heap_caps_get_total_size(INTERNAL_CAPS),
             (unsigned)heap_caps_get_free_size(INTERNAL_CAPS),
             (unsigned)heap_caps_get_minimum_free_size(INTERNAL_CAPS),
             (unsigned)heap_caps_get_largest_free_block(INTERNAL_CAPS), (unsigned)lv.total_size,
             (unsigned)(lv.total_size - lv.free_size), (unsigned)lv.max_used,
             (unsigned)lv.free_biggest_size, (unsigned)lv.frag_pct, refr_last / 1000.0,
             refr_max / 1000.0, (unsigned)refr_n);
    (void)sample_tasks(true);
}

// Invalidates the whole screen so the next refresh redraws every line.
static void full_redraw(void)
{
    if (!bsp_lvgl_lock(1000)) return;
    lv_obj_invalidate(lv_screen_active());
    bsp_lvgl_unlock();
}

// Scripted sequence on the 2 s stats period, from CONFIG_POCKET_DEBUG_MEMPROBE_START_S:
// replay the init marks and heap regions, time a full redraw of the home
// screen, capture and encode into the hold ring (nothing is sent: no press is
// streamed), inject a 4 KB REPLY through the BLE receive path, time a full
// redraw of it, scroll it, end it with REPLY_DONE.
static void memprobe_step(void)
{
    static int step = -1;
    static int64_t next_us;
    int64_t now = esp_timer_get_time();
    if (step < 0) {
        step = 0;
        next_us = (int64_t)CONFIG_POCKET_DEBUG_MEMPROBE_START_S * 1000000;
    }
    if (now < next_us) return;
    const int64_t period_us = (int64_t)CONFIG_POCKET_STATS_PERIOD_MS * 1000;
    switch (step++) {
    case 0:
        for (uint32_t i = 0; i < s_mark_count; i++) {
            ESP_LOGI(TAG, "memprobe mark %-14s free=%lu min=%lu largest=%lu", s_marks[i].stage,
                     (unsigned long)s_marks[i].free, (unsigned long)s_marks[i].min,
                     (unsigned long)s_marks[i].largest);
        }
        heap_caps_print_heap_info(INTERNAL_CAPS);
        refr_watch();
        memprobe_snapshot("idle");
        full_redraw();
        next_us = now + period_us;
        break;
    case 1:
        memprobe_snapshot("home_full_redraw");
        // Settings: open, walk all eight rows (the list scrolls), close.
        pocket_app_debug_key(POCKET_BTN_UP, POCKET_BTN_EV_LONG);
        for (int i = 0; i < POCKET_SET_COUNT - 1; i++) {
            pocket_app_debug_key(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        memprobe_snapshot("settings_scrolled");
        pocket_app_debug_key(POCKET_BTN_UP, POCKET_BTN_EV_LONG);
        ESP_LOGI(TAG, "memprobe: capture on, hold route (encode into the ring)");
        pocket_audio_set_capture(true);
        pocket_audio_arm();
        next_us = now + 5 * period_us;
        break;
    case 2:
        memprobe_snapshot("encoding");
        pocket_audio_discard();
        pocket_audio_set_capture(false);
        next_us = now + period_us;
        break;
    case 3:
        memprobe_snapshot("after_capture");
        pocket_ble_debug_inject_reply(0x7e570001u, POCKET_MAX_MSG_BYTES - 5);
        next_us = now + period_us;
        break;
    case 4:
        memprobe_snapshot("reply_shown");
        full_redraw();
        next_us = now + period_us;
        break;
    case 5:
        memprobe_snapshot("reply_full_redraw");
        next_us = now;
        break;
    case 6: case 7: case 8: case 9: case 10: case 11:
        // Scroll down: DOWN key down and up.
        pocket_app_debug_key(POCKET_BTN_DOWN, POCKET_BTN_EV_DOWN);
        pocket_app_debug_key(POCKET_BTN_DOWN, POCKET_BTN_EV_UP);
        next_us = now;
        break;
    case 12:
        memprobe_snapshot("reply_scrolled");
        pocket_ble_debug_inject_reply(0x7e570001u, 0);
        next_us = now + period_us;
        break;
    case 13:
        memprobe_snapshot("reply_done");
        // Reply history: four more replies fill the 16 KB budget, a fifth
        // evicts the first.
        pocket_ble_debug_inject_reply(0x7e570002u, 4000);
        next_us = now;
        break;
    case 14:
        pocket_ble_debug_inject_reply(0x7e570003u, 300);
        next_us = now;
        break;
    case 15:
        pocket_ble_debug_inject_reply(0x7e570004u, 4000);
        next_us = now;
        break;
    case 16:
        pocket_ble_debug_inject_reply(0x7e570005u, 4000);
        next_us = now;
        break;
    case 17:
        pocket_ble_debug_inject_reply(0x7e570006u, 300);
        next_us = now;
        break;
    case 18:
        memprobe_snapshot("history_stored");
        next_us = now;
        break;
    default:
        if (step <= 19 + 2 * MEMPROBE_WALK_STEPS) {
            // Walk the history: UP to the oldest, then DOWN to the newest,
            // a few presses per period so the event queue never fills.
            int k = step - 20;
            pocket_btn_t b = k < MEMPROBE_WALK_STEPS ? POCKET_BTN_UP : POCKET_BTN_DOWN;
            for (int i = 0; i < MEMPROBE_WALK_PRESSES; i++) {
                pocket_app_debug_key(b, POCKET_BTN_EV_DOWN);
                pocket_app_debug_key(b, POCKET_BTN_EV_UP);
            }
            next_us = now;
            break;
        }
        if (step == 20 + 2 * MEMPROBE_WALK_STEPS) {
            memprobe_snapshot("history_walked");
            // One software restart shows the stored newest reply after boot.
            if (s_probe_restarted != MEMPROBE_RESTART_MAGIC) {
                s_probe_restarted = MEMPROBE_RESTART_MAGIC;
                ESP_LOGI(TAG, "memprobe: restart to check the stored history");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
            s_probe_restarted = 0;
            ESP_LOGI(TAG, "memprobe: done");
        }
        next_us = INT64_MAX;
        break;
    }
}
#endif

#if CONFIG_POCKET_DEBUG_FLOWPROBE
#include "pocket_app.h"
#include "pocket_proto.h"

// Dense burst for the event queue peak: everything a phone may send for one
// answer, queued on the host at once (denser than a real link, where a
// connection event carries a few writes). RESULT 0 with a transcript, WORK
// 1/2/3/2/3, a 4 KB REPLY in 249-byte fragments (ATT MTU 256), REPLY_DONE.
#define BURST_FRAG 249
// Busy variant: the LVGL lock is held this long, the measured full redraw of
// the 4 KB reply (~104 ms) rounded up.
#define BURST_BUSY_MS 120
static void flowprobe_burst(uint16_t id)
{
    // 8 CJK characters, 24 B: the message exceeds 16 B, so the app keeps it.
    static const char text[] = "\xe4\xbb\x8a\xe5\xa4\xa9\xe5\xa4\xa9\xe6\xb0\x94"
                               "\xe6\x80\x8e\xe4\xb9\x88\xe6\xa0\xb7\xef\xbc\x9f";
    uint8_t r[3 + sizeof text];
    r[0] = (uint8_t)id;
    r[1] = (uint8_t)(id >> 8);
    r[2] = POCKET_RESULT_TRANSCRIBED;
    memcpy(r + 3, text, sizeof text - 1);
    pocket_ble_debug_inject_msg(POCKET_MSG_RESULT, r, 3 + sizeof text - 1);
    static const uint8_t phases[] = { POCKET_WORK_RECEIVED, POCKET_WORK_THINKING, POCKET_WORK_TOOL,
                                      POCKET_WORK_THINKING, POCKET_WORK_TOOL };
    for (size_t i = 0; i < sizeof phases; i++) {
        pocket_ble_debug_inject_msg(POCKET_MSG_WORK, &phases[i], 1);
    }
    static uint8_t reply[POCKET_MAX_MSG_BYTES];
    const uint32_t reply_id = 0x7e57;
    reply[0] = (uint8_t)reply_id;
    reply[1] = (uint8_t)(reply_id >> 8);
    reply[2] = 0;
    reply[3] = 0;
    reply[4] = 1;  // final
    for (size_t i = 5; i < sizeof reply; i++) reply[i] = (i % 40 == 39) ? ' ' : (uint8_t)('a' + i % 26);
    for (size_t off = 0; off < sizeof reply; off += BURST_FRAG) {
        size_t n = sizeof reply - off < BURST_FRAG ? sizeof reply - off : BURST_FRAG;
        pocket_ble_debug_inject_pdu(POCKET_MSG_REPLY, off + n < sizeof reply, reply + off, n);
    }
    pocket_ble_debug_inject_msg(POCKET_MSG_REPLY_DONE, reply, 4);
}

// The reply-wait flow of a real press, 30 s after boot: wait (sending
// screen), RESULT 0 with a 42-byte transcript in one PDU and WORK 1 right
// behind it, WORK 3, then REPLY_DONE 0 so no keepalive goes out. Then a
// second wait with the dense burst above.
static void flowprobe_step(void)
{
    static int step;
    if (esp_timer_get_time() < 30000000 || step > 10) return;
    uint16_t id = pocket_app_debug_wait_press_id();
    uint8_t b[16];
    switch (step++) {
    case 0:
        ESP_LOGI(TAG, "flowprobe: begin wait, screen %d", pocket_app_debug_screen());
        pocket_app_debug_begin_wait();
        return;
    case 1: {
        // As the phone sends them: RESULT 0 with a transcript longer than a
        // small message, WORK 1 right behind it, then WORK 3. All within one
        // period, so the wait ends before its first keepalive (5 s).
        static const char text[] = "\xe4\xbb\x8a\xe5\xa4\xa9\xe5\xa4\xa9\xe6\xb0\x94"
                                   "\xe6\x80\x8e\xe4\xb9\x88\xe6\xa0\xb7\xef\xbc\x9f"
                                   "\xe6\x98\x8e\xe5\xa4\xa9\xe8\xa6\x81\xe5\xb8\xa6"
                                   "\xe4\xbc\x9e\xe5\x90\x97";  // 14 CJK characters, 42 B
        uint8_t r[3 + sizeof text];
        r[0] = (uint8_t)id;
        r[1] = (uint8_t)(id >> 8);
        r[2] = POCKET_RESULT_TRANSCRIBED;
        memcpy(r + 3, text, sizeof text - 1);
        ESP_LOGI(TAG, "flowprobe: press %u, screen %d", id, pocket_app_debug_screen());
        pocket_ble_debug_inject_msg(POCKET_MSG_RESULT, r, 3 + sizeof text - 1);
        b[0] = POCKET_WORK_RECEIVED;
        pocket_ble_debug_inject_msg(POCKET_MSG_WORK, b, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP_LOGI(TAG, "flowprobe: after RESULT screen %d (THINKING = %d)", pocket_app_debug_screen(),
                 POCKET_SCR_THINKING);
        b[0] = POCKET_WORK_TOOL;
        pocket_ble_debug_inject_msg(POCKET_MSG_WORK, b, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
        memset(b, 0, 4);
        pocket_ble_debug_inject_msg(POCKET_MSG_REPLY_DONE, b, 4);
        return;
    }
    case 2:
    case 4:
        ESP_LOGI(TAG, "flowprobe: burst wait, evq_peak before %lu",
                 (unsigned long)pocket_app_debug_queue_peak());
        pocket_app_debug_queue_peak_reset();
        pocket_app_debug_begin_wait();
        return;
    case 3:
    case 5: {
        // Per-message log lines are silenced: console output would let the
        // app task drain the queue between injections. Step 5 also holds the
        // LVGL lock for BURST_BUSY_MS, as a full redraw does, so the app task
        // blocks at its first screen update while the burst arrives.
        const bool busy = step == 6;  // case 5; step is already incremented
        esp_log_level_set("pocket_ble", ESP_LOG_WARN);
        esp_log_level_set("pocket_app", ESP_LOG_WARN);
        bool locked = busy && bsp_lvgl_lock(1000);
        pocket_app_debug_queue_trace(true);
        flowprobe_burst(id);
        if (locked) {
            vTaskDelay(pdMS_TO_TICKS(BURST_BUSY_MS));
            bsp_lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_log_level_set("pocket_ble", ESP_LOG_INFO);
        esp_log_level_set("pocket_app", ESP_LOG_INFO);
        pocket_app_debug_queue_trace(false);
        ESP_LOGI(TAG, "flowprobe: burst (%s) for press %u: evq_peak %lu, screen %d",
                 busy ? "app busy" : "app idle", id, (unsigned long)pocket_app_debug_queue_peak(),
                 pocket_app_debug_screen());
        return;
    }
    case 6:
    case 7:
    case 8:
    case 9: {
        // APP_STATE language (1.2): English, a 1.0/1.1 phone (no byte), an
        // unknown value, then Simplified Chinese again.
        static const uint8_t lang[4][2] = { { 0, POCKET_LANG_EN }, { 0, 0 }, { 0, 9 },
                                            { 0, POCKET_LANG_ZH_HANS } };
        static const uint8_t len[4] = { 2, 1, 2, 2 };
        const int k = step - 7;  // step is already incremented
        pocket_ble_debug_inject_msg(POCKET_MSG_APP_STATE, lang[k], len[k]);
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP_LOGI(TAG, "flowprobe: APP_STATE len %u lang byte %u -> lang %d, screen %d",
                 (unsigned)len[k], (unsigned)lang[k][1], pocket_app_debug_lang(),
                 pocket_app_debug_screen());
        return;
    }
    default:
        ESP_LOGI(TAG, "flowprobe: done, screen %d", pocket_app_debug_screen());
        step = 11;
        return;
    }
}
#endif

static void stats_task(void *arg)
{
    (void)arg;
    uint32_t period = 0;
    (void)sample_tasks(false);
    uint64_t prev_sleep_us = 0;
    uint32_t prev_sleeps = 0;
    int64_t prev_t = esp_timer_get_time();
    // heap_pmin: lowest free heap within the period (local minimum monitor).
    (void)heap_caps_monitor_local_minimum_free_size_start();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_POCKET_STATS_PERIOD_MS));
        period++;
        const unsigned heap_pmin = (unsigned)heap_caps_get_minimum_free_size(INTERNAL_CAPS);
        (void)heap_caps_monitor_local_minimum_free_size_stop();
        const unsigned heap_min = (unsigned)heap_caps_get_minimum_free_size(INTERNAL_CAPS);
        (void)heap_caps_monitor_local_minimum_free_size_start();
        portENTER_CRITICAL(&s_sleep_lock);
        uint64_t sleep_us = s_sleep_us;
        uint32_t sleeps = s_sleeps;
        portEXIT_CRITICAL(&s_sleep_lock);
        int64_t t = esp_timer_get_time();
        float sleep_pct = t > prev_t ? 100.0f * (float)(sleep_us - prev_sleep_us) / (float)(t - prev_t)
                                     : 0.0f;
        uint32_t wakes = sleeps - prev_sleeps;
        prev_sleep_us = sleep_us;
        prev_sleeps = sleeps;
        prev_t = t;
        float cpu = sample_tasks(period % TASK_TABLE_EVERY == 0);
        pocket_audio_stats_t a;
        pocket_audio_stats(&a, true);
        report_standby(&a);
        pocket_ble_stats_t b;
        pocket_ble_stats(&b);
        lv_mem_monitor_t lv = { 0 };
        if (bsp_lvgl_lock(100)) {
            lv_mem_monitor(&lv);
            bsp_lvgl_unlock();
        }
        uint32_t enc_avg = a.enc_count ? a.enc_us_sum / a.enc_count : 0;
        float kbps = a.enc_count ? (float)a.enc_bytes * 8.0f / (a.enc_count * (float)POCKET_FRAME_MS) : 0;
        ESP_LOGI(TAG,
                 "t=%llus heap_free=%u heap_min=%u heap_pmin=%u heap_blk=%u cpu=%.1f%% "
                 "sleep=%.1f%% sleeps=%lu press_ms=%.1f/%lu codec_wake_ms=%.1f "
                 "enc_n=%lu enc_us=%lu/%lu enc_load=%.1f%% kbps=%.1f "
                 "frames=%lu rx_ovf=%lu mic_err=%lu drop=%lu enc_err=%lu wake_max_ms=%.1f "
                 "sent=%lu lost=%lu ring_evict=%lu tones=%lu/%lu "
                 "mtu=%u tx=%lu drop=%lu retry=%lu err=%lu buf_free=%lu/%lu rx=%lu rx_err=%lu "
                 "lv_used=%u lv_max=%u lv_free=%u lv_blk=%u evq_peak=%lu",
                 (unsigned long long)(esp_timer_get_time() / 1000000),
                 (unsigned)heap_caps_get_free_size(INTERNAL_CAPS), heap_min, heap_pmin,
                 (unsigned)heap_caps_get_largest_free_block(INTERNAL_CAPS), cpu,
                 sleep_pct, (unsigned long)wakes, a.press_to_frame_us / 1000.0f,
                 (unsigned long)a.press_frames, a.last_wake_us / 1000.0f,
                 (unsigned long)a.enc_count, (unsigned long)enc_avg, (unsigned long)a.enc_us_max,
                 100.0f * enc_avg / (POCKET_FRAME_MS * 1000.0f), kbps,
                 (unsigned long)a.frames, (unsigned long)a.rx_overflow,
                 (unsigned long)a.mic_errors, (unsigned long)a.drop_enc_in,
                 (unsigned long)a.enc_errors, a.wake_us_max / 1000.0f,
                 (unsigned long)a.sent_packets, (unsigned long)a.lost_packets,
                 (unsigned long)a.ring_evicted, (unsigned long)a.tones,
                 (unsigned long)a.tone_errors,
                 (unsigned)b.mtu, (unsigned long)b.tx_msgs, (unsigned long)b.tx_dropped,
                 (unsigned long)b.tx_retries, (unsigned long)b.tx_errors,
                 (unsigned long)b.tx_buffer_free, (unsigned long)b.tx_buffer_min,
                 (unsigned long)b.rx_msgs, (unsigned long)b.rx_errors,
                 (unsigned)(lv.total_size - lv.free_size), (unsigned)lv.max_used,
                 (unsigned)lv.free_size, (unsigned)lv.free_biggest_size,
                 (unsigned long)pocket_app_debug_queue_peak());
#if CONFIG_POCKET_DEBUG_MEMPROBE
        memprobe_step();
#endif
#if CONFIG_POCKET_DEBUG_FLOWPROBE
        flowprobe_step();
#endif
    }
}

esp_err_t pocket_stats_start(void)
{
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    esp_pm_sleep_cbs_register_config_t cbs = { .exit_cb = on_light_sleep_exit };
    if (esp_pm_light_sleep_register_cbs(&cbs) != ESP_OK) ESP_LOGW(TAG, "no light-sleep counters");
#endif
    return xTaskCreate(stats_task, "pocket_stats", CONFIG_POCKET_STATS_TASK_STACK, NULL,
                       CONFIG_POCKET_PRIO_STATS, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

#else

void pocket_stats_heap_mark(const char *stage) { (void)stage; }
esp_err_t pocket_stats_start(void) { return ESP_OK; }
void pocket_stats_note_screen(bool on) { (void)on; }
void pocket_stats_note_link(bool ready) { (void)ready; }
void pocket_stats_note_key(bool dark, int64_t wake_isr_to_event_us)
{
    (void)dark;
    (void)wake_isr_to_event_us;
}

#endif
