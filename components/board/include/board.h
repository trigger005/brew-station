// SPDX-License-Identifier: AGPL-3.0-or-later
// Board abstraction. Everything pin- or board-specific lives behind this header,
// so the final board (with screen) only needs a new board_<name>.c.
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_CLICK_NONE,
    BOARD_CLICK_SINGLE,
    BOARD_CLICK_DOUBLE,
} board_click_t;

esp_err_t board_init(void);
const char *board_name(void);

// Status LED (RGB). Values 0..255; keep them low, the LED is bright.
void board_led_set(uint8_t r, uint8_t g, uint8_t b);

// Call every ~10 ms. Reports a single click only after the double-click
// window has passed.
board_click_t board_button_poll(void);

#ifdef __cplusplus
}
#endif
