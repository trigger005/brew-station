// SPDX-License-Identifier: AGPL-3.0-or-later
// Brew Station: live AKU weight over BLE (M2) and the espresso machine the
// E64 WS syncs with (M1), on serial and a web page.
//
// Serial output, one line per event (timestamps = ms since boot, esp_timer):
//   AKU_RAW <t_ms> <hex>             every notification as received (for replay)
//   AKU <t_ms> weight <g>            parsed weight
//   AKU <t_ms> battery <pct> | timer <s> | <event> <arg>
//   AKU <t_ms> connected <name> | disconnected <reason>
//   GRINDER <t_ms> <ip> <method> <uri> [body] -> <reply>   every grinder request
//   MACHINE <t_ms> <event> ...       state machine events (grind, knob press, shot)
//   AUTO <t_ms> ...                  shot detector: armed, progress, done
// BOOT button: single click = tare, double click = timer start.
// RGB LED: blue = searching for the scale, green = connected.
#include <stdio.h>

#include "aku.h"
#include "autoshot.h"
#include "board.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "net.h"
#include "nvs_flash.h"
#include "ota.h"
#include "web.h"
#include "grinder_api.h"

static const char *TAG = "main";

static QueueHandle_t s_aku_queue;
static QueueHandle_t s_machine_queue;

static void print_ts(int64_t t_us) {
    printf("%lld.%03lld", (long long)(t_us / 1000), (long long)(t_us % 1000));
}

static void aku_event_task(void *arg) {
    aku_event_t ev;
    for (;;) {
        if (xQueueReceive(s_aku_queue, &ev, portMAX_DELAY) != pdTRUE) continue;
        switch (ev.kind) {
            case AKU_EV_RAW:
                printf("AKU_RAW ");
                print_ts(ev.t_us);
                putchar(' ');
                for (int i = 0; i < ev.raw.len; i++) printf("%02X", ev.raw.bytes[i]);
                putchar('\n');
                break;

            case AKU_EV_FRAME:
                printf("AKU ");
                print_ts(ev.t_us);
                switch (ev.frame.type) {
                    case AKU_TYPE_WEIGHT:
                        printf(" weight %.2f\n", (double)ev.frame.weight_g);
                        autoshot_weight(ev.t_us / 1000, ev.frame.weight_g);
                        break;
                    case AKU_TYPE_BATTERY:
                        printf(" battery %u\n", ev.frame.battery_pct);
                        web_log("Battery %u%%", ev.frame.battery_pct);
                        break;
                    case AKU_TYPE_TIMER: printf(" timer %u\n", ev.frame.timer_s); break;
                    default: {
                        char hex[3 * sizeof ev.frame.raw + 1] = "";
                        for (int i = 0; i < ev.frame.raw_len; i++)
                            snprintf(hex + 3 * i, 4, "%02X ", ev.frame.raw[i]);
                        printf(" %s %s\n", aku_type_name(ev.frame.type), hex);
                        web_log("Scale %s frame: %s", aku_type_name(ev.frame.type), hex);
                        break;
                    }
                }
                break;

            case AKU_EV_CONNECTED:
                printf("AKU ");
                print_ts(ev.t_us);
                printf(" connected %s\n", ev.conn.name);
                web_log("Scale connected: %s", ev.conn.name);
                board_led_set(0, 24, 0);
                break;

            case AKU_EV_DISCONNECTED:
                printf("AKU ");
                print_ts(ev.t_us);
                printf(" disconnected 0x%x\n", ev.conn.reason);
                web_log("Scale disconnected (reason 0x%x)", ev.conn.reason);
                board_led_set(0, 0, 24);
                break;
        }
    }
}

#if CONFIG_CS_KNOB_SCALE_ACTIONS
// Write-without-response commands back to back; a short gap keeps them apart.
static esp_err_t scale_cmd(aku_cmd_t cmd, const char *name) {
    esp_err_t err = aku_send(cmd);
    if (err != ESP_OK) ESP_LOGW(TAG, "Scale %s: %s", name, esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(60));
    return err;
}
#endif

