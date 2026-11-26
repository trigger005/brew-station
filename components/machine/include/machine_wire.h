// SPDX-License-Identifier: AGPL-3.0-or-later
// The JSON bodies of the link between the E64 WS and the machine it syncs with, with the exact keys.
// Ported from gbs-anywhere src/protocol/wire.rs and src/protocol/mod.rs.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "machine.h"

#ifdef __cplusplus
extern "C" {
#endif

// mod.rs:37-40: the endpoints the grinder calls
#define WIRE_PATH_MACHINE "/api/v2/machine"
#define WIRE_PATH_MAKO "/api/v2/mako"
#define WIRE_PATH_BREWRATIO "/api/v2/brewratio"
#define WIRE_PATH_SCRIPTS_EXECUTE "/api/v2/scripts/execute"
// mod.rs:46: the ID the grinder posts on the knob press
#define WIRE_START_BREW_SCRIPT_ID 9
// grinder_api.rs:30: the grinder's writes are a few dozen bytes
#define WIRE_MAX_BODY_BYTES 4096
// wire.rs:323: the longest text kept from a grinder field, in characters
#define WIRE_MAX_TEXT_CHARS 64

// wire.rs:275-288 GrinderRequest
typedef enum {
    WREQ_IDENTITY,      // GET /api/v2/machine
    WREQ_POLL_MAKO,     // GET /api/v2/mako
    WREQ_GRIND_RESULT,  // POST /api/v2/brewratio
    WREQ_START_BREW,    // POST /api/v2/scripts/execute
    WREQ_MALFORMED,     // a known write whose body did not parse as JSON
    WREQ_OTHER,         // anything else (answered with {})
} wire_req_kind_t;

typedef struct {
    wire_req_kind_t kind;
    grind_result_t grind;  // WREQ_GRIND_RESULT
    bool has_script_id;    // WREQ_START_BREW
    int64_t script_id;
} wire_request_t;

// wire.rs:291-311. `path` without the query; trailing '/' are ignored, the
// method is matched case-insensitively.
void wire_classify(const char *method, const char *path, const uint8_t *body, size_t len,
                    wire_request_t *out);

// Whether path is one of the grinder's (/api/v2 and below), grinder_api.rs:57-61.
bool wire_grinder_path(const char *path);

// JSON writers. Keys come sorted, as serde_json writes a struct turned into a
// Value without its preserve_order feature (gbs-anywhere's Cargo.toml has
// none, and grinder_api.rs:136-137 serves to_value output); numbers as
// serde_json writes them (a float always with a '.', e.g. 93.0).
// Return the length written, or a value >= size if it did not fit.
int wire_mako_json(const mako_state_t *s, char *buf, size_t size);
int wire_identity_json(const machine_identity_t *id, char *buf, size_t size);

// A double as serde_json writes an f64: shortest text that reads back the
// same, with ".0" added to whole numbers; NaN/inf as null.
int wire_fmt_f64(double v, char *buf, size_t size);

#ifdef __cplusplus
}
#endif
