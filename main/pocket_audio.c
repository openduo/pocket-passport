// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

#include "pocket_audio.h"

#include "pocket_ble.h"
#include "pocket_config.h"
#include "pocket_proto.h"
#include "pocket_ring.h"
#include "pocket_stats.h"

#include "bsp_audio.h"
#include "driver/i2s_common.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <math.h>
#include <string.h>

static const char *TAG = "pocket_audio";

// Ring for pre-roll plus the tap-threshold hold, in bytes. Basis: packets for
// pre-roll + tap window + 2 frames of command latency, each 2 B of length
// plus 120 B (2.5x the ~48 B average packet at the 19 kbps libopus AUTO
// bitrate for 16 kHz mono 20 ms, measured 19.4 kbps). If VBR peaks exceed it, the
// oldest pre-roll packets are evicted and counted (ring_evicted).
#define RING_PACKET_BUDGET 120
#define RING_FRAMES ((CONFIG_POCKET_PREROLL_MS + POCKET_TAP_THRESHOLD_MS) / POCKET_FRAME_MS + 2)
#define RING_BYTES (RING_FRAMES * (2 + RING_PACKET_BUDGET))
#define PREROLL_FRAMES (CONFIG_POCKET_PREROLL_MS / POCKET_FRAME_MS)

// PCM frames between the mic and the encoder. Basis: the depth the encoder
// timing was measured with; a stage may fall 1-2 frames behind before a drop
// is counted.
#define PCM_QUEUE_DEPTH 3
// Pending control commands. Largest burst: one model step posts one command
// per kind it requests, at most the six kinds (cmd_kind_t), back to back,
// even if the encoder task could not run in between.
#define CMD_QUEUE_DEPTH 6

// New-reply tone, generated (no stored audio): two short sine notes a fifth
// apart, rising. Aesthetic choices: A5 880 Hz for 70 ms, then E6 1320 Hz for
// 110 ms (a small speaker reproduces little below ~800 Hz), 4 ms attack and a
// linear fade over the last 40 % of each note so no note clicks, peak 12000
// (about -8.7 dBFS) at codec output volume 70 %. Pending a listening check
// on the device.
#define TONE_PEAK        12000.0f
#define TONE_VOLUME_PCT  70
#define TONE_ATTACK_SAMPLES (POCKET_SAMPLE_RATE_HZ * 4 / 1000)
// Silent frames after the notes before the output volume returns to 0: the
// I2S TX DMA holds 6 x 240 frames = 90 ms (bsp_audio.c), so 5 frames of
// 20 ms play the tail of the last note out.
#define TONE_TAIL_FRAMES 5
typedef struct {
    uint16_t hz;
    uint16_t ms;
} tone_note_t;
static const tone_note_t s_tone[] = { { 880, 70 }, { 1320, 110 } };

typedef enum { CMD_ARM, CMD_STREAM, CMD_FINISH, CMD_DISCARD, CMD_ABORT, CMD_PREROLL } cmd_kind_t;
typedef struct {
    cmd_kind_t kind;
    uint16_t press_id;
    bool flag;
} cmd_t;

typedef enum { ROUTE_IDLE, ROUTE_HOLD, ROUTE_STREAM } route_t;

static QueueHandle_t s_pcm_q;
static QueueHandle_t s_cmd_q;
// The encoder blocks on both queues at once, so it sleeps while idle instead
// of polling every frame period (light sleep needs idle tasks).
static QueueSetHandle_t s_enc_set;
#if CONFIG_PM_ENABLE
// Held while the codec is awake: I2S DMA stops in light sleep, and the
// encoder timing was measured at the full CPU clock.
static esp_pm_lock_handle_t s_pm_lock;
#endif
// Press latency (diagnostic): button event time of the last armed press,
// cleared by the first PCM frame read after it.
static volatile int64_t s_press_us;
static TaskHandle_t s_mic_task;
static volatile bool s_capture;
static volatile bool s_tone_req;
static volatile bool s_mic_idle;    // the codec sleeps and the mic task waits
static volatile uint16_t s_level;
static volatile uint32_t s_rx_ovf;

