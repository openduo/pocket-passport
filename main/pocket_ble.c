// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

#include "pocket_ble.h"

#include "pocket_config.h"
#include "pocket_proto.h"
#include "pocket_stats.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "pocket_ble";

// ESP-IDF's NimBLE store backend initializer (not in a public header).
void ble_store_config_init(void);

// 8bd9000x-86c5-454b-b65b-3f16cffb662f, little-endian.
#define POCKET_UUID(x) BLE_UUID128_INIT(0x2f, 0x66, 0xfb, 0xcf, 0x16, 0x3f, 0x5b, 0xb6, \
                                        0x4b, 0x45, 0xc5, 0x86, (x), 0x00, 0xd9, 0x8b)
static const ble_uuid128_t s_svc_uuid = POCKET_UUID(0x01);
static const ble_uuid128_t s_tx_uuid = POCKET_UUID(0x02);
static const ble_uuid128_t s_rx_uuid = POCKET_UUID(0x03);

// Advertising intervals, units of 0.625 ms. Basis: Apple Accessory Design
// Guidelines (Bluetooth LE, "Advertising Interval"): advertise at 20 ms for
// at least 30 s after a disconnect, then use one of the listed longer
// intervals. 1022.5 ms is one of them; the choice among them is pending
// power data.
#define ADV_FAST_ITVL      32      // 20 ms
#define ADV_FAST_MS        30000
#define ADV_SLOW_ITVL      1636    // 1022.5 ms
// Connection parameters the device requests (L2CAP connection parameter
// update; the phone decides). Basis: Apple Accessory Design Guidelines
// (Bluetooth LE, "Connection Parameters"): interval min >= 15 ms, max >= min
// + 15 ms, peripheral latency <= 30, supervision timeout 2-6 s, interval max
// x (latency + 1) <= 2 s, and interval max x (latency + 1) x 3 < timeout.
// Responsive (lit screen, press, reply wait): 15-30 ms (iOS picked 30 ms with
// no request), no latency, the 2 s minimum timeout. Idle (dark and quiet):
// the same interval, latency 15, so the radio wakes every (15 + 1) x 30 ms =
// 480 ms instead of every 30 ms while a phone message waits at most ~0.5 s
// longer; timeout 4 s (> 3 x 480 ms with margin; the guideline caps it at 6 s).
// Gain pending a battery measurement.
#define CONN_ITVL_MIN      12      // 15 ms, units of 1.25 ms
#define CONN_ITVL_MAX      24      // 30 ms
#define CONN_LATENCY_IDLE  15
#define CONN_TIMEOUT_ACTIVE 200    // 2 s, units of 10 ms
#define CONN_TIMEOUT_IDLE  400     // 4 s

// Retry delay when advertising fails to start. Basis: the slow advertising
// interval rounded up; a phone waiting to reconnect sees no longer gap than
// it does between slow advertisements.
#define ADV_RETRY_MS       1100

// Largest ATT value: BLE_ATT_MTU_MAX (527) minus the ATT header. A PDU, its
// own 4-byte header included, travels in one ATT value either way.
#define RX_PDU_MAX         (BLE_ATT_MTU_MAX - POCKET_ATT_HEADER_BYTES)
// Outbound message: [connection generation u8][type u8][payload]. Basis: the
// largest payload is AUDIO, [press_id u16][seq u16] plus one Opus packet.
#define TX_MSG_MAX         (2 + 4 + POCKET_OPUS_MAX_PACKET)

typedef struct {
    uint16_t conn;
    bool encrypted;
    bool authenticated;
    bool bonded_link;
    uint8_t key_size;
    bool subscribed;
    bool store_bonded;
    bool pairing_allowed;   // decided at pairing start
    uint8_t gen;            // bumps on connect and disconnect
    uint16_t mtu;
} link_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static link_t s_link = { .conn = BLE_HS_CONN_HANDLE_NONE };
static pocket_link_state_t s_reported;
static bool s_reported_valid;
static pocket_ble_callbacks_t s_cb;
static pocket_ble_stats_t s_stats;
static uint16_t s_tx_handle;
static uint8_t s_addr_type;
static char s_name[24];             // "DuoDuo Pocket XXXX" (18 chars) + NUL

