// SPDX-License-Identifier: AGPL-3.0-or-later
// OTA into the other app slot (partitions.csv: ota_0 / ota_1), then reboot.
// Guards: a token from menuconfig, no browser requests (same rule as the
// grinder routes), size within the slot, and the image must be this project's
// (esp_app_desc_t project_name). esp_ota_end checks the image checksum.
#include "ota.h"

#include <stdbool.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "grinder_api.h"

static const char *TAG = "ota";

#define CHUNK 4096
// Each is a recv timeout (2 s, web.c): up to 2 min of stalls in all. WiFi
// slows down a lot while the AKU is connected (BLE shares the radio).
#define MAX_TIMEOUTS 60

// Same length and every byte compared, so the reply time doesn't leak the token.
static bool token_ok(const char *got) {
    const char *want = CONFIG_CS_OTA_TOKEN;
    size_t gl = strlen(got), wl = strlen(want);
    unsigned diff = gl ^ wl;
    for (size_t i = 0; i < gl; i++) diff |= (unsigned char)got[i] ^ (unsigned char)want[i % (wl ? wl : 1)];
    return wl > 0 && diff == 0;
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *msg) {
    ESP_LOGW(TAG, "%s: %s", status, msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

static void reboot_cb(void *arg) { esp_restart(); }

static esp_err_t h_ota(httpd_req_t *req) {
    if (strlen(CONFIG_CS_OTA_TOKEN) == 0) return reply(req, "403 Forbidden", "OTA is off: no token set in menuconfig\n");
    if (httpd_req_get_hdr_value_len(req, "Origin") > 0 || httpd_req_get_hdr_value_len(req, "Sec-Fetch-Site") > 0)
        return reply(req, "403 Forbidden", "not from a browser\n");

    char tok[72] = "";
    if (httpd_req_get_hdr_value_len(req, "X-OTA-Token") >= (int)sizeof tok ||
        httpd_req_get_hdr_value_str(req, "X-OTA-Token", tok, sizeof tok) != ESP_OK || !token_ok(tok))
        return reply(req, "401 Unauthorized", "bad or missing X-OTA-Token\n");

    // The reboot would lose a running shot and the result the grinder is
    // about to read; the upload alone also holds the httpd task for seconds.
    grinder_api_status_t x;
    grinder_api_get_status(&x);
    if (x.phase != MACHINE_READY) return reply(req, "409 Conflict", "a shot is running or its result is on show: try again in a moment\n");

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return reply(req, "500 Internal Server Error", "no OTA slot\n");
    const size_t hdr = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (req->content_len < hdr + sizeof(esp_app_desc_t)) return reply(req, "400 Bad Request", "body too small for an app image\n");
    if (req->content_len > part->size) return reply(req, "413 Payload Too Large", "image bigger than the OTA slot\n");

    static char buf[CHUNK];  // httpd runs one handler at a time
    esp_ota_handle_t h = 0;
    bool begun = false;
    size_t got = 0;
    int timeouts = 0;
    int64_t t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "Receiving %u bytes into %s", (unsigned)req->content_len, part->label);

    while (got < req->content_len) {
        size_t want = req->content_len - got;
        // The first read holds the image header and app description in one go.
        if (!begun) want = hdr + sizeof(esp_app_desc_t);
        else if (want > CHUNK) want = CHUNK;
        size_t have = 0;
        while (have < want) {
            int r = httpd_req_recv(req, buf + have, want - have);
            if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= MAX_TIMEOUTS) continue;  // total, not in a row
            if (r <= 0) {
                if (begun) esp_ota_abort(h);
                return reply(req, "400 Bad Request", "upload broke off\n");
            }
            have += r;
        }
        if (!begun) {
            const esp_image_header_t *ih = (const esp_image_header_t *)buf;
            const esp_app_desc_t *d = (const esp_app_desc_t *)(buf + hdr);
            const esp_app_desc_t *mine = esp_app_get_description();
            if (ih->magic != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD)
                return reply(req, "400 Bad Request", "not an ESP app image (send build/<project>.bin)\n");
            if (ih->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) return reply(req, "400 Bad Request", "image is for another chip\n");
            if (strncmp(d->project_name, mine->project_name, sizeof d->project_name) != 0)
                return reply(req, "400 Bad Request", "image is from another project\n");
            ESP_LOGI(TAG, "Image %.32s %.32s, built %.16s %.16s", d->project_name, d->version, d->date, d->time);
            esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
            if (e != ESP_OK) return reply(req, "500 Internal Server Error", "esp_ota_begin failed\n");
            begun = true;
        }
        if (esp_ota_write(h, buf, have) != ESP_OK) {
            esp_ota_abort(h);
            return reply(req, "500 Internal Server Error", "flash write failed\n");
        }
        got += have;
    }

    esp_err_t e = esp_ota_end(h);
    if (e != ESP_OK)
        return reply(req, "400 Bad Request",
                     e == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed validation\n" : "esp_ota_end failed\n");
    if (esp_ota_set_boot_partition(part) != ESP_OK) return reply(req, "500 Internal Server Error", "set boot slot failed\n");

    ESP_LOGI(TAG, "Wrote %u bytes in %lld ms, rebooting into %s", (unsigned)got,
             (long long)((esp_timer_get_time() - t0) / 1000), part->label);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK, rebooting\n");

    // Reboot from a timer so the reply reaches the client first (500 ms lost it).
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = {.callback = reboot_cb, .name = "ota_reboot"};
    if (!t && esp_timer_create(&a, &t) != ESP_OK) esp_restart();
    esp_timer_start_once(t, 1500 * 1000);
    return ESP_OK;
}

esp_err_t ota_start(httpd_handle_t server) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running from %s, %s", run ? run->label : "?", esp_app_get_description()->version);
    if (!server) return ESP_ERR_INVALID_STATE;
    if (strlen(CONFIG_CS_OTA_TOKEN) == 0) ESP_LOGW(TAG, "No OTA token set: updates over WiFi are off");
    const httpd_uri_t u = {.uri = "/api/ota", .method = HTTP_POST, .handler = h_ota};
    return httpd_register_uri_handler(server, &u);
}

void ota_confirm_when_healthy(bool (*healthy)(void)) {
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return;
    ESP_LOGW(TAG, "New firmware on probation: confirming once WiFi is up (%d s)", CONFIG_CS_OTA_CONFIRM_S);
    for (int i = 0; i < CONFIG_CS_OTA_CONFIRM_S * 10; i++) {
        if (healthy()) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "New firmware confirmed");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGE(TAG, "Not healthy in time: rolling back to the previous firmware");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}
