// SPDX-License-Identifier: AGPL-3.0-or-later
// Unit tests for machine/, ported from the tests in gbs-anywhere
// src/protocol/machine.rs and wire.rs. The grinder side is a port of
// gbs-anywhere src/protocol/grinder.rs (GrinderModel), used as the oracle.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "machine.h"
#include "machine_wire.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        checks++;                                                         \
        if (!(cond)) {                                                    \
            failures++;                                                   \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                 \
    } while (0)

#define POLL_MS 2000

// ------------------------------------------------------------------ grinder.rs port

typedef enum {
    BREW_IDLE = 0,
    BREW_FLUSHING = 1,
    BREW_FLUSH_DONE = 2,
    BREW_STARTING = 3,
    BREW_EXTRACTION = 4,
    BREW_FINISHING = 5,
    BREW_ABORTED = 6,
} brew_state_t;

typedef struct {
    int64_t status, extraction_status;
    brew_state_t brew_state;
} grinder_t;

typedef struct {
    bool accepted;  // ShotAccepted
    uint32_t time_ms;
    double volume_ml;
    bool skipped;   // ShotSkipped
} grinder_out_t;

static void grinder_request_start(grinder_t *g) { g->brew_state = BREW_STARTING; }

// grinder.rs:90-129
static grinder_out_t grinder_observe(grinder_t *g, const mako_state_t *s) {
    grinder_out_t out = {0};
    int64_t status = s->status, extr = s->extraction_status;
    int64_t old_status = g->status, old_extr = g->extraction_status;
    g->status = status;
    g->extraction_status = extr;
    if (old_status == status && old_extr == extr) return out;
    int next = -1;
    switch (status) {
        case 0: case 2: case 5: case 6: next = BREW_IDLE; break;
        case 1: if (g->brew_state != BREW_FLUSH_DONE) next = BREW_IDLE; break;
        case 3:
            if (old_status != 3) next = g->brew_state == BREW_STARTING ? BREW_EXTRACTION : BREW_FLUSHING;
            break;
        case 4:
            if (g->brew_state == BREW_FLUSHING || g->brew_state == BREW_FLUSH_DONE) next = BREW_FLUSH_DONE;
            else if (extr == EXTRACTION_STATUS_USER_ABORT) next = BREW_ABORTED;
            else next = BREW_FINISHING;
            break;
    }
    if (next >= 0 && g->brew_state != (brew_state_t)next) {
        g->brew_state = next;
        if (next == BREW_FINISHING) {
            // grinder.rs:144-156
            if (g->extraction_status == EXTRACTION_STATUS_USER_ABORT ||
                s->real_extraction_time_ms > MACHINE_MAX_EXTRACTION_TIME_MS) {
                out.skipped = true;
            } else {
                out.accepted = true;
                out.time_ms = s->real_extraction_time_ms;
                out.volume_ml = s->flow_meter_volume_ml;
            }
        }
    }
    return out;
}

static grinder_out_t observe(grinder_t *g, machine_t *m, int64_t t) {
    mako_state_t s;
    machine_mako(m, t, &s);
    return grinder_observe(g, &s);
}

static grinder_t grinder_new(void) {
    grinder_t g = {0, 0, BREW_IDLE};
    return g;
}

static machine_t new_machine(uint32_t hold_ms) {
    machine_config_t c;
    machine_config_default(&c);
    if (hold_ms) c.finishing_hold_ms = hold_ms;
    machine_t m;
    machine_init(&m, &c);
    return m;
}

static shot_result_t shot(uint32_t ms, double g) {
    shot_result_t r = {ms, g};
    return r;
}

static const machine_event_t *last_event(const machine_t *m) {
    return m->seq ? &m->events[m->seq % MACHINE_EVENT_CAP] : NULL;
}

// machine.rs:705-719: polls every 2 s through the grinder; the accepted time or 0
static uint32_t poll_until_ready(machine_t *m, grinder_t *g, int64_t t) {
    uint32_t accepted = 0;
    for (int i = 0; i < 20; i++) {
        grinder_out_t o = observe(g, m, t);
        if (o.accepted) accepted = o.time_ms;
        if (machine_phase(m, t) == MACHINE_READY) break;
        t += POLL_MS;
    }
    return accepted;
}

// ------------------------------------------------------------------ wire.rs tests

