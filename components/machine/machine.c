// SPDX-License-Identifier: AGPL-3.0-or-later
// Ported from gbs-anywhere src/protocol/machine.rs and wire.rs (E64 WS only).
#include "machine.h"

#include <math.h>
#include <string.h>

// ------------------------------------------------------------------ wire.rs types

void mako_state_default(mako_state_t *s) {
    // wire.rs:96-107
    memset(s, 0, sizeof *s);
    s->real_extraction_time_ms = 0;
    s->flow_meter_volume_ml = 0.0;
    s->bg_set_temp = 93.0;
    s->bg_sens_temp = 93.0;
    s->tank_level = TANK_LEVEL_FULL;
    s->script[0] = 0;
    s->timestamp[0] = 0;
    s->extraction_status = 0;
    s->status = MA_STATUS_ON;
}

grind_blocked_t mako_grind_blocked(const mako_state_t *s) {
    // wire.rs:113-136
    bool heated = s->bg_sens_temp > s->bg_set_temp - HEATED_TOLERANCE_C;
    bool tank_full = s->tank_level == TANK_LEVEL_FULL;
    switch (s->status) {
        case MA_STATUS_ON:
        case MA_STATUS_ACTIVE_FINISHING:
            if (!heated) return GRIND_BLOCKED_MACHINE_WARMING_UP;
            if (!tank_full) return GRIND_BLOCKED_MACHINE_TANK_EMPTY;
            return GRIND_ALLOWED;
        case MA_STATUS_OFF: return GRIND_BLOCKED_MACHINE_IN_STANDBY;
        case MA_STATUS_ECO: return GRIND_BLOCKED_MACHINE_IN_ECO;
        case MA_STATUS_ACTIVE_SERVING: return GRIND_BLOCKED_SHOT_RUNNING;
        case MA_STATUS_TESTING: return GRIND_BLOCKED_MACHINE_TESTING;
        case MA_STATUS_FAULT: return GRIND_BLOCKED_MACHINE_FAULTY;
    }
    return GRIND_BLOCKED_MACHINE_FAULTY;
}

const char *grind_blocked_code(grind_blocked_t b) {
    // wire.rs:142 serde(rename_all = "snake_case")
    switch (b) {
        case GRIND_ALLOWED: return "none";
        case GRIND_BLOCKED_SHOT_RUNNING: return "shot_running";
        case GRIND_BLOCKED_MACHINE_IN_STANDBY: return "machine_in_standby";
        case GRIND_BLOCKED_MACHINE_IN_ECO: return "machine_in_eco";
        case GRIND_BLOCKED_MACHINE_TESTING: return "machine_testing";
        case GRIND_BLOCKED_MACHINE_FAULTY: return "machine_faulty";
        case GRIND_BLOCKED_MACHINE_WARMING_UP: return "machine_warming_up";
        case GRIND_BLOCKED_MACHINE_TANK_EMPTY: return "machine_tank_empty";
    }
    return "unknown";
}

void machine_identity_default(machine_identity_t *id) {
    // wire.rs:213-228: the identity values gbs-anywhere reports (MA 4.226 / ESP 3.87)
    memset(id, 0, sizeof *id);
    id->machine_type = 1;
    id->machine_subtype = 0;
    strcpy(id->serial, "0000000000");
    id->fw_major = 4;
    id->fw_minor = 226;
    id->esp_fw_major = 3;
    id->esp_fw_minor = 87;
    id->max_ampere = 13;
    id->fix_water_supply = 0;
    id->ble = 0;
    id->delay = -1;
}

void machine_config_default(machine_config_t *c) {
    machine_identity_default(&c->identity);
    c->finishing_hold_ms = MACHINE_DEFAULT_FINISHING_HOLD_MS;
    c->shot_window_ms = MACHINE_DEFAULT_SHOT_WINDOW_MS;
}

const char *abort_cause_code(abort_cause_t c) {
    return c == ABORT_SHOT_TIMED_OUT ? "shot_timed_out" : "shot_aborted_by_user";
}

const char *abort_cause_text(abort_cause_t c) {
    // machine.rs:288,291
    return c == ABORT_SHOT_TIMED_OUT ? "no result reported in time" : "aborted by user";
}

const char *machine_err_code(machine_err_t e) {
    switch (e) {
        case MACHINE_OK: return "ok";
        case MACHINE_ERR_NOT_RUNNING: return "shot_not_running";
        case MACHINE_ERR_ALREADY_RUNNING: return "shot_already_running";
        case MACHINE_ERR_NO_WEIGHT: return "shot_weight_missing";
    }
    return "unknown";
}

