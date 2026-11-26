// SPDX-License-Identifier: AGPL-3.0-or-later
// LOLIN (Wemos) S3 Mini: WS2812 RGB LED on GPIO47, BOOT button on GPIO0.
#include "board.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

static const char *TAG = "board";

#define PIN_RGB_LED 47
#define PIN_BUTTON 0

#define DEBOUNCE_MS 30
#define DOUBLE_CLICK_MS 350

static led_strip_handle_t s_led;

esp_err_t board_init(void) {
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn));

    led_strip_config_t strip = {
        .strip_gpio_num = PIN_RGB_LED,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t err = led_strip_new_rmt_device(&strip, &rmt, &s_led);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED init failed: %s", esp_err_to_name(err));
        s_led = NULL;
    }
    board_led_set(0, 0, 0);
    return ESP_OK;
}

const char *board_name(void) { return "LOLIN S3 Mini"; }

void board_led_set(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_led) return;
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}

board_click_t board_button_poll(void) {
    static bool stable_pressed, last_raw;
    static uint32_t last_change_ms, last_release_ms;
    static uint8_t clicks;

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool raw = gpio_get_level(PIN_BUTTON) == 0;

    if (raw != last_raw) {
        last_raw = raw;
        last_change_ms = now;
    }
    if (now - last_change_ms >= DEBOUNCE_MS && raw != stable_pressed) {
        stable_pressed = raw;
        if (!stable_pressed) {  // released: one click done
            clicks++;
            last_release_ms = now;
            if (clicks >= 2) {
                clicks = 0;
                return BOARD_CLICK_DOUBLE;
            }
        }
    }
    if (clicks == 1 && !stable_pressed && now - last_release_ms > DOUBLE_CLICK_MS) {
        clicks = 0;
        return BOARD_CLICK_SINGLE;
    }
    return BOARD_CLICK_NONE;
}