static void test_mako_serializes_with_grinder_keys(void) {
    mako_state_t s;
    mako_state_default(&s);
    char buf[512];
    int n = wire_mako_json(&s, buf, sizeof buf);
    CHECK(n > 0 && n < (int)sizeof buf);
    const char *keys[] = {"REAL_EXTRACTION_TIME_MS", "PU_SENS_FLOW_METER_VOLUME", "BG_SET_TEMP",
                          "BG_SENS_TEMP", "TANK_LEVEL", "script", "TIMESTAMP",
                          "MA_EXTRACTION_STATUS", "MA_STATUS"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        char k[48];
        snprintf(k, sizeof k, "\"%s\":", keys[i]);
        CHECK(strstr(buf, k) != NULL);
    }
    CHECK(strstr(buf, "\"MA_STATUS\":1,") != NULL);
    // As serde_json writes MakoState::default() through to_value (sorted keys).
    CHECK(!strcmp(buf,
                  "{\"BG_SENS_TEMP\":93.0,\"BG_SET_TEMP\":93.0,\"MA_EXTRACTION_STATUS\":0,"
                  "\"MA_STATUS\":1,\"PU_SENS_FLOW_METER_VOLUME\":0.0,\"REAL_EXTRACTION_TIME_MS\":0,"
                  "\"TANK_LEVEL\":1,\"TIMESTAMP\":\"\",\"script\":\"\"}"));
}

static void test_identity_json(void) {
    machine_identity_t id;
    machine_identity_default(&id);
    char buf[512];
    wire_identity_json(&id, buf, sizeof buf);
    CHECK(!strcmp(buf,
                  "{\"BLE\":0,\"ESP_FW_MAJOR\":3,\"ESP_FW_MINOR\":87,\"FW_VERSION_MAJOR\":4,"
                  "\"FW_VERSION_MINOR\":226,\"MA_DELAY\":-1,\"MA_FIX_WATER_SUPPLY\":0,"
                  "\"MA_MAX_AMPERE\":13,\"MA_SN\":\"0000000000\",\"MA_SUBTYPE\":0,\"MA_TYPE\":1}"));
    // A buffer too small reports the length it needed and stays terminated.
    char small[16];
    int n = wire_identity_json(&id, small, sizeof small);
    CHECK(n == (int)strlen(buf));
    CHECK(strlen(small) == sizeof small - 1);
}

static void test_fmt_f64(void) {
    char b[40];
    wire_fmt_f64(93.0, b, sizeof b); CHECK(!strcmp(b, "93.0"));
    wire_fmt_f64(0.0, b, sizeof b); CHECK(!strcmp(b, "0.0"));
    wire_fmt_f64(36.5, b, sizeof b); CHECK(!strcmp(b, "36.5"));
    wire_fmt_f64(38.12, b, sizeof b); CHECK(!strcmp(b, "38.12"));
    wire_fmt_f64(0.1, b, sizeof b); CHECK(!strcmp(b, "0.1"));
    wire_fmt_f64(-7.17, b, sizeof b); CHECK(!strcmp(b, "-7.17"));
    wire_fmt_f64(NAN, b, sizeof b); CHECK(!strcmp(b, "null"));
}

static void test_grind_gate(void) {
    // wire.rs:372-400
    mako_state_t ok;
    mako_state_default(&ok);
    CHECK(mako_grind_blocked(&ok) == GRIND_ALLOWED);
    mako_state_t s = ok;
    s.status = MA_STATUS_ACTIVE_FINISHING;
    CHECK(mako_grind_blocked(&s) == GRIND_ALLOWED);
    s = ok, s.bg_sens_temp = 90.9;
    CHECK(mako_grind_blocked(&s) == GRIND_BLOCKED_MACHINE_WARMING_UP);
    s = ok, s.bg_sens_temp = 91.1;
    CHECK(mako_grind_blocked(&s) == GRIND_ALLOWED);
    s = ok, s.tank_level = 2;
    CHECK(mako_grind_blocked(&s) == GRIND_BLOCKED_MACHINE_TANK_EMPTY);
    s = ok, s.status = MA_STATUS_ACTIVE_SERVING;
    CHECK(mako_grind_blocked(&s) == GRIND_BLOCKED_SHOT_RUNNING);
}