bool machine_e64_in_range(uint32_t time_ms) {
    // machine.rs:149-151 with GrinderRange::E64
    return time_ms > MACHINE_MIN_EXTRACTION_TIME_MS && time_ms <= MACHINE_MAX_EXTRACTION_TIME_MS;
}

// ------------------------------------------------------------------ machine

static uint32_t clamp_ms(int64_t d) {
    if (d < 0) return 0;
    if (d > (int64_t)UINT32_MAX) return UINT32_MAX;
    return (uint32_t)d;
}

static machine_event_t *push(machine_t *m, int64_t at_ms, machine_ev_kind_t kind) {
    m->seq++;
    machine_event_t *e = &m->events[m->seq % MACHINE_EVENT_CAP];
    memset(e, 0, sizeof *e);
    e->seq = m->seq;
    e->at_ms = at_ms;
    e->kind = kind;
    return e;
}

void machine_init(machine_t *m, const machine_config_t *cfg) {
    memset(m, 0, sizeof *m);
    if (cfg) m->config = *cfg;
    else machine_config_default(&m->config);
    if (m->config.finishing_hold_ms < MACHINE_MIN_FINISHING_HOLD_MS)
        m->config.finishing_hold_ms = MACHINE_MIN_FINISHING_HOLD_MS;
    if (m->config.finishing_hold_ms > MACHINE_MAX_FINISHING_HOLD_MS)
        m->config.finishing_hold_ms = MACHINE_MAX_FINISHING_HOLD_MS;
    mako_state_default(&m->ready);
    m->phase = MACHINE_READY;
}

void machine_set_shot_window(machine_t *m, uint32_t window_ms) {
    // machine.rs:408-410: a running shot keeps its start
    m->config.shot_window_ms = window_ms;
}

bool machine_recipe_weight(const machine_t *m, double *out_g) {
    // machine.rs:438-441
    if (!m->has_last_grind || !m->last_grind.has_target) return false;
    double g = m->last_grind.target_weight_g;
    if (!isfinite(g) || g <= 0.0) return false;
    if (out_g) *out_g = g;
    return true;
}

static void begin(machine_t *m, int64_t now_ms, bool requested_by_grinder) {
    // machine.rs:653-671
    m->phase = MACHINE_RUNNING;
    m->since_ms = now_ms;
    m->requested_by_grinder = requested_by_grinder;
    push(m, now_ms, MEV_SHOT_STARTED)->started.requested_by_grinder = requested_by_grinder;
}

static void abort_shot(machine_t *m, int64_t now_ms, abort_cause_t cause) {
    // machine.rs:673-681
    uint32_t running = clamp_ms(now_ms - m->since_ms);
    m->phase = MACHINE_FINISHING;
    m->until_ms = now_ms + m->config.finishing_hold_ms;
    m->result.time_ms = running;
    m->result.weight_g = 0.0;
    m->aborted = true;
    m->finishing_seen = false;
    machine_event_t *e = push(m, now_ms, MEV_SHOT_ABORTED);
    e->aborted.cause = cause;
    e->aborted.running_ms = running;
}

void machine_tick(machine_t *m, int64_t now_ms) {
    // machine.rs:589-604
    if (m->phase == MACHINE_RUNNING) {
        if (m->config.shot_window_ms && now_ms - m->since_ms >= (int64_t)m->config.shot_window_ms)
            abort_shot(m, now_ms, ABORT_SHOT_TIMED_OUT);
    } else if (m->phase == MACHINE_FINISHING && now_ms >= m->until_ms &&
               (m->finishing_seen || now_ms >= m->until_ms + (int64_t)MACHINE_MAX_UNSEEN_HOLD_MS)) {
        m->phase = MACHINE_READY;
        push(m, now_ms, MEV_SHOT_FINISHED);
    }
}

machine_phase_t machine_phase(machine_t *m, int64_t now_ms) {
    machine_tick(m, now_ms);
    return m->phase;
}

void machine_on_grind_result(machine_t *m, int64_t now_ms, const grind_result_t *g) {
    // machine.rs:453-456
    m->last_grind = *g;
    m->has_last_grind = true;
    machine_event_t *e = push(m, now_ms, MEV_GRINDER_GRIND_FINISHED);
    e->grind.has_target = g->has_target;
    e->grind.target_weight_g = g->target_weight_g;
    if (g->has_filter) strcpy(e->grind.filter, g->filter);
}

