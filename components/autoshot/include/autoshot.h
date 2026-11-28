// SPDX-License-Identifier: AGPL-3.0-or-later
// Shot reporting (M3): runs the shot/ detector on the AKU's readings while
// the grinder waits for a shot, and reports to the machine by itself.
// Steps 3-4 of a shot (README: "A shot"). Three report modes, set from the page
// and kept in NVS:
//   auto:   when the flow stops (time to the end of flow, final weight)
//   target: as soon as the cup holds the recipe weight, even while it still
//           runs; the rest of the pour is ignored. Without a recipe weight it
//           works as auto, and a flow that stops short is reported as auto.
//   manual: nothing by itself; the page's Send result only.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "shot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REPORT_AUTO = 0,
    REPORT_TARGET = 1,
    REPORT_MANUAL = 2,
} report_mode_t;

typedef struct {
    report_mode_t mode;      // as set
    report_mode_t watching;  // armed: what this shot does (target -> auto without a recipe)
    double target_g;         // armed in target mode
    int timing_mode;         // 1 or 3 (menuconfig)
    bool armed;              // watching a shot now
    shot_progress_kind_t progress;
    double grams;  // in the cup so far (flowing)
    bool has_last;
    uint32_t last_time_ms;  // what was reported
    double last_grams;
    report_mode_t last_by;  // which mode reported it
    bool last_from_scale;   // last_time_ms is the scale's timer (else the board clock)
    uint32_t seq;           // changes when anything above does
} autoshot_status_t;

esp_err_t autoshot_start(void);

// Knob press: watch for the shot (not in manual). `knob_ms` on the esp_timer
// clock; `tared` when a tare went out right before, so the jump to zero is skipped.
void autoshot_arm(int64_t knob_ms, bool tared);
// The shot was reported (by hand or by us), aborted or is over.
void autoshot_disarm(void);
// Every weight reading, `t_ms` when it arrived (esp_timer ms).
void autoshot_weight(int64_t t_ms, double grams);
// Every timer frame from the scale (whole seconds), `t_ms` when it arrived.
void autoshot_scale_timer(int64_t t_ms, uint32_t secs);

// ESP_ERR_INVALID_STATE while a shot is watched; saved to NVS.
esp_err_t autoshot_set_mode(report_mode_t mode);
const char *report_mode_code(report_mode_t m);  // "auto" | "target" | "manual"
bool report_mode_parse(const char *s, report_mode_t *out);

void autoshot_get_status(autoshot_status_t *out);

#ifdef __cplusplus
}
#endif
