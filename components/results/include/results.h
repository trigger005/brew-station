// SPDX-License-Identifier: AGPL-3.0-or-later
// The last shot results as the grinder got them, kept in flash (NVS) so an
// update, a crash or a power cut doesn't lose them. For checking what the
// grinder adjusted from. The board has no wall clock: each entry carries the
// boot it happened in and the uptime then.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RESULTS_MAX 12

typedef enum {
    RESULT_GOT = 0,      // a grinder poll got the result (MA_STATUS 4, finished)
    RESULT_GOT_ABORT,    // a grinder poll got an aborted shot (skipped by the grinder)
    RESULT_UNSEEN,       // the result was offered but no grinder poll got it
} result_kind_t;

typedef enum {
    RESULT_SRC_UNKNOWN = 0,
    RESULT_SRC_HAND,          // Send result on the page
    RESULT_SRC_AUTO_BOARD,    // end of flow, board clock
    RESULT_SRC_AUTO_SCALE,    // end of flow, scale timer
    RESULT_SRC_TARGET_BOARD,  // target reached, board clock
    RESULT_SRC_TARGET_SCALE,  // target reached, scale timer
} result_src_t;

typedef struct {
    uint32_t boot;      // boot count when it happened
    uint32_t uptime_s;  // uptime then
    uint32_t time_ms;
    float weight_g;
    uint8_t kind;  // result_kind_t
    uint8_t src;   // result_src_t
    uint8_t pad[2];
} result_entry_t;

// After nvs_flash_init: loads the saved results and counts this boot.
esp_err_t results_start(void);

// Who made the next result (call right before reporting it).
void results_set_source(result_src_t src);
// Saves an entry (any task; the flash write runs on its own task).
void results_add(result_kind_t kind, uint32_t time_ms, double weight_g);

uint32_t results_boot(void);
// Newest first; returns how many were copied.
int results_get(result_entry_t *out, int max);

const char *result_kind_code(result_kind_t k);  // "got" | "got_abort" | "unseen"
const char *result_src_code(result_src_t s);    // "hand" | "auto_board" | ... | "unknown"

#ifdef __cplusplus
}
#endif