static void *s_enc;
static int s_pkt_max;
static int16_t *s_mic_pcm, *s_enc_pcm;
static uint8_t *s_pkt, *s_msg, *s_ring_buf;
static pocket_ring_t s_ring;

static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static pocket_audio_stats_t s_stats;

static IRAM_ATTR bool on_rx_ovf(i2s_chan_handle_t h, i2s_event_data_t *e, void *u)
{
    (void)h;
    (void)e;
    (void)u;
    s_rx_ovf++;
    return false;
}

static void post(cmd_kind_t kind, uint16_t press_id, bool flag)
{
    cmd_t c = { .kind = kind, .press_id = press_id, .flag = flag };
    if (xQueueSend(s_cmd_q, &c, 0) != pdTRUE) ESP_LOGE(TAG, "command queue full: %d", kind);
}

void pocket_audio_arm(void) { post(CMD_ARM, 0, false); }

void pocket_audio_mark_press(int64_t press_us) { s_press_us = press_us; }
void pocket_audio_stream(uint16_t press_id) { post(CMD_STREAM, press_id, false); }
void pocket_audio_finish(void) { post(CMD_FINISH, 0, false); }
void pocket_audio_discard(void) { post(CMD_DISCARD, 0, false); }
void pocket_audio_abort(void) { post(CMD_ABORT, 0, false); }
void pocket_audio_set_preroll(bool on) { post(CMD_PREROLL, 0, on); }

void pocket_audio_tone(void)
{
    s_tone_req = true;
    if (s_mic_task) xTaskNotifyGive(s_mic_task);
}

// Wait for the mic task to sleep the codec. Basis: one 20 ms frame read and
// the ES8311 suspend sequence (I2C writes and read-back) take well under
// 100 ms; 500 ms is a guard against a stuck task, after which deep sleep
// proceeds with the BSP's own codec suspend.
#define SHUTDOWN_WAIT_MS 500
// Half a 20 ms frame: deep sleep starts at most 10 ms after the mic task
// went idle.
#define SHUTDOWN_POLL_MS (POCKET_FRAME_MS / 2)

void pocket_audio_shutdown(void)
{
    s_tone_req = false;
    s_capture = false;
    if (s_mic_task) xTaskNotifyGive(s_mic_task);
    for (int waited = 0; !s_mic_idle && waited < SHUTDOWN_WAIT_MS; waited += SHUTDOWN_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(SHUTDOWN_POLL_MS));
    }
    if (!s_mic_idle) ESP_LOGW(TAG, "mic task still busy at shutdown");
}

void pocket_audio_set_capture(bool on)
{
    s_capture = on;
    if (on && s_mic_task) xTaskNotifyGive(s_mic_task);
}

uint16_t pocket_audio_level(void)
{
    return s_capture ? s_level : 0;
}

void pocket_audio_stats(pocket_audio_stats_t *out, bool reset_window)
{
    portENTER_CRITICAL(&s_stats_lock);
    *out = s_stats;
    out->rx_overflow = s_rx_ovf;
    if (reset_window) {
        s_stats.enc_count = 0;
        s_stats.enc_us_sum = 0;
        s_stats.enc_us_max = 0;
        s_stats.enc_bytes = 0;
    }
    portEXIT_CRITICAL(&s_stats_lock);
}

// --- Tone synthesis (mic task) ------------------------------------------------

typedef struct {
    int note;                   // index into s_tone, or the tail after the last
    uint32_t pos, len;          // sample within the note, note length
    float c, y1, y2;            // sine oscillator: y[n] = c*y[n-1] - y[n-2]
    int tail;                   // silent frames left after the notes
} tone_t;