static MessageBufferHandle_t s_txbuf;
static SemaphoreHandle_t s_tx_mutex;
static TaskHandle_t s_tx_task;

static pocket_reasm_t s_reasm;
static uint8_t s_reasm_buf[POCKET_MAX_MSG_BYTES];
static uint8_t s_rx_pdu[RX_PDU_MAX];
// Given while nobody holds s_reasm_buf: the host task takes it before writing
// into the buffer, and the app gives it back after reading a message it kept
// (pocket_ble_callbacks_t.on_message).
static SemaphoreHandle_t s_rx_free;

static struct ble_npl_event s_ev_pair_reply;
static struct ble_npl_event s_ev_delete;
static struct ble_npl_event s_ev_secure_timeout;
static struct ble_npl_event s_ev_adv;
static struct ble_npl_event s_ev_params;
static volatile bool s_idle_want;   // app's request (pocket_ble_set_link_idle)
static bool s_idle_asked;           // host task: what was last requested
static bool s_params_pending;       // host task: an update is in progress
static struct ble_npl_callout s_adv_retry;
static bool s_adv_fast;
static esp_timer_handle_t s_secure_timer;
static volatile bool s_pair_accept;
static volatile uint16_t s_pair_conn = BLE_HS_CONN_HANDLE_NONE;

static int gap_event(struct ble_gap_event *event, void *arg);
static void advertise(bool fast);

// The TX subscription is never persisted for the bond. A restored CCCD
// subscribes the phone as soon as encryption is up, before iOS has
// rediscovered services and re-subscribed, so INFO and STATUS sent then are
// dropped by CoreBluetooth. NimBLE also raises no event when the phone later
// writes the value it already has, so the link would never learn that the
// phone is listening. Reading the stored TX CCCD as "off" and not storing it
// makes the phone's CCCD write on each connection the moment the link is
// ready. Other CCCDs (Service Changed) keep the default persistence.
static int store_read(int obj_type, const union ble_store_key *key,
                      union ble_store_value *value)
{
    int rc = ble_store_config_read(obj_type, key, value);
    if (rc == 0 && obj_type == BLE_STORE_OBJ_TYPE_CCCD &&
        value->cccd.chr_val_handle == s_tx_handle) {
        value->cccd.flags = 0;
        value->cccd.value_changed = 0;
    }
    return rc;
}

static int store_write(int obj_type, const union ble_store_value *value)
{
    if (obj_type == BLE_STORE_OBJ_TYPE_CCCD && value->cccd.chr_val_handle == s_tx_handle) {
        return 0;
    }
    return ble_store_config_write(obj_type, value);
}

static bool store_has_bond(void)
{
    int n = 0;
    if (ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &n) != 0) return false;
    return n > 0;
}

// Ready: encrypted with a bonded, authenticated (MITM) 128-bit key (16
// bytes, the LE Secure Connections key size) and the phone subscribed to TX.
static bool link_ready(const link_t *l)
{
    return l->conn != BLE_HS_CONN_HANDLE_NONE && l->encrypted && l->authenticated &&
           l->bonded_link && l->key_size == 16 && l->subscribed;
}

// Host task only. Reports the link to the app when it changes.
static void report_link(void)
{
    pocket_link_state_t now;
    const bool bonded = store_has_bond();  // NVS access: outside the spinlock
    portENTER_CRITICAL(&s_lock);
    s_link.store_bonded = bonded;
    now.bonded = bonded;
    now.connected = s_link.conn != BLE_HS_CONN_HANDLE_NONE;
    now.ready = link_ready(&s_link);
    portEXIT_CRITICAL(&s_lock);
    if (s_reported_valid && memcmp(&now, &s_reported, sizeof now) == 0) return;
    s_reported = now;
    s_reported_valid = true;
    ESP_LOGI(TAG, "link bonded=%d connected=%d ready=%d", now.bonded, now.connected, now.ready);
    if (s_cb.on_link) s_cb.on_link(&now);
}

