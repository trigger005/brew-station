// SPDX-License-Identifier: AGPL-3.0-or-later
#include "shot_decide.h"

#include <math.h>

// integration/mod.rs:318 tenths, as the detector rounds its weight.
static double tenths(double x) { return round(x * 10.0) / 10.0; }

static uint32_t since(int64_t to, int64_t from) { return to > from ? (uint32_t)(to - from) : 0; }

shot_decision_t shot_decide(const shot_progress_t *p, const shot_decide_cfg_t *c) {
    shot_decision_t d = {SHOT_REPORT_NONE, 0, 0.0};
    if (p->kind == SHOT_FLOWING && c->target && p->grams >= c->target_g) {
        // Time to the filtered reading that reached it (p->ms), not the newest:
        // the median of three lags one reading.
        d.kind = SHOT_REPORT_TARGET;
        d.time_ms = c->timing_mode == 3 ? since(p->ms, p->start_ms) + c->preinfusion_ms : since(p->ms, c->knob_ms);
        d.grams = tenths(p->grams);
        return d;
    }
    if (p->kind == SHOT_DONE) {
        const shot_t *s = &p->shot;
        d.kind = c->target ? SHOT_REPORT_END_BEFORE_TARGET : SHOT_REPORT_END;
        if (c->timing_mode == 3) d.time_ms = s->time_ms + c->preinfusion_ms;
        else d.time_ms = s->end_ms > c->knob_ms ? (uint32_t)(s->end_ms - c->knob_ms) : s->time_ms;
        d.grams = s->grams;
    }
    return d;
}

int64_t shot_monotonic_ms(int64_t *last_ms, int64_t t_ms) {
    if (t_ms <= *last_ms) t_ms = *last_ms + 1;
    *last_ms = t_ms;
    return t_ms;
}
