// Copyright 2026 openduo
// SPDX-License-Identifier: FSL-1.1-Apache-2.0

// NimBLE peripheral for the pocket link (docs/pocket/README.md, "BLE link"):
// one primary service, TX (notify, device -> phone) and RX (write without
// response, phone -> device), both requiring an encrypted, authenticated
// link. LE Secure Connections with numeric comparison confirmed on the
// device, bonding kept in NVS. Pairing is accepted only while no bond exists (or when the bonded
// phone re-pairs); "re-pair" deletes the bond.
//
// UUIDs (fixed; shared with the phone app, see docs/pocket/README.md):
//   service 8bd90001-86c5-454b-b65b-3f16cffb662f
//   TX      8bd90002-86c5-454b-b65b-3f16cffb662f
//   RX      8bd90003-86c5-454b-b65b-3f16cffb662f
#pragma once

#include "esp_err.h"
#include "pocket_model.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Callbacks run in the NimBLE host task: copy what you need and return.
// on_message may instead keep the payload: it then returns true, and the
// payload (the receive buffer) stays valid and untouched until
// pocket_ble_rx_release(). Meanwhile a following message that needs the
// buffer waits for the release in the host task; a message that fits in one
// PDU is delivered from the PDU itself and does not wait.
typedef struct {
    void (*on_link)(const pocket_link_state_t *link);
    void (*on_passkey)(uint32_t passkey);                 // numeric comparison value
    void (*on_pair_done)(bool success);
    bool (*on_message)(uint8_t type, const uint8_t *payload, size_t len, bool truncated);
} pocket_ble_callbacks_t;

typedef struct {
    uint16_t mtu;
    uint32_t tx_msgs;          // messages fully notified
    uint32_t tx_dropped;       // messages refused: link down or buffer full
    uint32_t tx_retries;       // notify retried for lack of mbufs
    uint32_t tx_errors;        // notify failures that dropped a message
    uint32_t rx_msgs;
    uint32_t rx_errors;        // malformed PDUs, cut-off messages
    uint32_t tx_buffer_free;   // bytes free in the outbound buffer
    uint32_t tx_buffer_min;    // all-time minimum of the above
} pocket_ble_stats_t;

esp_err_t pocket_ble_start(const pocket_ble_callbacks_t *cb);

// Queues one message for TX in order. False if the link is not ready or the
// outbound buffer is full (the message is dropped and counted).
bool pocket_ble_send(uint8_t type, const uint8_t *payload, size_t len);

// Returns the receive buffer kept by on_message. Any task.
void pocket_ble_rx_release(void);

// Low-power (true) or responsive (false) connection parameters, requested
// from the phone while the link is ready. Any task.
void pocket_ble_set_link_idle(bool idle);

// Answer the pending numeric comparison.
void pocket_ble_pair_reply(bool accept);

// Delete all bonds and drop the connection; pairing mode follows.
void pocket_ble_delete_bonds(void);

// Device name as advertised ("DuoDuo Pocket XXXX").
const char *pocket_ble_name(void);

void pocket_ble_stats(pocket_ble_stats_t *out);

#if CONFIG_POCKET_DEBUG_FLOWPROBE
// Debug flow probe: delivers phone PDUs through the real receive path (host
// task). Up to 24 may be queued back to back.
void pocket_ble_debug_inject_pdu(uint8_t type, bool more, const uint8_t *payload, size_t len);
// One whole message as a single PDU.
void pocket_ble_debug_inject_msg(uint8_t type, const uint8_t *payload, size_t len);
#endif

#if CONFIG_POCKET_DEBUG_MEMPROBE || CONFIG_POCKET_DEBUG_FLOWPROBE
#define POCKET_DEBUG_INJECTION 1
// True while the host task delivers a probe-injected message; on_message
// reads it so injected replies are never stored in the history partition.
bool pocket_ble_debug_injecting(void);
#else
#define POCKET_DEBUG_INJECTION 0
#endif

#if CONFIG_POCKET_DEBUG_MEMPROBE
// Debug memory probe: delivers a REPLY (or REPLY_DONE when text_bytes is 0)
// through the receive path on the NimBLE host task, as if the phone had sent
// it: the reassembly buffer is filled and on_message runs.
void pocket_ble_debug_inject_reply(uint32_t reply_id, size_t text_bytes);
#endif