static void test_classify_grinder_writes(void) {
    // wire.rs:403-442
    wire_request_t r;
    const char *b1 = "{\"SYNC_BEVERAGE_WEIGHT\":36.5,\"SYNC_FILTER\":\"2\"}";
    wire_classify("POST", "/api/v2/brewratio", (const uint8_t *)b1, strlen(b1), &r);
    CHECK(r.kind == WREQ_GRIND_RESULT);
    CHECK(r.grind.has_target && r.grind.target_weight_g == 36.5);
    CHECK(r.grind.has_filter && !strcmp(r.grind.filter, "2"));

    // A line break can't make a log line of its own; text is cut to 64 chars.
    char forged[256] = "{\"SYNC_FILTER\":\"2\\nWARN forged";
    for (int i = 0; i < 100; i++) strcat(forged, "x");
    strcat(forged, "\"}");
    wire_classify("POST", "/api/v2/brewratio", (const uint8_t *)forged, strlen(forged), &r);
    CHECK(r.kind == WREQ_GRIND_RESULT);
    CHECK(!strncmp(r.grind.filter, "2WARN forged", 12));
    CHECK(strlen(r.grind.filter) == WIRE_MAX_TEXT_CHARS);
    CHECK(!r.grind.has_target);

    const char *b2 = "{\"ID\":9}";
    wire_classify("POST", "/api/v2/scripts/execute", (const uint8_t *)b2, strlen(b2), &r);
    CHECK(r.kind == WREQ_START_BREW && r.has_script_id && r.script_id == 9);

    wire_classify("get", "/api/v2/mako/", NULL, 0, &r);
    CHECK(r.kind == WREQ_POLL_MAKO);
    wire_classify("GET", "/api/v2/machine", NULL, 0, &r);
    CHECK(r.kind == WREQ_IDENTITY);
    wire_classify("POST", "/api/v2/brewratio", (const uint8_t *)"nope", 4, &r);
    CHECK(r.kind == WREQ_MALFORMED);
    wire_classify("GET", "/api/v2/brewratio", NULL, 0, &r);
    CHECK(r.kind == WREQ_OTHER);
    wire_classify("PUT", "/api/v2/whatever", NULL, 0, &r);
    CHECK(r.kind == WREQ_OTHER);

    // Numbers given as strings count (wire.rs:317), a number filter is text.
    const char *b3 = "{\"SYNC_BEVERAGE_WEIGHT\":\" 40 \",\"SYNC_FILTER\":2}";
    wire_classify("POST", "/api/v2/brewratio", (const uint8_t *)b3, strlen(b3), &r);
    CHECK(r.grind.has_target && r.grind.target_weight_g == 40.0);
    CHECK(!strcmp(r.grind.filter, "2"));

    CHECK(wire_grinder_path("/api/v2"));
    CHECK(wire_grinder_path("/api/v2/"));
    CHECK(wire_grinder_path("/api/v2/mako"));
    CHECK(!wire_grinder_path("/api/v20"));
    CHECK(!wire_grinder_path("/api/scale"));
}

// ------------------------------------------------------------------ machine.rs tests

static void test_e64_range(void) {
    // machine.rs:733-744
    CHECK(!machine_e64_in_range(0));
    CHECK(!machine_e64_in_range(10000));
    CHECK(machine_e64_in_range(10001));
    CHECK(machine_e64_in_range(28500));
    CHECK(machine_e64_in_range(80000));
    CHECK(!machine_e64_in_range(80001));
}

static void test_human_shot_is_accepted_by_the_grinder_model(void) {
    // machine.rs:816-849
    int64_t t0 = 1000000;
    machine_t m = new_machine(0);
    grinder_t g = grinder_new();
    observe(&g, &m, t0);

    grinder_request_start(&g);
    machine_on_start_request(&m, t0, true, 9);
    int64_t t1 = t0 + POLL_MS;
    observe(&g, &m, t1);
    CHECK(g.brew_state == BREW_EXTRACTION);
    mako_state_t s;
    machine_mako(&m, t1, &s);
    CHECK(s.real_extraction_time_ms == 2000);

    machine_on_start_request(&m, t1, true, 9);  // grinder retry: ignored
    CHECK(m.since_ms == t0);

    int64_t t2 = t1 + 28000;
    observe(&g, &m, t2);
    CHECK(machine_report_shot(&m, t2, shot(30000, 36.0)) == MACHINE_OK);
    const machine_event_t *e = last_event(&m);
    CHECK(e && e->kind == MEV_SHOT_REPORTED && e->reported.running_ms == 30000);
    CHECK(e->reported.in_grinder_range == 1);
    CHECK(poll_until_ready(&m, &g, t2) == 30000);
    machine_mako(&m, t2 + 60000, &s);
    CHECK(s.status == MA_STATUS_ON);
    // While Ready, the last shot stays visible (machine.rs:613-616).
    CHECK(s.real_extraction_time_ms == 30000 && s.flow_meter_volume_ml == 36.0);
}