static void refresh_security(uint16_t conn)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) != 0) return;
    portENTER_CRITICAL(&s_lock);
    if (s_link.conn == conn) {
        s_link.encrypted = d.sec_state.encrypted;
        s_link.authenticated = d.sec_state.authenticated;
        s_link.bonded_link = d.sec_state.bonded;
        s_link.key_size = d.sec_state.key_size;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void log_conn_params(const char *why, uint16_t handle)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(handle, &d) != 0) return;
    ESP_LOGI(TAG, "conn_params %s itvl_us=%u latency=%u timeout_ms=%u", why,
             (unsigned)d.conn_itvl * 1250U, (unsigned)d.conn_latency,
             (unsigned)d.supervision_timeout * 10U);
}

// --- GATT -------------------------------------------------------------------

static void rx_pdu(size_t got);

static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    bool ready;
    portENTER_CRITICAL(&s_lock);
    ready = s_link.conn == conn && link_ready(&s_link);
    portEXIT_CRITICAL(&s_lock);
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof s_rx_pdu) {
        s_stats.rx_errors++;
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint16_t got = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, s_rx_pdu, sizeof s_rx_pdu, &got) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    // The characteristic already requires encryption; until the phone has
    // subscribed (link ready) its messages have no screen to land on.
    if (!ready) return 0;
    rx_pdu(got);
    return 0;
}

// Host task: one received PDU in s_rx_pdu.
static void rx_pdu(size_t got)
{
    // A small whole message is delivered from the PDU (the app copies it at
    // once), so REPLY_DONE, WORK and the like never wait behind a reply the
    // app still holds. Larger ones, single-PDU or not, go through the
    // reassembly buffer: the app may keep that one, never the PDU buffer.
    uint8_t type;
    const uint8_t *payload;
    size_t plen;
    if (pocket_pdu_inline(&s_reasm, s_rx_pdu, got, &type, &payload, &plen)) {
        s_stats.rx_msgs++;
        if (s_cb.on_message && s_cb.on_message(type, payload, plen, false)) {
            // Only the reassembly buffer may be kept; the PDU buffer is
            // reused by the next write.
            ESP_LOGE(TAG, "on_message kept a single-PDU message");
        }
        return;
    }
    xSemaphoreTake(s_rx_free, portMAX_DELAY);
    pocket_reasm_result_t r = pocket_reasm_push(&s_reasm, s_rx_pdu, got);
    s_stats.rx_errors = s_reasm.errors;
    bool kept = false;
    if (r == POCKET_REASM_DONE) {
        s_stats.rx_msgs++;
        if (s_cb.on_message) {
            kept = s_cb.on_message(s_reasm.type, s_reasm.buf, s_reasm.len, s_reasm.truncated);
        }
    }
    if (!kept) xSemaphoreGive(s_rx_free);
}

void pocket_ble_rx_release(void)
{
    xSemaphoreGive(s_rx_free);
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC |
                         BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN,
                .val_handle = &s_tx_handle,
            },
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC |
                         BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            { 0 },
        },
    },
    { 0 },
};

// --- TX -----------------------------------------------------------------------

bool pocket_ble_send(uint8_t type, const uint8_t *payload, size_t len)
{
    static uint8_t msg[TX_MSG_MAX];
    if (!s_txbuf || len + 2 > sizeof msg) {
        s_stats.tx_dropped++;
        return false;
    }
    uint8_t gen;
    bool ready;
    portENTER_CRITICAL(&s_lock);
    gen = s_link.gen;
    ready = link_ready(&s_link);
    portEXIT_CRITICAL(&s_lock);
    if (!ready) {
        s_stats.tx_dropped++;
        return false;
    }
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    msg[0] = gen;
    msg[1] = type;
    if (len) memcpy(msg + 2, payload, len);
    size_t sent = xMessageBufferSend(s_txbuf, msg, len + 2, 0);
    size_t free_now = xMessageBufferSpacesAvailable(s_txbuf);
    xSemaphoreGive(s_tx_mutex);
    if (free_now < s_stats.tx_buffer_min) s_stats.tx_buffer_min = (uint32_t)free_now;
    if (sent == 0) {
        s_stats.tx_dropped++;
        return false;
    }
    return true;
}

