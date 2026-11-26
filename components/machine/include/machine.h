// SPDX-License-Identifier: AGPL-3.0-or-later
// The espresso machine the E64 WS syncs with, as a pure state machine.
// Ported from gbs-anywhere src/protocol/machine.rs (E64 WS parts only).
//
//   Ready (MA_STATUS 1)
//     | grinder posts scripts/execute {"ID":9}  (knob press after a GbS grind)
//     v
//   Running (MA_STATUS 3, REAL_EXTRACTION_TIME_MS rising)
//     | machine_report_shot(time, weight)
//     v
//   Finishing (MA_STATUS 4, final time + weight)  <- the grinder takes the result here
//     | after finishing_hold_ms
//     v
//   Ready
//
// No I/O and no clock of its own: every call takes `now_ms` (any monotonic
// millisecond clock), so it runs the same on the ESP32 and in host tests.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// mod.rs:54 MIN_FINISHING_HOLD, mod.rs:58 MAX_FINISHING_HOLD
#define MACHINE_MIN_FINISHING_HOLD_MS 3000u
#define MACHINE_MAX_FINISHING_HOLD_MS 60000u
// machine.rs:83-84 MachineConfig::default
#define MACHINE_DEFAULT_FINISHING_HOLD_MS 6000u
#define MACHINE_DEFAULT_SHOT_WINDOW_MS 180000u
// mod.rs:65,69: the E64 WS adjusts from shots over 10 s, up to 80 s
#define MACHINE_MIN_EXTRACTION_TIME_MS 10000u
#define MACHINE_MAX_EXTRACTION_TIME_MS 80000u
// machine.rs:101 ShotResult::MAX_TIME: longest shot time a person can enter
#define MACHINE_MAX_TYPED_TIME_MS 600000u

// wire.rs:15-26 MA_STATUS
typedef enum {
    MA_STATUS_OFF = 0,
    MA_STATUS_ON = 1,
    MA_STATUS_ECO = 2,
    MA_STATUS_ACTIVE_SERVING = 3,
    MA_STATUS_ACTIVE_FINISHING = 4,
    MA_STATUS_TESTING = 5,
    MA_STATUS_FAULT = 6,
} ma_status_t;

// mod.rs:77-84 MA_EXTRACTION_STATUS. Only USER_ABORT means something to the
// grinder; the others are gbs-anywhere's best guess.
#define EXTRACTION_STATUS_IDLE 0
#define EXTRACTION_STATUS_BREWING 1
#define EXTRACTION_STATUS_FINISHED 1
#define EXTRACTION_STATUS_USER_ABORT 2

// mod.rs:88 HEATED_TOLERANCE_C, mod.rs:91 TANK_LEVEL_FULL
#define HEATED_TOLERANCE_C 2.0
#define TANK_LEVEL_FULL 1

// wire.rs:67-92 MakoState: the reply to GET /api/v2/mako.
typedef struct {
    uint32_t real_extraction_time_ms;  // REAL_EXTRACTION_TIME_MS
    double flow_meter_volume_ml;       // PU_SENS_FLOW_METER_VOLUME (the beverage weight)
    double bg_set_temp;                // BG_SET_TEMP
    double bg_sens_temp;               // BG_SENS_TEMP
    int64_t tank_level;                // TANK_LEVEL
    char script[32];                   // script
    char timestamp[32];                // TIMESTAMP
    int64_t extraction_status;         // MA_EXTRACTION_STATUS
    ma_status_t status;                // MA_STATUS
} mako_state_t;

// wire.rs:94-108: a switched-on, heated machine with a full tank.
void mako_state_default(mako_state_t *s);

// wire.rs:143-151 GrindBlocked
typedef enum {
    GRIND_ALLOWED = 0,
    GRIND_BLOCKED_SHOT_RUNNING,
    GRIND_BLOCKED_MACHINE_IN_STANDBY,
    GRIND_BLOCKED_MACHINE_IN_ECO,
    GRIND_BLOCKED_MACHINE_TESTING,
    GRIND_BLOCKED_MACHINE_FAULTY,
    GRIND_BLOCKED_MACHINE_WARMING_UP,
    GRIND_BLOCKED_MACHINE_TANK_EMPTY,
} grind_blocked_t;

// wire.rs:124-136: why the grinder would refuse to grind with this reply.
grind_blocked_t mako_grind_blocked(const mako_state_t *s);
const char *grind_blocked_code(grind_blocked_t b);

// wire.rs:188-228 MachineIdentity: the reply to GET /api/v2/machine.
typedef struct {
    int64_t machine_type;      // MA_TYPE
    int64_t machine_subtype;   // MA_SUBTYPE
    char serial[24];           // MA_SN
    int64_t fw_major;          // FW_VERSION_MAJOR
    int64_t fw_minor;          // FW_VERSION_MINOR
    int64_t esp_fw_major;      // ESP_FW_MAJOR
    int64_t esp_fw_minor;      // ESP_FW_MINOR
    int64_t max_ampere;        // MA_MAX_AMPERE
    int64_t fix_water_supply;  // MA_FIX_WATER_SUPPLY
    int64_t ble;               // BLE
    int64_t delay;             // MA_DELAY
} machine_identity_t;

void machine_identity_default(machine_identity_t *id);

typedef struct {
    machine_identity_t identity;
    uint32_t finishing_hold_ms;  // clamp to MIN..MAX_FINISHING_HOLD_MS (machine_init does)
    uint32_t shot_window_ms;     // 0 = no limit
} machine_config_t;