// machine.rs:854-881
static bool taken(uint32_t hold_ms, int64_t phase, const int64_t *gaps, size_t ngaps, size_t first) {
    int64_t t0 = 1000000;
    machine_t m = new_machine(hold_ms);
    m.config.finishing_hold_ms = hold_ms;  // allow < MIN to show a short hold misses
    grinder_t g = grinder_new();
    observe(&g, &m, t0);
    grinder_request_start(&g);
    machine_on_start_request(&m, t0, true, 9);
    observe(&g, &m, t0 + POLL_MS);
    int64_t reported = t0 + 30000;
    int64_t t = reported - phase;
    observe(&g, &m, t);
    machine_report_shot(&m, reported, shot(28000, 36.0));
    for (size_t i = 0; i < 20; i++) {
        t += gaps[(first + i) % ngaps];
        if (observe(&g, &m, t).accepted) return true;
    }
    return false;
}

static void test_shortest_hold_is_always_taken(void) {
    // machine.rs:884-901
    const int64_t gaps[] = {2000, 2400, 1600, 2900, 2000};
    int missed = 0;
    for (size_t first = 0; first < 5; first++) {
        for (int tenths = 0; tenths < 20; tenths++) {
            int64_t phase = tenths * 100;
            CHECK(taken(MACHINE_MIN_FINISHING_HOLD_MS, phase, gaps, 5, first));
            if (!taken(1000, phase, gaps, 5, first)) missed++;
        }
    }
    CHECK(missed > 0);
}

static void test_hold_is_clamped(void) {
    machine_t m = new_machine(1000);
    CHECK(m.config.finishing_hold_ms == MACHINE_MIN_FINISHING_HOLD_MS);
    m = new_machine(120000);
    CHECK(m.config.finishing_hold_ms == MACHINE_MAX_FINISHING_HOLD_MS);
}

static void test_shot_without_weight_gets_recipe_weight(void) {
    // machine.rs:904-928
    int64_t t0 = 1000000;
    machine_t m = new_machine(0);
    machine_on_start_request(&m, t0, true, 9);
    CHECK(machine_report_shot(&m, t0, shot(30000, 0.0)) == MACHINE_ERR_NO_WEIGHT);
    CHECK(machine_phase(&m, t0) == MACHINE_RUNNING);

    grind_result_t recipe = {.has_target = true, .target_weight_g = 36.5};
    machine_on_grind_result(&m, t0, &recipe);
    const double none[] = {0.0, -1.0, NAN};
    for (int i = 0; i < 3; i++) {
        machine_t c = m;
        CHECK(machine_report_shot(&c, t0, shot(30000, none[i])) == MACHINE_OK);
        CHECK(c.last_shot.weight_g == 36.5);
    }
    CHECK(machine_report_shot(&m, t0, shot(30000, 38.0)) == MACHINE_OK);
    CHECK(m.last_shot.weight_g == 38.0);
}

static void test_manual_start_counts_as_flush(void) {
    // machine.rs:931-942
    int64_t t0 = 1000000;
    machine_t m = new_machine(0);
    grinder_t g = grinder_new();
    observe(&g, &m, t0);
    CHECK(machine_start_shot(&m, t0) == MACHINE_OK);
    CHECK(machine_start_shot(&m, t0) == MACHINE_ERR_ALREADY_RUNNING);
    observe(&g, &m, t0 + POLL_MS);
    CHECK(g.brew_state == BREW_FLUSHING);
    CHECK(machine_report_shot(&m, t0 + POLL_MS, shot(25000, 30.0)) == MACHINE_OK);
    CHECK(last_event(&m)->reported.in_grinder_range == -1);
    CHECK(poll_until_ready(&m, &g, t0 + POLL_MS) == 0);
}

