// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of gbs-anywhere src/integration/half_decent/shot.rs. Line numbers
// below refer to that file. Times are int64 ms; u64 saturating_sub becomes
// sat_sub (never below 0).
#include "shot.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// shot.rs:63-100, copied as they are.
#define SETTLE_MS 500          // :64
#define SETTLE_SPREAD_G 0.3    // :66
#define STEP_MS 300            // :68
#define STEP_G 10.0            // :69
#define BELOW_BASELINE_G 1.0   // :71
#define AT_BASELINE_G 0.3      // :73
#define DROPS_G 1.0            // :75
#define DROPS_HOLD_MS 1000     // :77
#define DROPS_RESET_G 0.5      // :78
#define STOP_WINDOW_MS 3000    // :80
#define STOP_RISE_G 0.5        // :81
#define MIN_SHOT_G 5.0         // :84
#define RECIPE_SHARE 0.5       // :85
#define FLOW_WINDOW_MS 1000    // :87
#define FLOW_RISE_G 0.6        // :88
#define LIFT_GUARD_MS 800      // :91
#define MIN_SHOT_MS 5000       // :93
#define KEEP_MS (STOP_WINDOW_MS + 2000)  // :95
#define TARE_ZERO_G 0.5        // :98
#define TARE_MAX_MS 3000       // :100

static int64_t sat_sub(int64_t a, int64_t b) { return a > b ? a - b : 0; }

// integration/mod.rs:318 tenths
static double tenths(double x) { return round(x * 10.0) / 10.0; }

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

// ---- history ring

static const shot_reading_t *hist_at(const shot_detector_t *d, size_t i) {
    return &d->hist[(d->hist_head + i) % SHOT_KEEP_MAX];
}

static void hist_push(shot_detector_t *d, shot_reading_t r) {
    if (d->hist_len == SHOT_KEEP_MAX) {  // trim() pops it next anyway
        d->hist_head = (d->hist_head + 1) % SHOT_KEEP_MAX;
        d->hist_len--;
    }
    d->hist[(d->hist_head + d->hist_len) % SHOT_KEEP_MAX] = r;
    d->hist_len++;
}

static void hist_pop_front(shot_detector_t *d) {
    d->hist_head = (d->hist_head + 1) % SHOT_KEEP_MAX;
    d->hist_len--;
}

// :340 grams_at: the last filtered reading at or before `ms`.
static bool grams_at(const shot_detector_t *d, int64_t ms, double *out) {
    for (size_t i = d->hist_len; i-- > 0;) {
        const shot_reading_t *r = hist_at(d, i);
        if (r->ms <= ms) {
            *out = r->grams;
            return true;
        }
    }
    return false;
}

// :349 trim: drops history nobody will look at again.
static void trim(shot_detector_t *d, int64_t now_ms) {
    int64_t keep_from = d->state == ST_FLOWING ? sat_sub(d->start_ms, FLOW_WINDOW_MS) : sat_sub(now_ms, KEEP_MS);
    while (d->hist_len > SHOT_KEEP_MAX || (d->hist_len && hist_at(d, 0)->ms < keep_from)) hist_pop_front(d);
}

// :320 settled: the median of the last SETTLE_MS, if everything in it is
// within SETTLE_SPREAD_G of it.
static bool settled(shot_detector_t *d, int64_t now_ms, double *out) {
    if (now_ms < SETTLE_MS) return false;  // checked_sub
    int64_t from = now_ms - SETTLE_MS;
    if (d->hist_len == 0 || hist_at(d, 0)->ms > from) return false;
    size_t n = 0;
    for (size_t i = 0; i < d->hist_len; i++)
        if (hist_at(d, i)->ms >= from) d->scratch[n++] = hist_at(d, i)->grams;
    qsort(d->scratch, n, sizeof(double), cmp_double);
    double median = d->scratch[n / 2];
    for (size_t i = 0; i < n; i++)
        if (fabs(d->scratch[i] - median) > SETTLE_SPREAD_G) return false;
    *out = median;
    return true;
}

// ---- detector

static shot_progress_t prog(shot_progress_kind_t k) {
    shot_progress_t p = {.kind = k};
    return p;
}

static shot_progress_t flowing(double g) {
    shot_progress_t p = {.kind = SHOT_FLOWING, .grams = g};
    return p;
}

void shot_init(shot_detector_t *d) {
    // Everything but the two big arrays, which need no clearing.
    memset(d, 0, offsetof(shot_detector_t, hist));
    d->hist_head = d->hist_len = 0;
    memset(&d->state, 0, sizeof *d - offsetof(shot_detector_t, state));
    d->state = ST_SETTLING;
    d->min_grams = MIN_SHOT_G;
}

void shot_init_after_tare(shot_detector_t *d) {
    shot_init(d);
    d->tare_wait = true;  // :166
}

void shot_expecting(shot_detector_t *d, bool has_recipe, double recipe_g) {
    // :153
    double share = (has_recipe && isfinite(recipe_g) ? recipe_g : 0.0) * RECIPE_SHARE;
    d->min_grams = share > MIN_SHOT_G ? share : MIN_SHOT_G;
}

