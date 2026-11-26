// SPDX-License-Identifier: AGPL-3.0-or-later
// Unit tests for the AKU frame parser, using the example frames in GaggiMate's
// varia.cpp comments.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "aku_frame.h"

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

#define MAX_FRAMES 8
typedef struct {
    int n;
    aku_frame_t f[MAX_FRAMES];
} capture_t;

static void cap_cb(const aku_frame_t *fr, void *ctx) {
    capture_t *c = ctx;
    if (c->n < MAX_FRAMES) c->f[c->n++] = *fr;
}

static capture_t parse(const uint8_t *d, size_t n, aku_parse_stats_t *st) {
    capture_t c = {0};
    memset(st, 0, sizeof(*st));
    aku_parse(d, n, cap_cb, &c, st);
    return c;
}

static int near(float a, float b) { return fabsf(a - b) < 0.001f; }

static void test_weight_negative(void) {
    // FA 01 03 10 02 CD DD = -7.17 g
    const uint8_t d[] = {0xFA, 0x01, 0x03, 0x10, 0x02, 0xCD, 0xDD};
    aku_parse_stats_t st;
    capture_t c = parse(d, sizeof d, &st);
    CHECK(c.n == 1);
    CHECK(c.f[0].type == AKU_TYPE_WEIGHT);
    CHECK(near(c.f[0].weight_g, -7.17f));
    CHECK(st.frames == 1 && st.bad_xor == 0);
}

static void test_weight_positive(void) {
    // 18.20 g = 1820 = 0x00071C, xor = 01^03^00^07^1C = 0x19
    const uint8_t d[] = {0xFA, 0x01, 0x03, 0x00, 0x07, 0x1C, 0x19};
    aku_parse_stats_t st;
    capture_t c = parse(d, sizeof d, &st);
    CHECK(c.n == 1);
    CHECK(near(c.f[0].weight_g, 18.20f));
}

static void test_battery(void) {
    // FA 85 01 4B CF = 75 %
    const uint8_t d[] = {0xFA, 0x85, 0x01, 0x4B, 0xCF};
    aku_parse_stats_t st;
    capture_t c = parse(d, sizeof d, &st);
    CHECK(c.n == 1);
    CHECK(c.f[0].type == AKU_TYPE_BATTERY);
    CHECK(c.f[0].battery_pct == 75);
}

static void test_timer(void) {
    // GaggiMate varia.cpp:139: FA 87 02 00 02 87 = 2 s
    const uint8_t d[] = {0xFA, 0x87, 0x02, 0x00, 0x02, 0x87};
    aku_parse_stats_t st;
    capture_t c = parse(d, sizeof d, &st);
    CHECK(c.n == 1);
    CHECK(c.f[0].type == AKU_TYPE_TIMER);
    CHECK(c.f[0].timer_s == 2);
}

static void test_timer_events(void) {
    // GaggiMate varia.cpp:154-156
    const uint8_t d[] = {0xFA, 0x88, 0x01, 0x01, 0x88,
                         0xFA, 0x89, 0x01, 0x02, 0x8A,
                         0xFA, 0x8A, 0x01, 0x03, 0x88};
    aku_parse_stats_t st;
    capture_t c = parse(d, sizeof d, &st);
    CHECK(c.n == 3);
    CHECK(c.f[0].type == AKU_TYPE_TIMER_START);
    CHECK(c.f[1].type == AKU_TYPE_TIMER_STOP && c.f[1].event_arg == 0x02);
    CHECK(c.f[2].type == AKU_TYPE_TIMER_RESET && c.f[2].event_arg == 0x03);
}

static void test_multiple_frames(void) {
    // weight + battery + timer in one notification
    const uint8_t d[] = {0xFA, 0x01, 0x03, 0x10, 0x02, 0xCD, 0xDD,
                         0xFA, 0x85, 0x01, 0x4B, 0xCF,
                         0xFA, 0x87, 0x02, 0x00, 0x02, 0x87};
    aku_parse_stats_t st;
    capture_t c = parse(d, sizeof d, &st);
    CHECK(c.n == 3);
    CHECK(c.f[0].type == AKU_TYPE_WEIGHT && near(c.f[0].weight_g, -7.17f));
    CHECK(c.f[1].type == AKU_TYPE_BATTERY && c.f[1].battery_pct == 75);
    CHECK(c.f[2].type == AKU_TYPE_TIMER && c.f[2].timer_s == 2);
    CHECK(c.f[1].raw == d + 7 && c.f[1].raw_len == 5);
}

