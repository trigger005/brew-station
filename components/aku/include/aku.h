// SPDX-License-Identifier: AGPL-3.0-or-later
// Varia AKU BLE central: scan by name, connect, subscribe to FFF1, parse
// frames, publish timestamped events to a queue, send commands, reconnect.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "aku_frame.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AKU_RAW_MAX 64

typedef enum {
    AKU_EV_CONNECTED,
    AKU_EV_DISCONNECTED,
    AKU_EV_RAW,    // one notification, as received (for recording/replay)
    AKU_EV_FRAME,  // one parsed frame
    AKU_EV_SEARCH, // the search mode changed (search.mode, search.prev)
} aku_ev_kind_t;

// How the board looks for the scale (see CONFIG_CS_SCALE_* in Kconfig).
typedef enum {
    AKU_SEARCH_CONNECTED = 0,  // not searching: the scale is connected
    AKU_SEARCH_FAST,           // non-stop, for a few minutes after it went away or a wake
    AKU_SEARCH_SLOW,           // a short look every so often
    AKU_SEARCH_OFF,            // stopped until aku_wake()
} aku_search_t;

typedef struct {
    aku_ev_kind_t kind;
    int64_t t_us;  // esp_timer_get_time() when the notification arrived
    union {
        struct {
            uint8_t type;  // aku_type_t
            union {
                float weight_g;
                uint8_t battery_pct;
                uint16_t timer_s;
                uint8_t event_arg;
            };
            uint8_t raw_len;  // the whole frame, header and xor included
            uint8_t raw[16];  // (truncated to 16 bytes)
        } frame;
        struct {
            uint8_t len;
            uint8_t bytes[AKU_RAW_MAX];
        } raw;
        struct {
            char name[32];
            int reason;  // disconnect reason
        } conn;
        struct {
            uint8_t mode, prev;  // aku_search_t
        } search;
    };
} aku_event_t;

typedef struct {
    bool connected;
    char name[32];
    char addr[18];
    int rssi;
    float weight_g;
    int64_t weight_t_us;
    int battery_pct;  // -1 = none (not received yet, or no scale)
    int timer_s;      // -1 = none (not received yet, or no scale)
    float weight_rate_hz;          // weight frames per second, last 1 s window
    uint32_t notif_interval_min_ms;  // since connect
    uint32_t notif_interval_max_ms;
    uint32_t notif_interval_avg_ms;
    uint32_t notifications;
    uint32_t weights_per_notif_max;  // most weight frames seen in one notification
    uint32_t connects;
    uint8_t search;          // aku_search_t
    int64_t fast_until_us;   // the fast search runs until then
    int64_t lost_t_us;       // when the scale went away (0 = not connected since boot)
    aku_parse_stats_t stats;
    uint32_t seq;  // increments on every change
} aku_status_t;

// Starts NimBLE and the scan/connect loop. Events go to `queue` (may be NULL);
// it is never blocked on: when full, events are dropped and counted.
esp_err_t aku_start(QueueHandle_t queue);

void aku_get_status(aku_status_t *out);

// Searches non-stop again for CONFIG_CS_SCALE_FAST_SEARCH_MIN (any task).
// For moments the scale is about to be used: a grind, a knob press, the page
// opening, the Connect scale button. True when it sped up a slow or stopped
// search (false when connected or already searching non-stop).
bool aku_wake(void);

const char *aku_search_code(aku_search_t m);  // "connected" | "fast" | "slow" | "off"

// Sends cmd to the scale (write without response).
esp_err_t aku_send(aku_cmd_t cmd);

#ifdef __cplusplus
}
#endif
