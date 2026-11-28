// SPDX-License-Identifier: AGPL-3.0-or-later
// Unit tests for shot/, ported from the tests in gbs-anywhere
// src/integration/half_decent/shot.rs (mod synth + mod tests). The synth
// readings are 10 Hz like the Half Decent Scale; the *_at_aku_rate tests are
// ours and replay the same shots at the ~3 Hz the AKU sent on hardware.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shot.h"
#include "shot_decide.h"
#include "scale_clock.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                  \
    do {                                                             \
        checks++;                                                    \
        if (!(cond)) {                                               \
            failures++;                                              \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                            \
    } while (0)

static shot_detector_t *D;  // ~140 KB: one, on the heap

// ------------------------------------------------------------------ synth (shot.rs:364-414)

static const double NOISE[6] = {0.0, 0.08, -0.05, 0.1, -0.09, 0.03};

typedef double (*gfn)(double t, const void *ctx);

typedef struct {
    shot_reading_t *r;
    size_t n;
} readings_t;

// `secs` of readings every `step_ms` from `ms0`: grams(t) plus noise.
// shot.rs `readings` is step_ms = 100.
static readings_t readings_every(int64_t ms0, double secs, int step_ms, gfn g, const void *ctx) {
    readings_t out;
    out.n = (size_t)llround(secs * 1000.0 / step_ms);
    out.r = calloc(out.n, sizeof *out.r);
    for (size_t i = 0; i < out.n; i++) {
        double t = (double)(i * step_ms) / 1000.0;
        out.r[i].ms = ms0 + (int64_t)i * step_ms;
        out.r[i].grams = g(t, ctx) + NOISE[i % 6];
    }
    return out;
}

static readings_t readings(int64_t ms0, double secs, gfn g, const void *ctx) {
    return readings_every(ms0, secs, 100, g, ctx);
}

// Empty until 2 s, `rate` g/s for `flow_s`, then 3 s of drips at 0.1 g/s.
static double espresso(double t, double flow_s, double rate) {
    double end = 2.0 + flow_s;
    if (t < 2.0) return 0.0;
    if (t < end) return rate * (t - 2.0);
    return rate * flow_s + 0.1 * fmin(t - end, 3.0);
}

typedef struct {
    double secs, rate;
} phase_t;

typedef struct {
    const phase_t *p;
    size_t n;
} profile_t;

static double profile(double t, const profile_t *pr) {
    double at = 2.0, g = 0.0;
    for (size_t i = 0; i < pr->n; i++) {
        if (t < at + pr->p[i].secs) return g + pr->p[i].rate * fmax(t - at, 0.0);
        at += pr->p[i].secs;
        g += pr->p[i].rate * pr->p[i].secs;
    }
    return g + 0.1 * fmin(t - at, 3.0);
}

// ------------------------------------------------------------------ helpers

static bool run_with(shot_detector_t *d, readings_t rs, shot_t *out) {
    for (size_t i = 0; i < rs.n; i++) {
        shot_progress_t p = shot_push(d, rs.r[i]);
        if (p.kind == SHOT_DONE) {
            *out = p.shot;
            return true;
        }
    }
    return false;
}

static bool run(readings_t rs, shot_t *out) {
    shot_init(D);
    return run_with(D, rs, out);
}

static void check_shot(bool got, shot_t s, double secs, double grams, double tol, int line) {
    checks++;
    if (!got) {
        failures++;
        printf("  FAIL line %d: no shot (expected %.1f s, %.1f g)\n", line, secs, grams);
        return;
    }
    double t = s.time_ms / 1000.0;
    if (fabs(t - secs) > tol || fabs(s.grams - grams) > tol) {
        failures++;
        printf("  FAIL line %d: %.2f s %.2f g, expected %.1f s %.1f g (+-%.1f)\n", line, t, s.grams, secs, grams, tol);
    }
}
#define ASSERT_SHOT(got, s, secs, grams) check_shot(got, s, secs, grams, 0.4, __LINE__)

// ------------------------------------------------------------------ shots

static double g_espresso(double t, const void *c) {
    const double *a = c;
    return espresso(t, a[0], a[1]);
}

static void a_shot_from_first_drops_to_flow_stop(void) {
    shot_t s;
    double a[2] = {18.0, 2.0};
    readings_t rs = readings(60000, 32.0, g_espresso, a);
    bool got = run(rs, &s);
    ASSERT_SHOT(got, s, 18.0, 36.3);
    free(rs.r);
    double b[2] = {30.0, 1.2};
    rs = readings(0, 45.0, g_espresso, b);
    got = run(rs, &s);
    ASSERT_SHOT(got, s, 30.0, 36.3);
    free(rs.r);
}

static double g_cup(double t, const void *c) {
    (void)c;
    double cup = t < 1.0 ? 0.0 : t < 1.3 ? (t - 1.0) / 0.3 * 152.0 : t < 1.4 ? 152.0 : 150.0;
    return cup + espresso(t - 1.0, 18.0, 2.0);
}

static void the_cup_weight_is_not_coffee(void) {
    shot_t s;
    readings_t rs = readings(0, 36.0, g_cup, NULL);
    bool got = run(rs, &s);
    ASSERT_SHOT(got, s, 18.0, 36.3);
    free(rs.r);
}

static double g_lifted(double t, const void *c) {
    (void)c;
    double g = 150.0 + espresso(t, 18.0, 2.0);
    if (t < 22.0) return g;
    if (t < 22.2) return g * (22.2 - t) / 0.2;
    return 0.0;
}

static void lifting_the_cup_ends_the_shot(void) {
    shot_t s;
    readings_t rs = readings(0, 26.0, g_lifted, NULL);
    bool got = run(rs, &s);
    CHECK(got);
    CHECK(fabs(s.time_ms / 1000.0 - 18.0) <= 0.5);
    CHECK(fabs(s.grams - 36.2) <= 0.5);
    free(rs.r);
}

static double g_bumpy(double t, const void *c) {
    (void)c;
    double spike = (fabs(t - 1.5) < 0.05 || fabs(t - 4.0) < 0.05) ? 0.45 : 0.0;
    double bump = (t >= 6.0 && t < 6.5) ? 2.0 : 0.0;
    double press = (t >= 9.0 && t < 11.0) ? 3.0 : 0.0;
    return 150.0 + spike + bump + press;
}

static void bumps_are_not_shots(void) {
    readings_t rs = readings(0, 20.0, g_bumpy, NULL);
    shot_init(D);
    bool done = false;
    for (size_t i = 0; i < rs.n; i++) done |= shot_push(D, rs.r[i]).kind == SHOT_DONE;
    CHECK(!done);
    CHECK(shot_push(D, (shot_reading_t){20000, 150.0}).kind == SHOT_READY);
    free(rs.r);
}

static double g_press(double t, const void *c) {
    (void)c;
    double press = (t >= 4.0 && t < 6.0) ? 3.0 : 0.0;
    return 150.0 + press + espresso(t - 8.0, 18.0, 2.0);
}

static void press_before_shot_does_not_shift_start(void) {
    shot_t s;
    readings_t rs = readings(0, 40.0, g_press, NULL);
    bool got = run(rs, &s);
    ASSERT_SHOT(got, s, 18.0, 36.3);
    free(rs.r);
}

static void progress_goes_settling_ready_flowing(void) {
    double a[2] = {18.0, 2.0};
    readings_t rs = readings(0, 32.0, g_espresso, a);
    shot_init(D);
    bool ready = false, first_set = false;
    double first = 0, last = 0;
    for (size_t i = 0; i < rs.n; i++) {
        shot_progress_t p = shot_push(D, rs.r[i]);
        if (i == 0) CHECK(p.kind == SHOT_SETTLING);
        if (p.kind == SHOT_READY) ready = true;
        if (p.kind == SHOT_FLOWING) {
            if (!first_set) first = p.grams, first_set = true;
            last = p.grams;
        }
    }
    CHECK(ready);
    CHECK(first_set && first < 4.0);
    CHECK(last > 30.0);
    free(rs.r);
}

static void clock_going_back_starts_over(void) {
    double a[2] = {18.0, 2.0};
    readings_t rs = readings(500000, 10.0, g_espresso, a);
    shot_init(D);
    for (size_t i = 0; i < rs.n; i++) shot_push(D, rs.r[i]);
    free(rs.r);
    CHECK(shot_push(D, (shot_reading_t){510000, 16.0}).kind == SHOT_FLOWING);
    CHECK(shot_push(D, (shot_reading_t){1200, 16.0}).kind == SHOT_SETTLING);
    shot_t s;
    rs = readings(1300, 32.0, g_espresso, a);
    bool got = run_with(D, rs, &s);
    ASSERT_SHOT(got, s, 18.0, 36.3);
    free(rs.r);
}

static double g_tare(double t, const void *c) {
    (void)c;
    double before = t < 0.9 ? -3.0 : 0.0;
    return before + espresso(t - 2.0, 18.0, 2.0);
}

static void a_tare_is_not_coffee(void) {
    shot_t s;
    readings_t rs = readings(0, 36.0, g_tare, NULL);
    shot_init_after_tare(D);
    bool got = run_with(D, rs, &s);
    ASSERT_SHOT(got, s, 18.0, 36.3);
    free(rs.r);
    // Without the pause, the same jump would look like first drops.
    rs = readings(0, 3.5, g_tare, NULL);
    shot_init(D);
    bool flowed = false;
    for (size_t i = 0; i < rs.n; i++) flowed |= shot_push(D, rs.r[i]).kind == SHOT_FLOWING;
    CHECK(flowed);
    free(rs.r);
}

static double g_profile(double t, const void *c) { return profile(t, c); }

static void a_pause_after_preinfusion_is_not_the_end(void) {
    shot_t s;
    static const phase_t shortp[] = {{3.0, 2.0}, {4.0, 0.0}, {15.0, 2.0}};
    profile_t a = {shortp, 3};
    readings_t rs = readings(0, 36.0, g_profile, &a);
    bool got = run(rs, &s);
    ASSERT_SHOT(got, s, 22.0, 36.3);
    free(rs.r);

    static const phase_t longp[] = {{6.0, 1.2}, {4.0, 0.0}, {14.4, 2.0}};
    profile_t b = {longp, 3};
    rs = readings(0, 38.0, g_profile, &b);
    shot_init(D);
    shot_expecting(D, true, 36.0);
    got = run_with(D, rs, &s);
    ASSERT_SHOT(got, s, 24.4, 36.3);
    free(rs.r);
}

static void too_short_or_too_small_is_not_a_shot(void) {
    shot_t s;
    double a[2] = {3.0, 2.0};
    readings_t rs = readings(0, 15.0, g_espresso, a);
    CHECK(!run(rs, &s));
    free(rs.r);
}

// ------------------------------------------------------------------ ours

// The AKU sent ~3 readings/s on hardware (M2 status: rate_hz 3.0). Same
// shots at 333 ms; the median of three then spans ~0.7 s, so allow 0.7 s.
static void shots_at_aku_rate(void) {
    shot_t s;
    double a[2] = {18.0, 2.0};
    readings_t rs = readings_every(0, 32.0, 333, g_espresso, a);
    bool got = run(rs, &s);
    check_shot(got, s, 18.0, 36.3, 0.7, __LINE__);
    free(rs.r);

    rs = readings_every(0, 36.0, 333, g_tare, NULL);
    shot_init_after_tare(D);
    shot_expecting(D, true, 36.0);
    got = run_with(D, rs, &s);
    check_shot(got, s, 18.0, 36.3, 0.7, __LINE__);
    free(rs.r);

    static const phase_t longp[] = {{6.0, 1.2}, {4.0, 0.0}, {14.4, 2.0}};
    profile_t b = {longp, 3};
    rs = readings_every(0, 38.0, 333, g_profile, &b);
    shot_init(D);
    shot_expecting(D, true, 36.0);
    got = run_with(D, rs, &s);
    check_shot(got, s, 24.4, 36.3, 0.7, __LINE__);
    free(rs.r);
}

// end_ms - start_ms is the reported time, and both are on the readings' clock.
static void shot_carries_its_start_and_end(void) {
    shot_t s;
    double a[2] = {18.0, 2.0};
    readings_t rs = readings(60000, 32.0, g_espresso, a);
    CHECK(run(rs, &s));
    CHECK(s.end_ms - s.start_ms == (int64_t)s.time_ms);
    CHECK(llabs(s.start_ms - 62000) <= 400);
    free(rs.r);
}

// Hardware finding: AKU readings can share a timestamp (frames of one
// notification). Fed as they are, the detector restarts on each repeat and
// never finds the shot; nudged 1 ms apart (as autoshot.c does) it does.
static void repeated_timestamps(void) {
    double a[2] = {18.0, 2.0};
    readings_t rs = readings_every(0, 32.0, 333, g_espresso, a);
    // Every other reading repeats the previous time.
    for (size_t i = 1; i < rs.n; i += 2) rs.r[i].ms = rs.r[i - 1].ms;
    shot_t s;
    CHECK(!run(rs, &s));  // the raw feed: no shot (shot.rs restart rule)
    int64_t last = -1;
    for (size_t i = 0; i < rs.n; i++) {
        if (rs.r[i].ms <= last) rs.r[i].ms = last + 1;
        last = rs.r[i].ms;
    }
    bool got = run(rs, &s);
    check_shot(got, s, 18.0, 36.3, 0.7, __LINE__);
    free(rs.r);
}

// Flowing progress says when (the filtered reading) and where the shot started.
static void flowing_progress_carries_times(void) {
    double a[2] = {18.0, 2.0};
    readings_t rs = readings(0, 32.0, g_espresso, a);
    shot_init(D);
    int64_t prev = -1;
    bool any = false, ok = true;
    for (size_t i = 0; i < rs.n; i++) {
        shot_progress_t p = shot_push(D, rs.r[i]);
        if (p.kind != SHOT_FLOWING) continue;
        any = true;
        ok &= p.ms > prev && p.ms <= rs.r[i].ms && llabs(p.start_ms - 2000) <= 400;
        prev = p.ms;
    }
    CHECK(any);
    CHECK(ok);
    free(rs.r);
}

// ------------------------------------------------------------------ shot_decide (report modes)

static shot_progress_t flowing_at(int64_t ms, int64_t start_ms, double g) {
    shot_progress_t p = {.kind = SHOT_FLOWING, .grams = g, .ms = ms, .start_ms = start_ms};
    return p;
}

static void decide_target(void) {
    shot_decide_cfg_t c = {.target = true, .target_g = 36.0, .knob_ms = 10000, .timing_mode = 1};
    shot_progress_t below = flowing_at(30000, 15000, 35.9);
    CHECK(shot_decide(&below, &c).kind == SHOT_REPORT_NONE);
    shot_progress_t at = flowing_at(33600, 15000, 36.04);
    shot_decision_t d = shot_decide(&at, &c);
    CHECK(d.kind == SHOT_REPORT_TARGET);
    CHECK(d.time_ms == 23600);  // knob to the filtered reading that reached it
    CHECK(fabs(d.grams - 36.0) < 1e-9);
    // Timing mode 3: first drops + pre-infusion.
    c.timing_mode = 3;
    c.preinfusion_ms = 4000;
    d = shot_decide(&at, &c);
    CHECK(d.time_ms == 18600 + 4000);
    // Not flowing (settling / ready): never.
    shot_progress_t ready = {.kind = SHOT_READY, .grams = 50.0, .ms = 33600};
    CHECK(shot_decide(&ready, &c).kind == SHOT_REPORT_NONE);
}

static shot_progress_t done(int64_t start_ms, int64_t end_ms, double g) {
    shot_progress_t p = {.kind = SHOT_DONE};
    p.shot.start_ms = start_ms;
    p.shot.end_ms = end_ms;
    p.shot.time_ms = (uint32_t)(end_ms - start_ms);
    p.shot.grams = g;
    return p;
}

static void decide_end_of_flow(void) {
    shot_decide_cfg_t c = {.target = false, .knob_ms = 10000, .timing_mode = 1};
    shot_progress_t p = done(16000, 39800, 41.2);
    shot_decision_t d = shot_decide(&p, &c);
    CHECK(d.kind == SHOT_REPORT_END);
    CHECK(d.time_ms == 29800);  // knob to end of flow
    CHECK(fabs(d.grams - 41.2) < 1e-9);
    // Auto never reports while flowing, however much is in the cup.
    shot_progress_t f = flowing_at(30000, 16000, 80.0);
    CHECK(shot_decide(&f, &c).kind == SHOT_REPORT_NONE);
    // End before the knob (clock oddity): the detector's own time.
    shot_progress_t early = done(2000, 9000, 20.0);
    CHECK(shot_decide(&early, &c).time_ms == 7000);
    c.timing_mode = 3;
    c.preinfusion_ms = 3000;
    CHECK(shot_decide(&p, &c).time_ms == 23800 + 3000);
}

// A choked shot in target mode is reported at the end of flow, as auto.
static void decide_choke_in_target(void) {
    shot_decide_cfg_t c = {.target = true, .target_g = 36.0, .knob_ms = 10000, .timing_mode = 1};
    shot_progress_t p = done(16000, 52000, 25.0);
    shot_decision_t d = shot_decide(&p, &c);
    CHECK(d.kind == SHOT_REPORT_END_BEFORE_TARGET);
    CHECK(d.time_ms == 42000);
    CHECK(fabs(d.grams - 25.0) < 1e-9);
}

static void monotonic_times(void) {
    int64_t last = 0;
    CHECK(shot_monotonic_ms(&last, 100) == 100);
    CHECK(shot_monotonic_ms(&last, 100) == 101);
    CHECK(shot_monotonic_ms(&last, 100) == 102);
    CHECK(shot_monotonic_ms(&last, 50) == 103);
    CHECK(shot_monotonic_ms(&last, 200) == 200);
}

// The whole Target path: synthetic 10 Hz shot through the detector and the
// decision, as autoshot.c runs it. Reports as the cup passes 36 g.
static void target_end_to_end(void) {
    double a[2] = {30.0, 2.0};  // 60 g if left to run
    readings_t rs = readings(0, 40.0, g_espresso, a);
    shot_init(D);
    shot_expecting(D, true, 36.0);
    shot_decide_cfg_t c = {.target = true, .target_g = 36.0, .knob_ms = 0, .timing_mode = 1};
    int64_t last = -1;
    shot_decision_t d = {SHOT_REPORT_NONE, 0, 0};
    for (size_t i = 0; i < rs.n && d.kind == SHOT_REPORT_NONE; i++) {
        shot_reading_t r = rs.r[i];
        r.ms = shot_monotonic_ms(&last, r.ms);
        shot_progress_t p = shot_push(D, r);
        d = shot_decide(&p, &c);
    }
    CHECK(d.kind == SHOT_REPORT_TARGET);
    // 36 g at 2 g/s from 2 s: t = 20 s; median lag + noise: within 0.4 s.
    CHECK(fabs(d.time_ms / 1000.0 - 20.0) <= 0.4);
    CHECK(d.grams >= 36.0 && d.grams < 37.0);
    free(rs.r);
}

// ------------------------------------------------------------------ scale_clock (ours)

// A timer started at 1000 ms, one frame per second 120 ms after each tick.
static void scale_clock_follows_the_display(void) {
    scale_clock_t c;
    scale_clock_reset(&c);
    uint32_t s = 99;
    CHECK(!scale_clock_at(&c, 5000, 2500, &s));
    scale_clock_frame(&c, 1100, 0);  // reset frame: not started yet
    CHECK(!scale_clock_at(&c, 1500, 2500, &s));
    for (uint32_t n = 1; n <= 25; n++) scale_clock_frame(&c, 1000 + n * 1000 + 120, n);
    // 21.5 s after the start the scale showed 21.
    CHECK(scale_clock_at(&c, 1000 + 21500, 2500, &s) && s == 21);
    // Long after the last frame (timer stopped, scale gone): nothing.
    CHECK(!scale_clock_at(&c, 1000 + 25120 + 3000, 2500, &s));
}

// Frames more often than ticks pin the start down; a reset starts over.
static void scale_clock_resets(void) {
    scale_clock_t c;
    scale_clock_reset(&c);
    uint32_t s = 0;
    // Started at 400 ms; frames every 300 ms, 80 ms late.
    for (int64_t t = 480; t < 10000; t += 300) scale_clock_frame(&c, t, (uint32_t)((t - 480) / 1000));
    CHECK(c.valid && c.hi_ms - c.lo_ms <= 300);
    CHECK(scale_clock_at(&c, 400 + 7600, 2500, &s) && s == 7);
    // Previous shot's value, then a reset and a new start at 20000.
    scale_clock_frame(&c, 15000, 0);
    CHECK(!c.valid);
    for (uint32_t n = 1; n <= 12; n++) scale_clock_frame(&c, 20000 + n * 1000 + 100, n);
    CHECK(scale_clock_at(&c, 20000 + 12050, 2500, &s) && s == 11);
}

// A stale frame from before a restart can't wedge it: the newest wins.
static void scale_clock_inconsistent_frames(void) {
    scale_clock_t c;
    scale_clock_reset(&c);
    uint32_t s = 0;
    scale_clock_frame(&c, 5000, 4);   // start in (0, 1000]
    scale_clock_frame(&c, 9000, 5);   // start in (3000, 4000]: contradicts
    CHECK(c.valid && c.lo_ms == 3000 && c.hi_ms == 4000);
    CHECK(scale_clock_at(&c, 9500, 2500, &s) && s == 5);
}

int main(void) {
    D = malloc(sizeof *D);
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"a_shot_from_first_drops_to_flow_stop", a_shot_from_first_drops_to_flow_stop},
        {"the_cup_weight_is_not_coffee", the_cup_weight_is_not_coffee},
        {"lifting_the_cup_ends_the_shot", lifting_the_cup_ends_the_shot},
        {"bumps_are_not_shots", bumps_are_not_shots},
        {"press_before_shot_does_not_shift_start", press_before_shot_does_not_shift_start},
        {"progress_goes_settling_ready_flowing", progress_goes_settling_ready_flowing},
        {"clock_going_back_starts_over", clock_going_back_starts_over},
        {"a_tare_is_not_coffee", a_tare_is_not_coffee},
        {"a_pause_after_preinfusion_is_not_the_end", a_pause_after_preinfusion_is_not_the_end},
        {"too_short_or_too_small_is_not_a_shot", too_short_or_too_small_is_not_a_shot},
        {"shots_at_aku_rate", shots_at_aku_rate},
        {"shot_carries_its_start_and_end", shot_carries_its_start_and_end},
        {"repeated_timestamps", repeated_timestamps},
        {"flowing_progress_carries_times", flowing_progress_carries_times},
        {"decide_target", decide_target},
        {"decide_end_of_flow", decide_end_of_flow},
        {"decide_choke_in_target", decide_choke_in_target},
        {"monotonic_times", monotonic_times},
        {"target_end_to_end", target_end_to_end},
        {"scale_clock_follows_the_display", scale_clock_follows_the_display},
        {"scale_clock_resets", scale_clock_resets},
        {"scale_clock_inconsistent_frames", scale_clock_inconsistent_frames},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = failures;
        tests[i].fn();
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
    }
    printf("%d checks, %d failures\n", checks, failures);
    free(D);
    return failures ? 1 : 0;
}