static bool tx_still_valid(uint8_t gen, uint16_t *conn, uint16_t *mtu)
{
    bool ok;
    portENTER_CRITICAL(&s_lock);
    ok = s_link.gen == gen && link_ready(&s_link);
    *conn = s_link.conn;
    *mtu = s_link.mtu;
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

static void tx_task(void *arg)
{
    (void)arg;
    static uint8_t msg[TX_MSG_MAX];
    static uint8_t pdu[RX_PDU_MAX];
    for (;;) {
        size_t n = xMessageBufferReceive(s_txbuf, msg, sizeof msg, portMAX_DELAY);
        if (n < 2) continue;
        uint16_t conn, mtu;
        if (!tx_still_valid(msg[0], &conn, &mtu)) {
            s_stats.tx_dropped++;
            continue;
        }
        size_t chunk = pocket_pdu_chunk_max(mtu);
        if (chunk > sizeof pdu - POCKET_PDU_HEADER_BYTES) chunk = sizeof pdu - POCKET_PDU_HEADER_BYTES;
        pocket_frag_t f;
        if (!pocket_frag_init(&f, msg[1], msg + 2, n - 2, chunk)) {
            s_stats.tx_errors++;
            continue;
        }
        bool failed = false;
        size_t len;
        while (!failed && (len = pocket_frag_next(&f, pdu, sizeof pdu)) > 0) {
            for (;;) {
                struct os_mbuf *om = ble_hs_mbuf_from_flat(pdu, len);
                int rc = om ? ble_gatts_notify_custom(conn, s_tx_handle, om) : BLE_HS_ENOMEM;
                if (rc == 0) break;
                if (rc != BLE_HS_ENOMEM) {
                    failed = true;
                    break;
                }
                // Out of mbufs: wait for a sent notification to free one.
                s_stats.tx_retries++;
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONFIG_POCKET_TX_RETRY_MS));
                if (!tx_still_valid(msg[0], &conn, &mtu)) {
                    failed = true;
                    break;
                }
            }
        }
        if (failed) s_stats.tx_errors++;
        else s_stats.tx_msgs++;
    }
}

// --- Host-task work items -------------------------------------------------------

static void secure_timer_cb(void *arg)
{
    (void)arg;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_ev_secure_timeout);
}

static void on_secure_timeout(struct ble_npl_event *ev)
{
    (void)ev;
    uint16_t conn;
    bool ready;
    portENTER_CRITICAL(&s_lock);
    conn = s_link.conn;
    ready = link_ready(&s_link);
    portEXIT_CRITICAL(&s_lock);
    if (conn != BLE_HS_CONN_HANDLE_NONE && !ready) {
        ESP_LOGW(TAG, "link not secure after %d ms; disconnecting", POCKET_SECURE_TIMEOUT_MS);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void on_pair_reply(struct ble_npl_event *ev)
{
    (void)ev;
    uint16_t conn = s_pair_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
    struct ble_sm_io io = { .action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = s_pair_accept };
    int rc = ble_sm_inject_io(conn, &io);
    ESP_LOGI(TAG, "numeric comparison %s (rc=%d)", s_pair_accept ? "accepted" : "rejected", rc);
    s_pair_conn = BLE_HS_CONN_HANDLE_NONE;
}

static void on_delete_bonds(struct ble_npl_event *ev)
{
    (void)ev;
    uint16_t conn;
    portENTER_CRITICAL(&s_lock);
    conn = s_link.conn;
    portEXIT_CRITICAL(&s_lock);
    if (conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    int rc = ble_store_clear();
    ESP_LOGI(TAG, "bonds deleted (rc=%d)", rc);
    report_link();
}

void pocket_ble_pair_reply(bool accept)
{
    s_pair_accept = accept;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_ev_pair_reply);
}

void pocket_ble_delete_bonds(void)
{
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_ev_delete);
}

// Host task only. Asks the phone for the parameters the app wants, one
// update at a time; a finished update re-checks.
static void request_params(void)
{
    uint16_t conn;
    bool ready;
    portENTER_CRITICAL(&s_lock);
    conn = s_link.conn;
    ready = link_ready(&s_link);
    portEXIT_CRITICAL(&s_lock);
    const bool idle = s_idle_want;
    if (!ready || s_params_pending || idle == s_idle_asked) return;
    const struct ble_gap_upd_params p = {
        .itvl_min = CONN_ITVL_MIN,
        .itvl_max = CONN_ITVL_MAX,
        .latency = idle ? CONN_LATENCY_IDLE : 0,
        .supervision_timeout = idle ? CONN_TIMEOUT_IDLE : CONN_TIMEOUT_ACTIVE,
    };
    int rc = ble_gap_update_params(conn, &p);
    ESP_LOGI(TAG, "conn_params request %s rc=%d", idle ? "idle" : "active", rc);
    if (rc == 0) {
        s_params_pending = true;
        s_idle_asked = idle;
    }
}

static void on_params(struct ble_npl_event *ev)
{
    (void)ev;
    request_params();
}

void pocket_ble_set_link_idle(bool idle)
{
    s_idle_want = idle;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_ev_params);
}

