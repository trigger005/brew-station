// SPDX-License-Identifier: AGPL-3.0-or-later
#include "aku_frame.h"

// Expected total frame length per known type, header and xor included.
// GaggiMate varia.cpp:125 (weight 7), :140 (timer 6), :157 (timer events 5), :170 (battery 5).
// All of them equal <len> + 4, which is how unknown types are skipped.
static int expected_len(uint8_t type) {
    switch (type) {
        case AKU_TYPE_WEIGHT: return 7;
        case AKU_TYPE_TIMER: return 6;
        case AKU_TYPE_BATTERY:
        case AKU_TYPE_TARE:
        case AKU_TYPE_TIMER_START:
        case AKU_TYPE_TIMER_STOP:
        case AKU_TYPE_TIMER_RESET: return 5;
        default: return -1;
    }
}

static uint8_t xor_bytes(const uint8_t *p, size_t n) {
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= p[i];
    return x;
}

size_t aku_parse(const uint8_t *data, size_t len, aku_frame_cb_t cb, void *ctx,
                 aku_parse_stats_t *stats) {
    aku_parse_stats_t dummy = {0};
    if (!stats) stats = &dummy;
    size_t delivered = 0;
    size_t pos = 0;

    while (pos < len) {
        const uint8_t *f = data + pos;
        size_t left = len - pos;

        if (f[0] != AKU_FRAME_HEADER) {
            stats->garbage++;
            break;
        }
        if (left < 3) {
            stats->truncated++;
            break;
        }
        size_t frame_len = (size_t)f[2] + 4;  // FA type len data[len] xor
        if (left < frame_len) {
            stats->truncated++;
            break;
        }
        int want = expected_len(f[1]);
        if (want > 0 && (size_t)want != frame_len) {
            stats->bad_len++;
            break;  // can't trust the length byte, so can't find the next frame
        }
        // xor over <type> .. last data byte (varia.cpp:192-200)
        if (xor_bytes(f + 1, frame_len - 2) != f[frame_len - 1]) {
            stats->bad_xor++;
            pos += frame_len;  // length is plausible: skip just this frame
            continue;
        }

        aku_frame_t fr = {.type = f[1], .raw = f, .raw_len = (uint8_t)frame_len};
        switch (f[1]) {
            case AKU_TYPE_WEIGHT: {
                // varia.cpp:130-132: sign = bit 0x10 of byte 3, value / 100 = grams
                int32_t v = ((int32_t)(f[3] & 0x0F) << 16) | ((int32_t)f[4] << 8) | f[5];
                if (f[3] & 0x10) v = -v;
                fr.weight_g = (float)v / 100.0f;
                break;
            }
            case AKU_TYPE_BATTERY: fr.battery_pct = f[3]; break;           // varia.cpp:175
            case AKU_TYPE_TIMER: fr.timer_s = (uint16_t)((f[3] << 8) | f[4]); break;  // varia.cpp:145
            case AKU_TYPE_TARE:
            case AKU_TYPE_TIMER_START:
            case AKU_TYPE_TIMER_STOP:
            case AKU_TYPE_TIMER_RESET: fr.event_arg = f[3]; break;
            default: stats->unknown++; break;
        }
        stats->frames++;
        delivered++;
        if (cb) cb(&fr, ctx);
        pos += frame_len;
    }
    return delivered;
}

size_t aku_cmd_build(uint8_t cmd, uint8_t arg, uint8_t out[5]) {
    out[0] = AKU_FRAME_HEADER;
    out[1] = cmd;
    out[2] = 0x01;
    out[3] = arg;
    out[4] = (uint8_t)(cmd ^ 0x01 ^ arg);
    return 5;
}

// Byte sequences copied verbatim from the sources (not computed).
// Tare: Brew-Controller Varia.cpp:5, GaggiMate varia.cpp:48.
static const uint8_t CMD_TARE[5] = {0xFA, 0x82, 0x01, 0x01, 0x82};
// Start: Brew-Controller Varia.cpp:6.
static const uint8_t CMD_START[5] = {0xFA, 0x88, 0x01, 0x01, 0x88};
// Brew-Controller Varia.cpp:7-8 (also what the scale emits, GaggiMate varia.cpp:155-156).
static const uint8_t CMD_STOP[5] = {0xFA, 0x89, 0x01, 0x02, 0x8A};
static const uint8_t CMD_RESET[5] = {0xFA, 0x8A, 0x01, 0x03, 0x88};
const uint8_t *aku_cmd_bytes(aku_cmd_t cmd) {
    switch (cmd) {
        case AKU_CMD_TARE: return CMD_TARE;
        case AKU_CMD_TIMER_START: return CMD_START;
        case AKU_CMD_TIMER_STOP: return CMD_STOP;
        case AKU_CMD_TIMER_RESET: return CMD_RESET;
    }
    return CMD_TARE;
}

const char *aku_type_name(uint8_t type) {
    switch (type) {
        case AKU_TYPE_WEIGHT: return "weight";
        case AKU_TYPE_TARE: return "tare";
        case AKU_TYPE_BATTERY: return "battery";
        case AKU_TYPE_TIMER: return "timer";
        case AKU_TYPE_TIMER_START: return "timer_start";
        case AKU_TYPE_TIMER_STOP: return "timer_stop";
        case AKU_TYPE_TIMER_RESET: return "timer_reset";
        default: return "unknown";
    }
}