void machine_config_default(machine_config_t *c);

typedef struct {
    uint32_t time_ms;
    double weight_g;  // sent as PU_SENS_FLOW_METER_VOLUME (1 g ~ 1 ml)
} shot_result_t;

typedef enum {
    MACHINE_READY,
    MACHINE_RUNNING,
    MACHINE_FINISHING,
} machine_phase_t;

// machine.rs:248-270 AbortCause (the two an E64 WS bridge can hit)
typedef enum {
    ABORT_SHOT_ABORTED_BY_USER,
    ABORT_SHOT_TIMED_OUT,
} abort_cause_t;

const char *abort_cause_code(abort_cause_t c);  // "shot_aborted_by_user", ...
const char *abort_cause_text(abort_cause_t c);  // machine.rs:286-297

// machine.rs:176-220 MachineEvent
typedef enum {
    MEV_GRINDER_GRIND_FINISHED,   // POST /api/v2/brewratio
    MEV_GRINDER_SHOT_REQUESTED,   // POST /api/v2/scripts/execute (knob press)
    MEV_SHOT_STARTED,             // entered ACTIVE SERVING
    MEV_SHOT_REPORTED,            // result shown to the grinder (ACTIVE FINISHING)
    MEV_SHOT_ABORTED,             // ended without a result
    MEV_SHOT_FINISHED,            // back to ON
} machine_ev_kind_t;

typedef struct {
    uint32_t seq;  // starts at 1, increments per event
    int64_t at_ms;
    machine_ev_kind_t kind;
    union {
        struct {
            bool has_target;
            double target_weight_g;
            char filter[65];
        } grind;
        struct {
            bool has_script_id;
            int64_t script_id;
        } requested;
        struct {
            bool requested_by_grinder;
        } started;
        struct {
            shot_result_t result;
            int in_grinder_range;  // 1 / 0, or -1 when not known (no knob press yet)
            uint32_t running_ms;
        } reported;
        struct {
            abort_cause_t cause;
            uint32_t running_ms;
        } aborted;
    };
} machine_event_t;

#define MACHINE_EVENT_CAP 16

// The grind result from POST /api/v2/brewratio (wire.rs:234-242).
typedef struct {
    bool has_target;
    double target_weight_g;  // SYNC_BEVERAGE_WEIGHT
    bool has_filter;
    char filter[65];         // SYNC_FILTER, as text, control characters removed
} grind_result_t;

typedef enum {
    MACHINE_OK = 0,
    MACHINE_ERR_NOT_RUNNING,      // "shot_not_running"
    MACHINE_ERR_ALREADY_RUNNING,  // "shot_already_running"
    MACHINE_ERR_NO_WEIGHT,        // "shot_weight_missing"
} machine_err_t;

const char *machine_err_code(machine_err_t e);  // machine.rs:351-357

typedef struct {
    machine_config_t config;
    mako_state_t ready;  // values served while Ready (the grind gate needs them)

    machine_phase_t phase;
    int64_t since_ms;           // Running
    bool requested_by_grinder;  // Running
    int64_t until_ms;           // Finishing
    shot_result_t result;       // Finishing
    bool aborted;               // Finishing

    bool has_range;  // machine.rs:433: set by the first knob press

    bool has_last_shot;
    shot_result_t last_shot;
    bool has_last_grind;
    grind_result_t last_grind;

    machine_event_t events[MACHINE_EVENT_CAP];  // ring, oldest overwritten
    uint32_t seq;
} machine_t;

void machine_init(machine_t *m, const machine_config_t *cfg);

// Applies time-based transitions (shot window, end of the finishing hold).
// Every other call does it; call it periodically too for events without polls.
void machine_tick(machine_t *m, int64_t now_ms);

machine_phase_t machine_phase(machine_t *m, int64_t now_ms);

// POST /api/v2/brewratio
void machine_on_grind_result(machine_t *m, int64_t now_ms, const grind_result_t *g);

// POST /api/v2/scripts/execute: the brew starts at once (the grinder retries
// only a few times). Repeats while running are those retries: ignored.
void machine_on_start_request(machine_t *m, int64_t now_ms, bool has_script_id, int64_t script_id);

// A brew without a grinder request: the grinder counts it as a flush.
machine_err_t machine_start_shot(machine_t *m, int64_t now_ms);

// The measured numbers. A weight that is not > 0 becomes the recipe weight;
// with no recipe weight either, MACHINE_ERR_NO_WEIGHT and the brew goes on.
machine_err_t machine_report_shot(machine_t *m, int64_t now_ms, shot_result_t r);

// User abort (MA_EXTRACTION_STATUS 2): the grinder skips the result.
machine_err_t machine_abort(machine_t *m, int64_t now_ms);

void machine_set_shot_window(machine_t *m, uint32_t window_ms);

// The recipe's target weight, if the last grind sent a finite one > 0.
bool machine_recipe_weight(const machine_t *m, double *out_g);

// Whether the E64 WS adjusts from a shot of time_ms (machine.rs:131-151).
bool machine_e64_in_range(uint32_t time_ms);

// The typed reply to GET /api/v2/mako at now_ms.
void machine_mako(machine_t *m, int64_t now_ms, mako_state_t *out);

// Events with seq > after, oldest first; returns how many were copied.
size_t machine_events_after(const machine_t *m, uint32_t after, machine_event_t *out, size_t max);

#ifdef __cplusplus
}
#endif