// --- GAP --------------------------------------------------------------------------

static void advertise(bool fast)
{
    struct ble_hs_adv_fields f = { 0 };
    struct ble_hs_adv_fields rsp = { 0 };
    // iOS background scans are passive and filter by service UUID, so the
    // UUID sits in the primary advertisement; the name goes to the scan
    // response (verified with iOS background scanning).
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &s_svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    rsp.name = (const uint8_t *)s_name;
    rsp.name_len = strlen(s_name);
    rsp.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc == 0) rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc == 0) {
        struct ble_gap_adv_params p = { 0 };
        p.conn_mode = BLE_GAP_CONN_MODE_UND;
        p.disc_mode = BLE_GAP_DISC_MODE_GEN;
        p.itvl_min = fast ? ADV_FAST_ITVL : ADV_SLOW_ITVL;
        p.itvl_max = p.itvl_min;
        rc = ble_gap_adv_start(s_addr_type, NULL, fast ? ADV_FAST_MS : BLE_HS_FOREVER, &p,
                               gap_event, NULL);
    }
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        // Without advertising no phone can ever reconnect, so never give up.
        ESP_LOGE(TAG, "advertising failed: %d; retrying", rc);
        s_adv_fast = fast;
        ble_npl_callout_reset(&s_adv_retry, ble_npl_time_ms_to_ticks32(ADV_RETRY_MS));
    }
}

// Host task only. Starts advertising once the host has released the last
// connection. A link that breaks before NimBLE reported it (for example a
// phone that drops it while encryption fails) arrives as a failed CONNECT
// event while its connection slot is still allocated; starting advertising
// from inside that event fails with BLE_HS_ENOMEM (one connection allowed).
static void on_adv(struct ble_npl_event *ev)
{
    (void)ev;
    if (s_link.conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) return;
    advertise(s_adv_fast);
}