static void tone_note(tone_t *t)
{
    if (t->note >= (int)(sizeof s_tone / sizeof s_tone[0])) return;
    const float w = 2.0f * 3.14159265f * s_tone[t->note].hz / POCKET_SAMPLE_RATE_HZ;
    t->c = 2.0f * cosf(w);
    t->y1 = 0.0f;               // sin(0)
    t->y2 = -sinf(w);           // sin(-w)
    t->pos = 0;
    t->len = (uint32_t)s_tone[t->note].ms * POCKET_SAMPLE_RATE_HZ / 1000;
}

static void tone_start(tone_t *t)
{
    *t = (tone_t){ .note = 0, .tail = TONE_TAIL_FRAMES };
    tone_note(t);
}

// Fills one frame; false once the notes and the silent tail are out.
static bool tone_fill(tone_t *t, int16_t *pcm, int n)
{
    const int notes = (int)(sizeof s_tone / sizeof s_tone[0]);
    if (t->note >= notes) {
        memset(pcm, 0, (size_t)n * sizeof *pcm);
        return t->tail-- > 0;
    }
    for (int i = 0; i < n; i++) {
        float v = 0.0f;
        if (t->note < notes) {
            float y = t->c * t->y1 - t->y2;
            t->y2 = t->y1;
            t->y1 = y;
            float env = 1.0f;
            const uint32_t fade = t->len * 2 / 5;
            if (t->pos < TONE_ATTACK_SAMPLES) env = (float)t->pos / TONE_ATTACK_SAMPLES;
            else if (t->pos > t->len - fade) env = (float)(t->len - t->pos) / (float)fade;
            v = y * env * TONE_PEAK;
            if (++t->pos >= t->len) {
                t->note++;
                tone_note(t);
            }
        }
        pcm[i] = (int16_t)v;
    }
    return true;
}

// --- Mic task: the only PCM reader and writer, the only caller of sleep/wake.

static void mic_task(void *arg)
{
    (void)arg;
    bool awake = true;  // bsp_audio_init leaves the codec running
    bool toning = false;
    tone_t tone;
    for (;;) {
        if (!s_capture && !s_tone_req && !toning) {
            if (awake) {
                esp_err_t e = bsp_audio_sleep();
                if (e != ESP_OK) ESP_LOGE(TAG, "audio sleep failed: %s", esp_err_to_name(e));
                awake = false;
                s_level = 0;
#if CONFIG_PM_ENABLE
                esp_pm_lock_release(s_pm_lock);
#endif
            }
            s_mic_idle = true;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            s_mic_idle = false;
            continue;
        }
        if (!awake) {
#if CONFIG_PM_ENABLE
            esp_pm_lock_acquire(s_pm_lock);
#endif
            int64_t t0 = esp_timer_get_time();
            esp_err_t e = bsp_audio_wake();
            uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
            portENTER_CRITICAL(&s_stats_lock);
            if (us > s_stats.wake_us_max) s_stats.wake_us_max = us;
            s_stats.last_wake_us = us;
            portEXIT_CRITICAL(&s_stats_lock);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "audio wake failed: %s", esp_err_to_name(e));
#if CONFIG_PM_ENABLE
                esp_pm_lock_release(s_pm_lock);
#endif
                // One frame period: this task outranks all but the radio, so
                // a retry without a delay would starve the rest; at the frame
                // cadence a failing loop costs what normal capture does.
                vTaskDelay(pdMS_TO_TICKS(POCKET_FRAME_MS));
                continue;
            }
            awake = true;
        }
        if (s_tone_req) {
            s_tone_req = false;
            tone_start(&tone);
            toning = true;
            bsp_audio_set_volume(TONE_VOLUME_PCT);
        }
        if (toning) {
            // Written before the frame is read into the same buffer: the
            // write copies it into the DMA ring.
            toning = tone_fill(&tone, s_mic_pcm, POCKET_FRAME_SAMPLES);
            if (bsp_audio_write(s_mic_pcm, POCKET_FRAME_BYTES) != ESP_OK) {
                portENTER_CRITICAL(&s_stats_lock);
                s_stats.tone_errors++;
                portEXIT_CRITICAL(&s_stats_lock);
            }
            if (!toning) {
                bsp_audio_set_volume(0);
                portENTER_CRITICAL(&s_stats_lock);
                s_stats.tones++;
                portEXIT_CRITICAL(&s_stats_lock);
            }
        }
        if (bsp_audio_read(s_mic_pcm, POCKET_FRAME_BYTES) != ESP_OK) {
            portENTER_CRITICAL(&s_stats_lock);
            s_stats.mic_errors++;
            portEXIT_CRITICAL(&s_stats_lock);
            vTaskDelay(pdMS_TO_TICKS(POCKET_FRAME_MS));  // as for a failed wake
            continue;
        }
        int64_t press = s_press_us;
        if (press) {
            s_press_us = 0;
            uint32_t us = (uint32_t)(esp_timer_get_time() - press);
            portENTER_CRITICAL(&s_stats_lock);
            s_stats.press_to_frame_us = us;
            s_stats.press_frames++;
            portEXIT_CRITICAL(&s_stats_lock);
        }
        // Playing the tone only: the frame keeps the RX ring drained.
        if (!s_capture) continue;
        uint16_t peak = 0;
        for (int i = 0; i < POCKET_FRAME_SAMPLES; i++) {
            int v = s_mic_pcm[i] < 0 ? -s_mic_pcm[i] : s_mic_pcm[i];
            if (v > peak) peak = (uint16_t)(v > 32767 ? 32767 : v);
        }
        s_level = peak;
        bool dropped = xQueueSend(s_pcm_q, s_mic_pcm, 0) != pdTRUE;
        portENTER_CRITICAL(&s_stats_lock);
        s_stats.frames++;
        if (dropped) s_stats.drop_enc_in++;
        portEXIT_CRITICAL(&s_stats_lock);
    }
}