static void test_bad_xor_skips_only_that_frame(void) {
    const uint8_t d[] = {0xFA, 0x01, 0x03, 0x10, 0x02, 0xCD, 0x00,  // bad xor
                         0xFA, 0x85, 0x01, 0x4B, 0xCF};
    aku_parse_stats_t st;
    capture_t c = parse(d, sizeof d, &st);
    CHECK(c.n == 1);
    CHECK(c.f[0].type == AKU_TYPE_BATTERY);
    CHECK(st.bad_xor == 1);
}

static void test_truncated(void) {
    const uint8_t d[] = {0xFA, 0x85, 0x01, 0x4B, 0xCF, 0xFA, 0x01, 0x03, 0x10};
    aku_parse_stats_t st;
    capture_t c = parse(d, sizeof d, &st);
    CHECK(c.n == 1);
    CHECK(st.truncated == 1);
}

static void test_garbage_and_bad_len(void) {
    const uint8_t g[] = {0x00, 0xFA, 0x85, 0x01, 0x4B, 0xCF};
    aku_parse_stats_t st;
    capture_t c = parse(g, sizeof g, &st);
    CHECK(c.n == 0 && st.garbage == 1);

    // weight frame claiming len 2: known type, wrong length
    const uint8_t b[] = {0xFA, 0x01, 0x02, 0x00, 0x00, 0x03};
    c = parse(b, sizeof b, &st);
    CHECK(c.n == 0 && st.bad_len == 1);
}

static void test_unknown_type_is_skipped_by_length(void) {
    // unknown 0x40 with 2 data bytes (xor 40^02^11^22 = 0x71), then battery
    const uint8_t d[] = {0xFA, 0x40, 0x02, 0x11, 0x22, 0x71, 0xFA, 0x85, 0x01, 0x4B, 0xCF};
    aku_parse_stats_t st;
    capture_t c = parse(d, sizeof d, &st);
    CHECK(c.n == 2);
    CHECK(c.f[0].type == 0x40 && st.unknown == 1);
    CHECK(c.f[1].type == AKU_TYPE_BATTERY);
}

static void test_commands(void) {
    // Tare FA 82 01 01 82, start FA 88 01 01 88 (Brew-Controller Varia.cpp:5-6)
    const uint8_t tare[] = {0xFA, 0x82, 0x01, 0x01, 0x82};
    const uint8_t start[] = {0xFA, 0x88, 0x01, 0x01, 0x88};
    CHECK(memcmp(aku_cmd_bytes(AKU_CMD_TARE), tare, 5) == 0);
    CHECK(memcmp(aku_cmd_bytes(AKU_CMD_TIMER_START), start, 5) == 0);

    uint8_t out[5];
    aku_cmd_build(0x82, 0x01, out);
    CHECK(memcmp(out, tare, 5) == 0);

    // Stop/reset (Brew-Controller, verified on hardware) have valid checksums
    aku_cmd_build(0x89, 0x02, out);
    CHECK(memcmp(out, aku_cmd_bytes(AKU_CMD_TIMER_STOP), 5) == 0);
    aku_cmd_build(0x8A, 0x03, out);
    CHECK(memcmp(out, aku_cmd_bytes(AKU_CMD_TIMER_RESET), 5) == 0);
}

int main(void) {
    struct { const char *name; void (*fn)(void); } tests[] = {
        {"weight_negative", test_weight_negative},
        {"weight_positive", test_weight_positive},
        {"battery", test_battery},
        {"timer", test_timer},
        {"timer_events", test_timer_events},
        {"multiple_frames", test_multiple_frames},
        {"bad_xor_skips_only_that_frame", test_bad_xor_skips_only_that_frame},
        {"truncated", test_truncated},
        {"garbage_and_bad_len", test_garbage_and_bad_len},
        {"unknown_type_is_skipped_by_length", test_unknown_type_is_skipped_by_length},
        {"commands", test_commands},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = failures;
        tests[i].fn();
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