static void advertise_later(bool fast)
{
    s_adv_fast = fast;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_ev_adv);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connection failed: %d", event->connect.status);
            advertise_later(true);
            return 0;
        }
        portENTER_CRITICAL(&s_lock);
        s_link = (link_t){ .conn = event->connect.conn_handle, .gen = (uint8_t)(s_link.gen + 1),
                           .mtu = ble_att_mtu(event->connect.conn_handle) };
        portEXIT_CRITICAL(&s_lock);
        pocket_reasm_reset(&s_reasm);
        // A new connection starts with the phone's parameters.
        s_idle_asked = false;
        s_params_pending = false;
        ESP_LOGI(TAG, "connected handle=%d", event->connect.conn_handle);
        log_conn_params("connect", event->connect.conn_handle);
        esp_timer_stop(s_secure_timer);
        esp_timer_start_once(s_secure_timer, (uint64_t)POCKET_SECURE_TIMEOUT_MS * 1000);
        // Bonded phone: re-encrypt with the stored key. New phone: the
        // security request makes iOS start pairing.
        ble_gap_security_initiate(event->connect.conn_handle);
        report_link();
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        portENTER_CRITICAL(&s_lock);
        s_link.conn = BLE_HS_CONN_HANDLE_NONE;
        s_link.encrypted = s_link.authenticated = s_link.bonded_link = false;
        s_link.subscribed = false;
        s_link.gen++;
        s_link.mtu = 0;
        portEXIT_CRITICAL(&s_lock);
        esp_timer_stop(s_secure_timer);
        pocket_reasm_reset(&s_reasm);
        if (s_pair_conn != BLE_HS_CONN_HANDLE_NONE) {
            s_pair_conn = BLE_HS_CONN_HANDLE_NONE;
            if (s_cb.on_pair_done) s_cb.on_pair_done(false);
        }
        ESP_LOGI(TAG, "disconnected reason=0x%x", event->disconnect.reason);
        report_link();
        advertise_later(true);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_link.conn == BLE_HS_CONN_HANDLE_NONE) advertise(false);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        refresh_security(event->enc_change.conn_handle);
        ESP_LOGI(TAG, "encryption status=%d enc=%d auth=%d bonded=%d key=%u",
                 event->enc_change.status, s_link.encrypted, s_link.authenticated,
                 s_link.bonded_link, (unsigned)s_link.key_size);
        if (s_cb.on_pair_done) s_cb.on_pair_done(event->enc_change.status == 0);
        report_link();
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_handle) {
            portENTER_CRITICAL(&s_lock);
            if (s_link.conn == event->subscribe.conn_handle) {
                s_link.subscribed = event->subscribe.cur_notify != 0;
            }
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "subscribe reason=%u notify=%u", (unsigned)event->subscribe.reason,
                     (unsigned)event->subscribe.cur_notify);
            refresh_security(event->subscribe.conn_handle);
            report_link();
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        portENTER_CRITICAL(&s_lock);
        if (s_link.conn == event->mtu.conn_handle) s_link.mtu = event->mtu.value;
        portEXIT_CRITICAL(&s_lock);
        s_stats.mtu = event->mtu.value;
        ESP_LOGI(TAG, "mtu=%u", (unsigned)event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.status == 0) log_conn_params("update", event->conn_update.conn_handle);
        else ESP_LOGW(TAG, "conn_params update failed: %d", event->conn_update.status);
        s_params_pending = false;
        // A refused request is not repeated until the app's wish changes.
        request_params();
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        if (s_tx_task) xTaskNotifyGive(s_tx_task);
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        uint16_t conn = event->passkey.conn_handle;
        if (event->passkey.params.action != BLE_SM_IOACT_NUMCMP) {
            ESP_LOGW(TAG, "unsupported pairing action %d", event->passkey.params.action);
            return 0;
        }
        bool allowed;
        portENTER_CRITICAL(&s_lock);
        allowed = s_link.pairing_allowed || !s_link.store_bonded;
        portEXIT_CRITICAL(&s_lock);
        if (!allowed) {
            // A bond exists: a new phone needs "re-pair" on the device first.
            ESP_LOGW(TAG, "pairing refused: device already bonded");
            struct ble_sm_io io = { .action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = 0 };
            ble_sm_inject_io(conn, &io);
            return 0;
        }
        s_pair_conn = conn;
        if (s_cb.on_passkey) s_cb.on_passkey(event->passkey.params.numcmp);
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The bonded phone lost its keys (e.g. "Forget This Device") and
        // pairs again: drop the old bond and let the user confirm the new one.
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &d) != 0) {
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        ble_store_util_delete_peer(&d.peer_id_addr);
        portENTER_CRITICAL(&s_lock);
        s_link.pairing_allowed = true;
        portEXIT_CRITICAL(&s_lock);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}

static void on_sync(void)
{
    // Host start re-runs ble_store_config_init(), which resets the store
    // callbacks, so they are installed once the host is up.
    ble_hs_cfg.store_read_cb = store_read;
    ble_hs_cfg.store_write_cb = store_write;
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "address setup failed: %d", rc);
        return;
    }
    report_link();
    advertise(true);
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset: %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