static void test_abort_and_timeout(void) {
    // machine.rs:945-969
    int64_t t0 = 1000000;
    machine_t m = new_machine(0);
    CHECK(machine_abort(&m, t0) == MACHINE_ERR_NOT_RUNNING);
    machine_on_start_request(&m, t0, true, 9);
    CHECK(machine_abort(&m, t0 + POLL_MS) == MACHINE_OK);
    mako_state_t s;
    machine_mako(&m, t0 + POLL_MS, &s);
    CHECK(s.extraction_status == EXTRACTION_STATUS_USER_ABORT);
    const machine_event_t *e = last_event(&m);
    CHECK(e->seq == 3 && e->kind == MEV_SHOT_ABORTED);  // machine.rs:981
    CHECK(e->aborted.cause == ABORT_SHOT_ABORTED_BY_USER && e->aborted.running_ms == 2000);
    CHECK(!strcmp(abort_cause_code(e->aborted.cause), "shot_aborted_by_user"));
    CHECK(!strcmp(abort_cause_text(e->aborted.cause), "aborted by user"));

    m = new_machine(0);
    machine_on_start_request(&m, t0, true, 9);
    int64_t late = t0 + 181000;
    CHECK(machine_phase(&m, late) == MACHINE_FINISHING && m.aborted);
    e = last_event(&m);
    CHECK(e->kind == MEV_SHOT_ABORTED && e->aborted.cause == ABORT_SHOT_TIMED_OUT &&
          e->aborted.running_ms == 181000);
    CHECK(!strcmp(abort_cause_text(ABORT_SHOT_TIMED_OUT), "no result reported in time"));

    // The aborted shot is skipped by the grinder.
    m = new_machine(0);
    grinder_t g = grinder_new();
    observe(&g, &m, t0);
    grinder_request_start(&g);
    machine_on_start_request(&m, t0, true, 9);
    observe(&g, &m, t0 + POLL_MS);
    machine_abort(&m, t0 + 3000);
    observe(&g, &m, t0 + 4000);
    CHECK(g.brew_state == BREW_ABORTED);
}

static void test_new_shot_window_counts_for_running_shot(void) {
    // machine.rs:1056-1075
    int64_t t0 = 1000000;
    machine_t m = new_machine(0);
    machine_on_start_request(&m, t0, true, 9);
    machine_set_shot_window(&m, 90000);
    CHECK(machine_phase(&m, t0 + 89000) == MACHINE_RUNNING);
    CHECK(machine_phase(&m, t0 + 90000) == MACHINE_FINISHING && m.aborted);

    m = new_machine(0);
    machine_set_shot_window(&m, 0);
    machine_on_start_request(&m, t0, true, 9);
    CHECK(machine_phase(&m, t0 + 4LL * 3600 * 1000) == MACHINE_RUNNING);
}

static void test_events_ring(void) {
    int64_t t0 = 1000000;
    machine_t m = new_machine(0);
    machine_event_t out[MACHINE_EVENT_CAP];
    CHECK(machine_events_after(&m, 0, out, MACHINE_EVENT_CAP) == 0);
    grind_result_t gr = {.has_target = true, .target_weight_g = 40.0, .has_filter = true, .filter = "2"};
    machine_on_grind_result(&m, t0, &gr);
    machine_on_start_request(&m, t0, true, 9);
    size_t n = machine_events_after(&m, 0, out, MACHINE_EVENT_CAP);
    CHECK(n == 3);
    CHECK(out[0].kind == MEV_GRINDER_GRIND_FINISHED && out[0].grind.target_weight_g == 40.0 &&
          !strcmp(out[0].grind.filter, "2"));
    CHECK(out[1].kind == MEV_GRINDER_SHOT_REQUESTED && out[1].requested.script_id == 9);
    CHECK(out[2].kind == MEV_SHOT_STARTED && out[2].started.requested_by_grinder);
    CHECK(machine_events_after(&m, 2, out, MACHINE_EVENT_CAP) == 1 && out[0].seq == 3);
    // Past the ring size only the newest MACHINE_EVENT_CAP are kept, oldest first.
    for (int i = 0; i < 40; i++) machine_on_grind_result(&m, t0, &gr);
    n = machine_events_after(&m, 0, out, MACHINE_EVENT_CAP);
    CHECK(n == MACHINE_EVENT_CAP && out[0].seq == m.seq - MACHINE_EVENT_CAP + 1 &&
          out[n - 1].seq == m.seq);
}

int main(void) {
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"mako_serializes_with_grinder_keys", test_mako_serializes_with_grinder_keys},
        {"identity_json", test_identity_json},
        {"fmt_f64", test_fmt_f64},
        {"grind_gate", test_grind_gate},
        {"classify_grinder_writes", test_classify_grinder_writes},
        {"e64_range", test_e64_range},
        {"human_shot_is_accepted_by_the_grinder_model", test_human_shot_is_accepted_by_the_grinder_model},
        {"shortest_hold_is_always_taken", test_shortest_hold_is_always_taken},
        {"hold_is_clamped", test_hold_is_clamped},
        {"shot_without_weight_gets_recipe_weight", test_shot_without_weight_gets_recipe_weight},
        {"manual_start_counts_as_flush", test_manual_start_counts_as_flush},
        {"abort_and_timeout", test_abort_and_timeout},
        {"new_shot_window_counts_for_running_shot", test_new_shot_window_counts_for_running_shot},
        {"events_ring", test_events_ring},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = failures;
        tests[i].fn();
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
