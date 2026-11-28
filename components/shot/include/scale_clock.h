// SPDX-License-Identifier: AGPL-3.0-or-later
// What the scale's own timer showed at a given moment, from its timer frames.
//
// The AKU reports its timer in whole seconds (type 0x87, see aku_frame.c),
// with no time of its own. A frame showing N seconds that arrives at board
// time t says the timer started in (t - (N+1) s, t - N s]. Intersecting that
// window over the frames pins the start down; the display at any board time T
// is then floor((T - start) / 1 s). The latest possible start is used: frames
// arrive a little after the display changed (BLE delay), never before.
//
// Pure, host-tested (test_shot.c). Ours, not from a reference source.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    int64_t lo_ms, hi_ms;  // the timer started in (lo, hi], board clock
    int64_t last_frame_ms;
    uint32_t last_s;
} scale_clock_t;

void scale_clock_reset(scale_clock_t *c);

// A timer frame showing `secs`, arrived at board time `t_ms`.
void scale_clock_frame(scale_clock_t *c, int64_t t_ms, uint32_t secs);

// The whole seconds the scale showed at board time `t_ms`. False when there
// is nothing to go on: no running timer seen (only 0 s frames), or no frame
// within `max_gap_ms` before `t_ms` (the timer stopped or the scale went away).
bool scale_clock_at(const scale_clock_t *c, int64_t t_ms, int64_t max_gap_ms, uint32_t *secs);

#ifdef __cplusplus
}
#endif