// --- Encoder task: owns the encoder, the ring and the press stream. --------

typedef struct {
    route_t route;
    bool preroll;
    uint16_t press_id;
    uint16_t seq;
} stream_t;

// AUDIO payload header: [press_id u16][seq u16] before the Opus packet.
#define AUDIO_HEADER_BYTES 4

static void send_audio(stream_t *st, const uint8_t *pkt, size_t len)
{
    size_t n = pocket_msg_audio(s_msg, AUDIO_HEADER_BYTES + (size_t)s_pkt_max, st->press_id,
                                st->seq, pkt, len);
    st->seq++;  // a refused packet still consumes a seq: the phone sees the gap
    bool ok = n && pocket_ble_send(POCKET_MSG_AUDIO, s_msg, n);
    portENTER_CRITICAL(&s_stats_lock);
    if (ok) s_stats.sent_packets++;
    else s_stats.lost_packets++;
    portEXIT_CRITICAL(&s_stats_lock);
}

static void reset_idle(stream_t *st)
{
    st->route = ROUTE_IDLE;
    pocket_ring_clear(&s_ring);
}

static void handle_cmd(stream_t *st, const cmd_t *c)
{
    uint8_t b[8];  // PRESS_START (2 B) and PRESS_END (4 B) payloads
    switch (c->kind) {
    case CMD_PREROLL:
        st->preroll = c->flag;
        if (!st->preroll && st->route == ROUTE_IDLE) pocket_ring_clear(&s_ring);
        break;
    case CMD_ARM:
        if (st->route != ROUTE_IDLE) break;
        // Without pre-roll each press starts a fresh stream; with pre-roll
        // the encoder has been running and the ring holds its packets.
        if (!st->preroll) {
            esp_opus_enc_reset(s_enc);
            pocket_ring_clear(&s_ring);
        }
        st->route = ROUTE_HOLD;
        break;
    case CMD_STREAM: {
        if (st->route != ROUTE_HOLD) break;
        st->press_id = c->press_id;
        st->seq = 0;
        st->route = ROUTE_STREAM;
        size_t n = pocket_msg_press_start(b, sizeof b, st->press_id);
        if (!pocket_ble_send(POCKET_MSG_PRESS_START, b, n)) {
            ESP_LOGW(TAG, "PRESS_START %u refused by the link", st->press_id);
        }
        // Pre-roll and the held tap window precede the live packets.
        bool dropped;
        size_t len;
        while ((len = pocket_ring_pop(&s_ring, s_pkt, (size_t)s_pkt_max, &dropped)) > 0 || dropped) {
            if (len) send_audio(st, s_pkt, len);
        }
        break;
    }
    case CMD_FINISH:
        if (st->route == ROUTE_STREAM) {
            size_t n = pocket_msg_press_end(b, sizeof b, st->press_id, st->seq);
            if (!pocket_ble_send(POCKET_MSG_PRESS_END, b, n)) {
                ESP_LOGW(TAG, "PRESS_END %u refused by the link", st->press_id);
            }
            ESP_LOGI(TAG, "press %u: %u packets", st->press_id, st->seq);
        }
        reset_idle(st);
        break;
    case CMD_DISCARD:
    case CMD_ABORT:
        reset_idle(st);
        break;
    }
}

