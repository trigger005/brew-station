// SPDX-License-Identifier: AGPL-3.0-or-later
// Varia AKU frame parser and command builder. Pure C, no I/O: builds on the
// host for unit tests (test/host) as well as in the firmware.
//
// Frame: FA <type> <len> <data[len]> <xor>, xor = XOR of <type> .. last data byte.
// Sources: refs/esp-arduino-ble-scales/src/scales/varia.cpp:118-200 (GaggiMate),
//          refs/Brew-Controller/lib/EspressoScaleProtocol/src/Varia.cpp:5-60.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AKU_FRAME_HEADER 0xFA

typedef enum {
    AKU_TYPE_WEIGHT = 0x01,       // varia.h:7
    AKU_TYPE_TARE = 0x82,         // varia.h:8
    AKU_TYPE_BATTERY = 0x85,      // varia.h:9
    AKU_TYPE_TIMER = 0x87,        // varia.h:10
    AKU_TYPE_TIMER_START = 0x88,  // varia.h:11
    AKU_TYPE_TIMER_STOP = 0x89,   // varia.h:12
    AKU_TYPE_TIMER_RESET = 0x8A,  // varia.h:13
} aku_type_t;

typedef struct {
    uint8_t type;  // aku_type_t, or an unknown type byte
    union {
        float weight_g;       // AKU_TYPE_WEIGHT
        uint8_t battery_pct;  // AKU_TYPE_BATTERY
        uint16_t timer_s;     // AKU_TYPE_TIMER (whole seconds)
        uint8_t event_arg;    // TARE / TIMER_START / STOP / RESET: the data byte
    };
    const uint8_t *raw;  // points into the parsed buffer, valid during the callback
    uint8_t raw_len;
} aku_frame_t;

typedef struct {
    uint32_t frames;     // valid frames delivered
    uint32_t bad_xor;    // checksum mismatch
    uint32_t bad_len;    // known type with an unexpected length
    uint32_t truncated;  // notification ended mid-frame
    uint32_t garbage;    // byte where a header (FA) was expected
    uint32_t unknown;    // valid checksum, unknown type (delivered too)
} aku_parse_stats_t;

typedef void (*aku_frame_cb_t)(const aku_frame_t *frame, void *ctx);

// Parses every frame in one notification and calls cb for each valid one.
// A notification can contain several frames (GaggiMate varia.cpp:118).
// Stops at the first byte that can't start a frame. stats may be NULL.
// Returns the number of valid frames.
size_t aku_parse(const uint8_t *data, size_t len, aku_frame_cb_t cb, void *ctx,
                 aku_parse_stats_t *stats);

// Writes FA <cmd> 01 <arg> <cmd^01^arg> into out. Returns 5.
size_t aku_cmd_build(uint8_t cmd, uint8_t arg, uint8_t out[5]);

typedef enum {
    AKU_CMD_TARE,
    AKU_CMD_TIMER_START,
    AKU_CMD_TIMER_STOP,
    AKU_CMD_TIMER_RESET,
} aku_cmd_t;

// The exact 5 bytes to send for cmd. Stop and reset are Brew-Controller's,
// verified on an AKU Micro (2026-11-24).
const uint8_t *aku_cmd_bytes(aku_cmd_t cmd);

const char *aku_type_name(uint8_t type);

#ifdef __cplusplus
}
#endif