#if POCKET_DEBUG_INJECTION
// Host task only: set around the delivery of an injected message.
static bool s_injecting;

bool pocket_ble_debug_injecting(void)
{
    return s_injecting;
}
#endif

#if CONFIG_POCKET_DEBUG_MEMPROBE
// Text cycled into the injected reply: common CJK covered by pocket_cjk_20.
static const char s_probe_text[] =
    "\xe8\xbf\x99\xe6\x98\xaf\xe5\x86\x85\xe5\xad\x98\xe6\x8e\xa2\xe9\x92\x88"  // zhe shi nei cun tan zhen
    "\xe6\xb3\xa8\xe5\x85\xa5\xe7\x9a\x84\xe9\x95\xbf\xe5\x9b\x9e\xe5\xa4\x8d"  // zhu ru de chang hui fu
    "\xef\xbc\x8c\xe7\x94\xa8\xe6\x9d\xa5\xe6\xb5\x8b\xe9\x87\x8f\xe5\xa0\x86"  // , yong lai ce liang dui
    "\xe5\x92\x8c\xe7\x95\x8c\xe9\x9d\xa2\xe5\x86\x85\xe5\xad\x98\xe3\x80\x82"; // he jie mian nei cun .
static uint32_t s_probe_reply_id;
static size_t s_probe_bytes;
static struct ble_npl_event s_ev_probe;

static void on_probe(struct ble_npl_event *ev)
{
    (void)ev;
    xSemaphoreTake(s_rx_free, portMAX_DELAY);
    uint8_t *b = s_reasm_buf;
    size_t cap = sizeof s_reasm_buf;
    b[0] = (uint8_t)s_probe_reply_id;
    b[1] = (uint8_t)(s_probe_reply_id >> 8);
    b[2] = (uint8_t)(s_probe_reply_id >> 16);
    b[3] = (uint8_t)(s_probe_reply_id >> 24);
    uint8_t type = s_probe_bytes ? POCKET_MSG_REPLY : POCKET_MSG_REPLY_DONE;
    size_t len = 4;
    if (s_probe_bytes) {
        b[4] = 1;  // final
        len = 5;
        const size_t unit = sizeof s_probe_text - 1;
        while (len < cap && len - 5 + unit <= s_probe_bytes && len + unit <= cap) {
            memcpy(b + len, s_probe_text, unit);
            len += unit;
        }
    }
    ESP_LOGI(TAG, "memprobe: inject type 0x%02x reply_id %lu len %u", type,
             (unsigned long)s_probe_reply_id, (unsigned)len);
    s_injecting = true;
    if (!s_cb.on_message || !s_cb.on_message(type, b, len, false)) xSemaphoreGive(s_rx_free);
    s_injecting = false;
}

void pocket_ble_debug_inject_reply(uint32_t reply_id, size_t text_bytes)
{
    s_probe_reply_id = reply_id;
    s_probe_bytes = text_bytes;
    ble_npl_event_init(&s_ev_probe, on_probe, NULL);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_ev_probe);
}
#endif

#if CONFIG_POCKET_DEBUG_FLOWPROBE
// Back-to-back injections, as a phone writes them. The dense burst of the
// flow probe queues 24 PDUs on the host before the first one runs.
#define FLOW_SLOTS 24
static struct ble_npl_event s_ev_flow[FLOW_SLOTS];
static uint8_t s_flow_pdu[FLOW_SLOTS][RX_PDU_MAX];
static size_t s_flow_len[FLOW_SLOTS];
static unsigned s_flow_next;

static void on_flow(struct ble_npl_event *ev)
{
    unsigned i = (unsigned)(uintptr_t)ble_npl_event_get_arg(ev);
    memcpy(s_rx_pdu, s_flow_pdu[i], s_flow_len[i]);
    ESP_LOGI(TAG, "flowprobe: inject type 0x%02x len %u", s_rx_pdu[0],
             (unsigned)s_flow_len[i] - POCKET_PDU_HEADER_BYTES);
    s_injecting = true;
    rx_pdu(s_flow_len[i]);
    s_injecting = false;
}