static void enc_task(void *arg)
{
    (void)arg;
    stream_t st = { .route = ROUTE_IDLE };
    cmd_t c;
    for (;;) {
        // Commands and frames are handled in arrival order.
        QueueSetMemberHandle_t q = xQueueSelectFromSet(s_enc_set, portMAX_DELAY);
        if (q == s_cmd_q) {
            if (xQueueReceive(s_cmd_q, &c, 0) == pdTRUE) handle_cmd(&st, &c);
            continue;
        }
        if (q != s_pcm_q || xQueueReceive(s_pcm_q, s_enc_pcm, 0) != pdTRUE) continue;
        if (st.route == ROUTE_IDLE && !st.preroll) continue;  // nothing wants it

        esp_audio_enc_in_frame_t in = { .buffer = (uint8_t *)s_enc_pcm, .len = POCKET_FRAME_BYTES };
        esp_audio_enc_out_frame_t out = { .buffer = s_pkt, .len = (uint32_t)s_pkt_max };
        int64_t t0 = esp_timer_get_time();
        esp_audio_err_t r = esp_opus_enc_process(s_enc, &in, &out);
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        portENTER_CRITICAL(&s_stats_lock);
        if (r != ESP_AUDIO_ERR_OK) {
            s_stats.enc_errors++;
        } else {
            s_stats.enc_count++;
            s_stats.enc_us_sum += us;
            if (us > s_stats.enc_us_max) s_stats.enc_us_max = us;
            s_stats.enc_bytes += out.encoded_bytes;
        }
        portEXIT_CRITICAL(&s_stats_lock);
        if (r != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0) continue;

        if (st.route == ROUTE_STREAM) {
            send_audio(&st, s_pkt, out.encoded_bytes);
        } else {
            uint32_t before = s_ring.evicted;
            pocket_ring_push(&s_ring, s_pkt, out.encoded_bytes);
            if (st.route == ROUTE_IDLE) pocket_ring_trim(&s_ring, PREROLL_FRAMES);
            else if (s_ring.evicted != before) {
                portENTER_CRITICAL(&s_stats_lock);
                s_stats.ring_evicted += s_ring.evicted - before;
                portEXIT_CRITICAL(&s_stats_lock);
            }
        }
    }
}

