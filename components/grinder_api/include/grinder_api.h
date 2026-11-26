// SPDX-License-Identifier: AGPL-3.0-or-later
// The machine the E64 WS sees: /api/v2/* on port 80, plain HTTP, served
// from the same httpd as the status page. Holds the one machine_t, ticks it,
// and posts its events to a queue. See gbs-anywhere src/server/grinder_api.rs.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "machine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    machine_phase_t phase;
    uint32_t running_ms;         // Running: time since the start
    bool requested_by_grinder;   // Running: knob press (else a flush)
    shot_result_t result;        // Finishing
    bool aborted;                // Finishing
    uint32_t finishing_left_ms;  // Finishing
    bool has_recipe;
    double recipe_g;  // last brewratio SYNC_BEVERAGE_WEIGHT
    char filter[65];  // last brewratio SYNC_FILTER
    bool has_last_shot;
    shot_result_t last_shot;
    int64_t last_shot_age_ms;  // since it was reported, -1 = not this boot
    grind_blocked_t grind_blocked;  // what the grinder would make of the current reply
    char grinder_ip[16];            // "" until the grinder's first request
    int64_t grinder_age_ms;         // since its last request, -1 = never
    uint32_t polls;                 // GET /api/v2/mako count
    uint32_t requests;              // all /api/v2 requests from anyone
    uint32_t refused;               // browser headers / oversize bodies
    uint32_t dropped;               // machine events lost (queue full)
    uint32_t finishing_hold_ms;
    uint32_t shot_window_ms;
    uint32_t seq;  // changes on every machine event and grinder request
} grinder_api_status_t;

// Registers the catch-all handler on `server` (call it after every other
// route, so those win) and starts the 0.5 s tick. Machine events go to
// `events` (machine_event_t, may be NULL); when full they are dropped.
esp_err_t grinder_api_start(httpd_handle_t server, QueueHandle_t events);

void grinder_api_get_status(grinder_api_status_t *out);

// The measured numbers, typed on the web page (M1) or from the detector (M3).
// weight_g <= 0 means none: the recipe weight is used.
machine_err_t grinder_api_report_shot(uint32_t time_ms, double weight_g);
machine_err_t grinder_api_abort(void);

#ifdef __cplusplus
}
#endif