void machine_on_start_request(machine_t *m, int64_t now_ms, bool has_script_id, int64_t script_id) {
    // machine.rs:462-476
    machine_tick(m, now_ms);
    if (m->phase == MACHINE_RUNNING) return;
    m->has_range = true;
    machine_event_t *e = push(m, now_ms, MEV_GRINDER_SHOT_REQUESTED);
    e->requested.has_script_id = has_script_id;
    e->requested.script_id = script_id;
    begin(m, now_ms, true);
}

machine_err_t machine_start_shot(machine_t *m, int64_t now_ms) {
    // machine.rs:514-521
    machine_tick(m, now_ms);
    if (m->phase == MACHINE_RUNNING) return MACHINE_ERR_ALREADY_RUNNING;
    begin(m, now_ms, false);
    return MACHINE_OK;
}

machine_err_t machine_report_shot(machine_t *m, int64_t now_ms, shot_result_t r) {
    // machine.rs:532-568
    machine_tick(m, now_ms);
    if (m->phase != MACHINE_RUNNING) return MACHINE_ERR_NOT_RUNNING;
    // Never send 0 g: without a weight, use the recipe's (machine.rs:542-551).
    if (!(isfinite(r.weight_g) && r.weight_g > 0.0)) {
        if (!machine_recipe_weight(m, &r.weight_g)) return MACHINE_ERR_NO_WEIGHT;
    }
    int64_t since = m->since_ms;
    m->phase = MACHINE_FINISHING;
    m->until_ms = now_ms + m->config.finishing_hold_ms;
    m->result = r;
    m->aborted = false;
    m->finishing_seen = false;
    m->last_shot = r;
    m->has_last_shot = true;
    machine_event_t *e = push(m, now_ms, MEV_SHOT_REPORTED);
    e->reported.result = r;
    e->reported.in_grinder_range = m->has_range ? (machine_e64_in_range(r.time_ms) ? 1 : 0) : -1;
    e->reported.running_ms = clamp_ms(now_ms - since);
    return MACHINE_OK;
}

machine_err_t machine_abort(machine_t *m, int64_t now_ms) {
    // machine.rs:572-585
    machine_tick(m, now_ms);
    if (m->phase != MACHINE_RUNNING) return MACHINE_ERR_NOT_RUNNING;
    abort_shot(m, now_ms, ABORT_SHOT_ABORTED_BY_USER);
    return MACHINE_OK;
}

void machine_mako(machine_t *m, int64_t now_ms, mako_state_t *s) {
    // machine.rs:607-640
    machine_tick(m, now_ms);
    *s = m->ready;
    switch (m->phase) {
        case MACHINE_READY:
            s->extraction_status = EXTRACTION_STATUS_IDLE;
            if (m->has_last_shot) {
                s->real_extraction_time_ms = m->last_shot.time_ms;
                s->flow_meter_volume_ml = m->last_shot.weight_g;
            }
            break;
        case MACHINE_RUNNING:
            s->status = MA_STATUS_ACTIVE_SERVING;
            s->extraction_status = EXTRACTION_STATUS_BREWING;
            s->real_extraction_time_ms = clamp_ms(now_ms - m->since_ms);
            s->flow_meter_volume_ml = 0.0;
            break;
        case MACHINE_FINISHING:
            s->status = MA_STATUS_ACTIVE_FINISHING;
            s->extraction_status = m->aborted ? EXTRACTION_STATUS_USER_ABORT : EXTRACTION_STATUS_FINISHED;
            s->real_extraction_time_ms = m->result.time_ms;
            s->flow_meter_volume_ml = m->result.weight_g;
            break;
    }
}

void machine_poll(machine_t *m, int64_t now_ms, mako_state_t *s) {
    machine_mako(m, now_ms, s);
    if (m->phase == MACHINE_FINISHING) m->finishing_seen = true;
}

size_t machine_events_after(const machine_t *m, uint32_t after, machine_event_t *out, size_t max) {
    uint32_t first = after + 1;
    if (m->seq >= MACHINE_EVENT_CAP && first < m->seq - MACHINE_EVENT_CAP + 1)
        first = m->seq - MACHINE_EVENT_CAP + 1;
    size_t n = 0;
    for (uint32_t s = first; s <= m->seq && n < max; s++) out[n++] = m->events[s % MACHINE_EVENT_CAP];
    return n;
}