void pocket_ble_debug_inject_pdu(uint8_t type, bool more, const uint8_t *payload, size_t len)
{
    if (len + POCKET_PDU_HEADER_BYTES > RX_PDU_MAX) return;
    unsigned i = s_flow_next++ % FLOW_SLOTS;
    uint8_t *p = s_flow_pdu[i];
    p[0] = type;
    p[1] = more ? POCKET_PDU_FLAG_MORE : 0;
    p[2] = (uint8_t)len;
    p[3] = (uint8_t)(len >> 8);
    memcpy(p + POCKET_PDU_HEADER_BYTES, payload, len);
    s_flow_len[i] = len + POCKET_PDU_HEADER_BYTES;
    ble_npl_event_init(&s_ev_flow[i], on_flow, (void *)(uintptr_t)i);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_ev_flow[i]);
}

void pocket_ble_debug_inject_msg(uint8_t type, const uint8_t *payload, size_t len)
{
    // One PDU, as a phone write of that size would arrive.
    pocket_ble_debug_inject_pdu(type, false, payload, len);
}
#endif

const char *pocket_ble_name(void)
{
    return s_name;
}

void pocket_ble_stats(pocket_ble_stats_t *out)
{
    *out = s_stats;
    out->tx_buffer_free = s_txbuf ? (uint32_t)xMessageBufferSpacesAvailable(s_txbuf) : 0;
}

esp_err_t pocket_ble_start(const pocket_ble_callbacks_t *cb)
{
    s_cb = *cb;
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof s_name, "DuoDuo Pocket %02X%02X", mac[4], mac[5]);

    pocket_reasm_init(&s_reasm, s_reasm_buf, sizeof s_reasm_buf);
    s_txbuf = xMessageBufferCreate(CONFIG_POCKET_TX_BUFFER_BYTES);
    s_tx_mutex = xSemaphoreCreateMutex();
    s_rx_free = xSemaphoreCreateBinary();
    if (!s_txbuf || !s_tx_mutex || !s_rx_free) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_rx_free);
    s_stats.tx_buffer_min = CONFIG_POCKET_TX_BUFFER_BYTES;

    const esp_timer_create_args_t targs = { .callback = secure_timer_cb, .name = "ble_secure" };
    esp_err_t e = esp_timer_create(&targs, &s_secure_timer);
    if (e != ESP_OK) return e;

    // Bonds and the controller's PHY calibration live in NVS. Never erase it
    // on failure: that would silently drop the pairing.
    e = nvs_flash_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s (not erased)", esp_err_to_name(e));
        return e;
    }
    pocket_stats_heap_mark("ble_pre_port");
    e = nimble_port_init();
    if (e != ESP_OK) return e;
    pocket_stats_heap_mark("nimble_port");

    ble_npl_event_init(&s_ev_pair_reply, on_pair_reply, NULL);
    ble_npl_event_init(&s_ev_delete, on_delete_bonds, NULL);
    ble_npl_event_init(&s_ev_secure_timeout, on_secure_timeout, NULL);
    ble_npl_event_init(&s_ev_adv, on_adv, NULL);
    ble_npl_event_init(&s_ev_params, on_params, NULL);
    ble_npl_callout_init(&s_adv_retry, nimble_port_get_dflt_eventq(), on_adv, NULL);

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_YES_NO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_sc_only = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc == 0) rc = ble_svc_gap_device_name_set(s_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT setup failed: %d", rc);
        return ESP_FAIL;
    }
    ble_store_config_init();
    pocket_stats_heap_mark("ble_gatt");

    if (xTaskCreate(tx_task, "pocket_tx", CONFIG_POCKET_TX_TASK_STACK, NULL,
                    CONFIG_POCKET_PRIO_LINK, &s_tx_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    pocket_stats_heap_mark("ble_tx_task");
    nimble_port_freertos_init(host_task);
    pocket_stats_heap_mark("ble_host_task");
    ESP_LOGI(TAG, "started as \"%s\"", s_name);
    return ESP_OK;
}
