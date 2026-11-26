// SPDX-License-Identifier: AGPL-3.0-or-later
// Finds the shot in the scale's readings: from the first drops to the flow
// stopping, and the weight in the cup. Pure C, no I/O and no clock: each
// reading carries its own time, so it runs the same on the host in tests.
//
// A port of gbs-anywhere src/integration/half_decent/shot.rs (Detector).
// Every reading goes through a median of three, then:
//  1. Settling: half a second of steady weight is the baseline.
//  2. Ready: a jump of over 10 g within 0.3 s is a cup placed or lifted:
//     settle again. First drops: 1 g over the baseline, held for 1 s; the
//     shot starts at the last reading still at the baseline.
//  3. Flowing: the flow stopped once the weight rose under 0.5 g in 3 s,
//     with at least 5 g (or half the recipe weight) in the cup. A drop of
//     over 10 g is the cup lifted: the shot ends there.
// A stop under 5 s or 5 g is a pause, not the end.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// shot.rs:96 KEEP_MAX: history kept at most (10 min at 10 Hz).
#define SHOT_KEEP_MAX 6000

typedef struct {
    int64_t ms;
    double grams;
} shot_reading_t;

typedef struct {
    uint32_t time_ms;  // first drops to flow stop
    double grams;      // in the cup, rounded to 0.1 g
    int64_t start_ms;  // the last reading still at the baseline
    int64_t end_ms;    // middle of the last second the weight still rose
} shot_t;

typedef enum {
    SHOT_SETTLING,  // waiting for a steady weight to measure from
    SHOT_READY,     // steady; waiting for the first drops
    SHOT_FLOWING,   // coffee is flowing: .grams in the cup so far
    SHOT_DONE,      // .shot; the next reading starts over
} shot_progress_kind_t;

typedef struct {
    shot_progress_kind_t kind;
    double grams;      // SHOT_FLOWING
    int64_t ms;        // time of the filtered reading this is about (ours,
                       // not in shot.rs: the median lags the latest reading)
    int64_t start_ms;  // SHOT_FLOWING: where the shot starts (ours)
    shot_t shot;       // SHOT_DONE
} shot_progress_t;

typedef enum { ST_SETTLING, ST_READY, ST_FLOWING, ST_DONE } shot_state_t;

// Big (history of SHOT_KEEP_MAX readings, ~140 KB): allocate it, don't put
// it on a stack.
typedef struct {
    shot_reading_t raw[3];  // the last three as they came, for the median
    size_t raw_len;
    shot_reading_t hist[SHOT_KEEP_MAX];  // median-filtered, ring, oldest first
    size_t hist_head, hist_len;
    double scratch[SHOT_KEEP_MAX];  // for medians
    shot_state_t state;
    double baseline;
    int64_t at_baseline_ms;  // Ready
    bool above;              // Ready: above_since is set
    int64_t above_since;     // Ready
    int64_t start_ms;        // Flowing
    bool tare_wait;          // waiting for a tare to land
    bool tare_first_set;
    int64_t tare_first_ms;
    double min_grams;  // the flow can only stop with this much in the cup
} shot_detector_t;

// Detector::new
void shot_init(shot_detector_t *d);
// Detector::after_tare: readings count only from the first one near zero
// (at most 3 s after the first reading).
void shot_init_after_tare(shot_detector_t *d);
// Detector::expecting: the flow stops only once half the recipe is in the cup.
void shot_expecting(shot_detector_t *d, bool has_recipe, double recipe_g);

// Detector::push: takes the next reading.
shot_progress_t shot_push(shot_detector_t *d, shot_reading_t r);

const char *shot_progress_name(shot_progress_kind_t k);

#ifdef __cplusplus
}
#endif
