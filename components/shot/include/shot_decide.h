// SPDX-License-Identifier: AGPL-3.0-or-later
// What to report for a detector progress, given the report mode: pure, so the
// decisions autoshot.c makes on the device are host-tested (test_shot.c).
// The modes are ours (not from a reference source); see autoshot.h.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "shot.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SHOT_REPORT_NONE = 0,
    SHOT_REPORT_TARGET,         // target mode: the cup reached the target
    SHOT_REPORT_END,            // the flow stopped (auto)
    SHOT_REPORT_END_BEFORE_TARGET,  // target mode, but the flow stopped short
} shot_report_kind_t;

typedef struct {
    shot_report_kind_t kind;
    uint32_t time_ms;
    double grams;
} shot_decision_t;

typedef struct {
    bool target;       // watching for a target weight
    double target_g;
    int64_t knob_ms;   // the grinder's knob press, same clock as the readings
    int timing_mode;   // 1: from the knob press; 3: from the first drops + pre-infusion
    uint32_t preinfusion_ms;
} shot_decide_cfg_t;

shot_decision_t shot_decide(const shot_progress_t *p, const shot_decide_cfg_t *c);

// Readings that share an arrival time (or come back in time) would make the
// detector start over (shot.rs:175). Keeps them strictly increasing.
int64_t shot_monotonic_ms(int64_t *last_ms, int64_t t_ms);

#ifdef __cplusplus
}
#endif
