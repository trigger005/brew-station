// SPDX-License-Identifier: AGPL-3.0-or-later
#include "scale_clock.h"

#include <string.h>

void scale_clock_reset(scale_clock_t *c) { memset(c, 0, sizeof *c); }

void scale_clock_frame(scale_clock_t *c, int64_t t_ms, uint32_t secs) {
    int64_t lo = t_ms - (int64_t)(secs + 1) * 1000, hi = t_ms - (int64_t)secs * 1000;
    // 0 s says nothing about when it will start; a lower value than before
    // means the timer was reset.
    if (secs == 0 || (c->valid && secs < c->last_s)) {
        scale_clock_reset(c);
        c->last_frame_ms = t_ms;
        return;
    }
    if (c->valid) {
        if (lo > c->lo_ms) c->lo_ms = lo;
        if (hi < c->hi_ms) c->hi_ms = hi;
        // Frames that can't all be true (a stop and start, a delayed frame):
        // trust the newest one alone.
        if (c->lo_ms >= c->hi_ms) {
            c->lo_ms = lo;
            c->hi_ms = hi;
        }
    } else {
        c->lo_ms = lo;
        c->hi_ms = hi;
        c->valid = true;
    }
    c->last_s = secs;
    c->last_frame_ms = t_ms;
}

bool scale_clock_at(const scale_clock_t *c, int64_t t_ms, int64_t max_gap_ms, uint32_t *secs) {
    if (!c->valid || t_ms < c->hi_ms) return false;
    if (t_ms - c->last_frame_ms > max_gap_ms) return false;
    *secs = (uint32_t)((t_ms - c->hi_ms) / 1000);
    return true;
}