static void machine_event_task(void *arg) {
    machine_event_t ev;
    for (;;) {
        if (xQueueReceive(s_machine_queue, &ev, portMAX_DELAY) != pdTRUE) continue;
        printf("MACHINE ");
        print_ts(ev.at_ms * 1000);
        switch (ev.kind) {
            case MEV_GRINDER_GRIND_FINISHED:
                if (ev.grind.has_target) {
                    printf(" grind_finished target %.2f g filter \"%s\"\n", ev.grind.target_weight_g, ev.grind.filter);
                    web_log("Grinder: GbS grind done, target %.1f g", ev.grind.target_weight_g);
                } else {
                    printf(" grind_finished no target, filter \"%s\"\n", ev.grind.filter);
                    web_log("Grinder: grind done, no target weight");
                }
                break;
            case MEV_GRINDER_SHOT_REQUESTED:
                printf(" shot_requested script %lld\n", ev.requested.has_script_id ? (long long)ev.requested.script_id : -1LL);
                web_log("Grinder: knob pressed, start the shot");
            {
                bool tared = false;
#if CONFIG_CS_KNOB_SCALE_ACTIONS
                // Knob press (README: "A shot", step 2): tare, reset the timer, watch.
                tared = scale_cmd(AKU_CMD_TARE, "tare") == ESP_OK;
                scale_cmd(AKU_CMD_TIMER_RESET, "timer reset");
#endif
                autoshot_arm(ev.at_ms, tared);
#if CONFIG_CS_KNOB_SCALE_ACTIONS && !CONFIG_CS_TIMING_MODE_DROPS
                // Timing mode 1 counts from the knob press (mode 3: from the first drops).
                scale_cmd(AKU_CMD_TIMER_START, "timer start");
#endif
                break;
            }
            case MEV_SHOT_STARTED:
                printf(" shot_started %s\n", ev.started.requested_by_grinder ? "by_grinder" : "flush");
                break;
            case MEV_SHOT_REPORTED: {
                const char *range = ev.reported.in_grinder_range < 0 ? "unknown"
                                    : ev.reported.in_grinder_range ? "yes" : "no";
                printf(" shot_reported %lu ms %.2f g in_range %s running %lu ms\n",
                       (unsigned long)ev.reported.result.time_ms, ev.reported.result.weight_g, range,
                       (unsigned long)ev.reported.running_ms);
                autoshot_disarm();
                web_log("Shot reported: %.1f s, %.1f g%s", ev.reported.result.time_ms / 1000.0,
                        ev.reported.result.weight_g,
                        ev.reported.in_grinder_range == 0 ? " (outside 10-80 s: grinder won't adjust)" : "");
#if CONFIG_CS_KNOB_SCALE_ACTIONS
                scale_cmd(AKU_CMD_TIMER_STOP, "timer stop");
#endif
                break;
            }
            case MEV_SHOT_ABORTED:
                printf(" shot_aborted %s after %lu ms\n", abort_cause_code(ev.aborted.cause),
                       (unsigned long)ev.aborted.running_ms);
                autoshot_disarm();
                web_log("Shot aborted: %s", abort_cause_text(ev.aborted.cause));
#if CONFIG_CS_KNOB_SCALE_ACTIONS
                scale_cmd(AKU_CMD_TIMER_STOP, "timer stop");
#endif
                break;
            case MEV_SHOT_FINISHED:
                printf(" shot_finished (ready)\n");
                autoshot_disarm();
                break;
        }
    }
}

static void button_task(void *arg) {
    for (;;) {
        switch (board_button_poll()) {
            case BOARD_CLICK_SINGLE:
                ESP_LOGI(TAG, "Button: tare -> %s", esp_err_to_name(aku_send(AKU_CMD_TARE)));
                web_log("Button: tare");
                break;
            case BOARD_CLICK_DOUBLE:
                ESP_LOGI(TAG, "Button: timer start -> %s", esp_err_to_name(aku_send(AKU_CMD_TIMER_START)));
                web_log("Button: timer start");
                break;
            default:
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Enough to take the next update over WiFi.
static bool healthy(void) { return net_connected() && web_httpd() != NULL; }

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(board_init());
    board_led_set(0, 0, 24);
    ESP_LOGI(TAG, "GbS AKU Bridge (M1+M2) on %s", board_name());

    s_aku_queue = xQueueCreate(64, sizeof(aku_event_t));
    xTaskCreate(aku_event_task, "aku_events", 8192, NULL, 5, NULL);  // reports shots: web_log, printf %f
    xTaskCreate(button_task, "button", 3072, NULL, 3, NULL);
    autoshot_start();
    s_machine_queue = xQueueCreate(16, sizeof(machine_event_t));
    xTaskCreate(machine_event_task, "machine_events", 6144, NULL, 5, NULL);

    if (net_start() == ESP_OK) {
        web_start();
    }
    // After web_start, before grinder_api: exact routes must come before its "/*".
    ota_start(web_httpd());
    grinder_api_start(web_httpd(), s_machine_queue);
    ESP_ERROR_CHECK(aku_start(s_aku_queue));
    // A new firmware from OTA stays only if WiFi and the server come up.
    ota_confirm_when_healthy(healthy);
}
