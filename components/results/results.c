// SPDX-License-Identifier: AGPL-3.0-or-later
#include "results.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "results";

#define NVS_NS "cs"
#define KEY_BOOTS "boots"
#define KEY_RING "results"

// Saved as one blob; `n` counts entries ever added.
typedef struct {
    uint32_t n;
    result_entry_t e[RESULTS_MAX];
} ring_t;

static ring_t s_ring;
static uint32_t s_boot;
static volatile uint8_t s_next_src;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_q;

static void save_task(void *arg) {
    result_entry_t e;
    for (;;) {
        if (xQueueReceive(s_q, &e, portMAX_DELAY) != pdTRUE) continue;
        ring_t copy;
        portENTER_CRITICAL(&s_mux);
        s_ring.e[s_ring.n % RESULTS_MAX] = e;
        s_ring.n++;
        copy = s_ring;
        portEXIT_CRITICAL(&s_mux);
        nvs_handle_t h;
        esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_blob(h, KEY_RING, &copy, sizeof copy);
            if (err == ESP_OK) err = nvs_commit(h);
            nvs_close(h);
        }
        if (err != ESP_OK) ESP_LOGW(TAG, "Saving a result: %s", esp_err_to_name(err));
    }
}

esp_err_t results_start(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_get_u32(h, KEY_BOOTS, &s_boot);
        s_boot++;
        nvs_set_u32(h, KEY_BOOTS, s_boot);
        size_t len = sizeof s_ring;
        // A blob of another size (an older layout) is dropped.
        if (nvs_get_blob(h, KEY_RING, &s_ring, &len) != ESP_OK || len != sizeof s_ring) memset(&s_ring, 0, sizeof s_ring);
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "NVS: %s", esp_err_to_name(err));
    }
    s_q = xQueueCreate(4, sizeof(result_entry_t));
    if (!s_q || xTaskCreate(save_task, "results", 3072, NULL, 2, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Boot %lu, %lu saved results", (unsigned long)s_boot,
             (unsigned long)(s_ring.n < RESULTS_MAX ? s_ring.n : RESULTS_MAX));
    return ESP_OK;
}

void results_set_source(result_src_t src) { s_next_src = (uint8_t)src; }

void results_add(result_kind_t kind, uint32_t time_ms, double weight_g) {
    if (!s_q) return;
    result_entry_t e = {
        .boot = s_boot,
        .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000),
        .time_ms = time_ms,
        .weight_g = (float)weight_g,
        .kind = (uint8_t)kind,
        .src = s_next_src,
    };
    s_next_src = RESULT_SRC_UNKNOWN;
    if (xQueueSend(s_q, &e, 0) != pdTRUE) ESP_LOGW(TAG, "Result not saved (queue full)");
}

uint32_t results_boot(void) { return s_boot; }

int results_get(result_entry_t *out, int max) {
    portENTER_CRITICAL(&s_mux);
    uint32_t n = s_ring.n;
    int k = 0;
    for (uint32_t i = n; i > 0 && k < max && n - i < RESULTS_MAX; i--) out[k++] = s_ring.e[(i - 1) % RESULTS_MAX];
    portEXIT_CRITICAL(&s_mux);
    return k;
}

const char *result_kind_code(result_kind_t k) {
    return k == RESULT_GOT ? "got" : k == RESULT_GOT_ABORT ? "got_abort" : "unseen";
}

const char *result_src_code(result_src_t s) {
    switch (s) {
        case RESULT_SRC_HAND: return "hand";
        case RESULT_SRC_AUTO_BOARD: return "auto_board";
        case RESULT_SRC_AUTO_SCALE: return "auto_scale";
        case RESULT_SRC_TARGET_BOARD: return "target_board";
        case RESULT_SRC_TARGET_SCALE: return "target_scale";
        default: return "unknown";
    }
}