static void *internal_alloc(size_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

esp_err_t pocket_audio_start(void)
{
    const i2s_event_callbacks_t rx = { .on_recv_q_ovf = on_rx_ovf };
    esp_err_t e = bsp_audio_set_i2s_callbacks(NULL, &rx, NULL);
    if (e != ESP_OK) return e;
    e = bsp_audio_init();
    if (e != ESP_OK) return e;
    pocket_stats_heap_mark("i2s_codec");
    e = bsp_audio_set_format(POCKET_SAMPLE_RATE_HZ, 16, 1);
    if (e != ESP_OK) return e;
    pocket_stats_heap_mark("codec_format");
    // The DAC stays silent except while the new-reply tone plays.
    bsp_audio_set_volume(0);

    esp_opus_enc_config_t cfg = {
        .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel = ESP_AUDIO_MONO,
        .bits_per_sample = ESP_AUDIO_BIT16,
        .bitrate = ESP_OPUS_BITRATE_AUTO,
        .frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS,
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
        .complexity = POCKET_OPUS_COMPLEXITY,
        .enable_fec = false,
        .enable_dtx = false,
        .enable_vbr = true,
    };
    if (esp_opus_enc_open(&cfg, sizeof cfg, &s_enc) != ESP_AUDIO_ERR_OK || !s_enc) {
        ESP_LOGE(TAG, "opus encoder open failed");
        return ESP_FAIL;
    }
    int in_size = 0;
    if (esp_opus_enc_get_frame_size(s_enc, &in_size, &s_pkt_max) != ESP_AUDIO_ERR_OK ||
        in_size != POCKET_FRAME_BYTES || s_pkt_max <= 0) {
        ESP_LOGE(TAG, "unexpected opus frame size in=%d out=%d", in_size, s_pkt_max);
        return ESP_FAIL;
    }
    if (s_pkt_max > POCKET_OPUS_MAX_PACKET) s_pkt_max = POCKET_OPUS_MAX_PACKET;
    pocket_stats_heap_mark("opus_enc_open");

    s_pcm_q = xQueueCreate(PCM_QUEUE_DEPTH, POCKET_FRAME_BYTES);
    s_cmd_q = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(cmd_t));
    // One set slot per queue slot, so the set never overflows.
    s_enc_set = xQueueCreateSet(PCM_QUEUE_DEPTH + CMD_QUEUE_DEPTH);
    if (!s_pcm_q || !s_cmd_q || !s_enc_set || xQueueAddToSet(s_pcm_q, s_enc_set) != pdPASS ||
        xQueueAddToSet(s_cmd_q, s_enc_set) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_PM_ENABLE
    e = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "pocket_audio", &s_pm_lock);
    if (e != ESP_OK) return e;
    // bsp_audio_init left the codec running; the mic task sleeps it.
    esp_pm_lock_acquire(s_pm_lock);
#endif
    s_mic_pcm = internal_alloc(POCKET_FRAME_BYTES);
    s_enc_pcm = internal_alloc(POCKET_FRAME_BYTES);
    s_pkt = internal_alloc((size_t)s_pkt_max);
    s_msg = internal_alloc(AUDIO_HEADER_BYTES + (size_t)s_pkt_max);
    s_ring_buf = internal_alloc(RING_BYTES);
    if (!s_pcm_q || !s_cmd_q || !s_mic_pcm || !s_enc_pcm || !s_pkt || !s_msg || !s_ring_buf) {
        return ESP_ERR_NO_MEM;
    }
    pocket_ring_init(&s_ring, s_ring_buf, RING_BYTES);
    pocket_stats_heap_mark("audio_buffers");

    if (xTaskCreate(enc_task, "pocket_enc", CONFIG_POCKET_ENC_TASK_STACK, NULL,
                    CONFIG_POCKET_PRIO_CODEC, NULL) != pdPASS ||
        xTaskCreate(mic_task, "pocket_mic", CONFIG_POCKET_MIC_TASK_STACK, NULL,
                    CONFIG_POCKET_PRIO_AUDIO_IO, &s_mic_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    pocket_stats_heap_mark("audio_tasks");
    ESP_LOGI(TAG, "opus 16 kHz mono %d ms c%d vbr dtx=off, pkt_max %d B, ring %d B (%d frames), "
                  "preroll %d ms", POCKET_FRAME_MS, POCKET_OPUS_COMPLEXITY, s_pkt_max, RING_BYTES,
             RING_FRAMES, CONFIG_POCKET_PREROLL_MS);
    return ESP_OK;
}