// :287 finish: ends the shot with what was seen up to `until_ms`.
static shot_progress_t finish(shot_detector_t *d, double baseline, int64_t start_ms, int64_t until_ms) {
    int64_t end_ms = until_ms;
    for (size_t i = d->hist_len; i-- > 0;) {
        const shot_reading_t *r = hist_at(d, i);
        if (r->ms > until_ms || r->ms < start_ms + FLOW_WINDOW_MS) continue;
        double old;
        if (grams_at(d, r->ms - FLOW_WINDOW_MS, &old) && r->grams - old >= FLOW_RISE_G) {
            end_ms = r->ms - FLOW_WINDOW_MS / 2;
            break;
        }
    }
    size_t n = 0;
    for (size_t i = 0; i < d->hist_len; i++) {
        const shot_reading_t *r = hist_at(d, i);
        if (r->ms <= until_ms && r->ms + FLOW_WINDOW_MS >= until_ms) d->scratch[n++] = r->grams;
    }
    qsort(d->scratch, n, sizeof(double), cmp_double);
    double in_cup = n ? d->scratch[n / 2] - baseline : 0.0;
    int64_t time_ms = sat_sub(end_ms, start_ms);
    if (time_ms < MIN_SHOT_MS || in_cup < MIN_SHOT_G) {
        // :308 a pause, not the end (or a lift too early to be a shot): keep
        // the start and watch on.
        return flowing(in_cup);
    }
    d->state = ST_DONE;
    shot_progress_t p = {.kind = SHOT_DONE};
    p.shot.time_ms = (uint32_t)time_ms;
    p.shot.grams = tenths(in_cup);
    p.shot.start_ms = start_ms;
    p.shot.end_ms = end_ms;
    return p;
}

// :204 step
static shot_progress_t step(shot_detector_t *d, shot_reading_t s) {
    double old;
    bool has_change = grams_at(d, sat_sub(s.ms, STEP_MS), &old);
    double change = has_change ? s.grams - old : 0.0;

    switch (d->state) {
        case ST_SETTLING:
        case ST_DONE: {
            double baseline;
            if (!settled(d, s.ms, &baseline)) return prog(SHOT_SETTLING);
            d->state = ST_READY;
            d->baseline = baseline;
            d->at_baseline_ms = s.ms;
            d->above = false;
            return prog(SHOT_READY);
        }
        case ST_READY: {
            double baseline = d->baseline;
            if ((has_change && fabs(change) > STEP_G) || s.grams < baseline - BELOW_BASELINE_G) {
                d->state = ST_SETTLING;
                return prog(SHOT_SETTLING);
            }
            if (s.grams <= baseline + AT_BASELINE_G) d->at_baseline_ms = s.ms;
            if (s.grams < baseline + DROPS_RESET_G) {
                d->above = false;
            } else if (s.grams >= baseline + DROPS_G) {
                if (!d->above) {
                    d->above = true;
                    d->above_since = s.ms;
                }
                if (s.ms - d->above_since >= DROPS_HOLD_MS) {
                    d->state = ST_FLOWING;
                    d->start_ms = d->at_baseline_ms;
                    return flowing(s.grams - baseline);
                }
            }
            return prog(SHOT_READY);
        }
        case ST_FLOWING: {
            double baseline = d->baseline;
            int64_t start_ms = d->start_ms;
            if (has_change && change > STEP_G) {
                // :256 a cup arrived: what looked like drops was not coffee.
                d->state = ST_SETTLING;
                return prog(SHOT_SETTLING);
            }
            if (has_change && change < -STEP_G) return finish(d, baseline, start_ms, sat_sub(s.ms, LIFT_GUARD_MS));
            if (s.grams < baseline + DROPS_RESET_G) {
                // :264 back to the baseline: it was a press, not coffee.
                d->state = ST_READY;
                d->at_baseline_ms = s.ms;
                d->above = false;
                return prog(SHOT_READY);
            }
            double then;
            bool stopped = s.grams - baseline >= d->min_grams && s.ms >= start_ms + STOP_WINDOW_MS &&
                           grams_at(d, s.ms - STOP_WINDOW_MS, &then) && s.grams - then < STOP_RISE_G;
            if (stopped) return finish(d, baseline, start_ms, s.ms);
            return flowing(s.grams - baseline);
        }
    }
    return prog(SHOT_SETTLING);
}

// :174 push
shot_progress_t shot_push(shot_detector_t *d, shot_reading_t r) {
    bool restarted = d->raw_len && r.ms <= d->raw[d->raw_len - 1].ms;
    if (restarted || d->state == ST_DONE) {
        double keep = d->min_grams;  // Self::new().with_min_grams(..)
        shot_init(d);
        d->min_grams = keep;
    }
    if (d->tare_wait) {
        if (!d->tare_first_set) {
            d->tare_first_set = true;
            d->tare_first_ms = r.ms;
        }
        if (fabs(r.grams) > TARE_ZERO_G && r.ms < d->tare_first_ms + TARE_MAX_MS) return prog(SHOT_SETTLING);
        d->tare_wait = false;
    }
    if (d->raw_len == 3) {
        memmove(&d->raw[0], &d->raw[1], 2 * sizeof d->raw[0]);
        d->raw_len = 2;
    }
    d->raw[d->raw_len++] = r;
    if (d->raw_len < 3) return prog(SHOT_SETTLING);

    double g[3] = {d->raw[0].grams, d->raw[1].grams, d->raw[2].grams};
    qsort(g, 3, sizeof(double), cmp_double);
    shot_reading_t s = {.ms = d->raw[1].ms, .grams = g[1]};
    hist_push(d, s);
    trim(d, s.ms);
    shot_progress_t p = step(d, s);
    p.ms = s.ms;
    if (p.kind == SHOT_FLOWING) p.start_ms = d->start_ms;
    return p;
}

const char *shot_progress_name(shot_progress_kind_t k) {
    switch (k) {
        case SHOT_SETTLING: return "settling";
        case SHOT_READY: return "ready";
        case SHOT_FLOWING: return "flowing";
        case SHOT_DONE: return "done";
    }
    return "?";
}
