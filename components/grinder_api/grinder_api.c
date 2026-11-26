// SPDX-License-Identifier: AGPL-3.0-or-later
// The E64 WS's side of the link, after gbs-anywhere src/server/grinder_api.rs:
// every path goes through one handler so nothing the grinder sends is missed,
// unknown requests get {}, and requests with browser headers or bodies over
// 4 KiB are refused, so a web page can't act as the grinder.
#include "grinder_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "machine_wire.h"
#include "sdkconfig.h"

static const char *TAG = "grinder";

#define TICK_MS 500

static SemaphoreHandle_t s_lock;
static machine_t s_m;
static QueueHandle_t s_events;
static uint32_t s_posted_seq;  // last machine event seq sent to s_events

static char s_grinder_ip[16];
static int64_t s_grinder_at_ms = -1;
static uint32_t s_polls, s_requests, s_refused, s_seq;
static int64_t s_reported_at_ms = -1;
static uint32_t s_dropped;  // machine events the queue had no room for  // when the last result went to the machine

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// Posts machine events not sent yet. Call with s_lock held.
static void drain_events(void) {
    // Static, not on the caller's stack (~2 KB; web_push overflowed): s_lock
    // is held, so one buffer is enough.
    static machine_event_t ev[MACHINE_EVENT_CAP];
    size_t n = machine_events_after(&s_m, s_posted_seq, ev, MACHINE_EVENT_CAP);
    for (size_t i = 0; i < n; i++) {
        if (ev[i].kind == MEV_SHOT_REPORTED) s_reported_at_ms = ev[i].at_ms;
        if (s_events && xQueueSend(s_events, &ev[i], 0) != pdTRUE) {
            s_dropped++;
            ESP_LOGW(TAG, "Machine event %lu dropped (queue full)", (unsigned long)ev[i].seq);
        }
        s_posted_seq = ev[i].seq;
        s_seq++;
    }
}

static void tick_cb(void *arg) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    machine_tick(&s_m, now_ms());
    drain_events();
    xSemaphoreGive(s_lock);
}

// ------------------------------------------------------------------ handler

static void peer_ip(httpd_req_t *req, char *out, size_t size) {
    out[0] = 0;
    struct sockaddr_in6 addr;
    socklen_t len = sizeof addr;
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &len) != 0) return;
    if (addr.sin6_family == AF_INET6) {
        // IPv4-mapped (::ffff:a.b.c.d) when the server listens on IPv6
        struct in_addr a4;
        memcpy(&a4, &addr.sin6_addr.un.u32_addr[3], sizeof a4);
        inet_ntoa_r(a4, out, size);
    } else {
        inet_ntoa_r(((struct sockaddr_in *)&addr)->sin_addr, out, size);
    }
}

// grinder_api.rs / browser.rs:120: Origin with every browser write,
// Sec-Fetch-Site over HTTPS.
static bool from_browser(httpd_req_t *req) {
    return httpd_req_get_hdr_value_len(req, "Origin") > 0 ||
           httpd_req_get_hdr_value_len(req, "Sec-Fetch-Site") > 0;
}

// For the serial log: printable ASCII only, so a request can't forge a line.
static void printable(const char *in, size_t len, char *out, size_t size) {
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < size; i++) {
        unsigned char c = (unsigned char)in[i];
        out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    out[n] = 0;
}

static esp_err_t refuse(httpd_req_t *req, const char *ip, const char *status, const char *why) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_refused++;
    s_seq++;
    xSemaphoreGive(s_lock);
    char uri[96];
    printable(req->uri, strlen(req->uri), uri, sizeof uri);
    printf("GRINDER %lld %s %s %s refused: %s\n", (long long)now_ms(), ip,
           http_method_str(req->method), uri, why);
    httpd_resp_set_status(req, status);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_any(httpd_req_t *req) {
    char ip[16];
    peer_ip(req, ip, sizeof ip);

    if (from_browser(req)) return refuse(req, ip, "403 Forbidden", "browser headers (Origin or Sec-Fetch-Site)");
    if (req->content_len > WIRE_MAX_BODY_BYTES) return refuse(req, ip, "413 Payload Too Large", "a body over 4 KiB");

    // httpd runs handlers one at a time on its own task: one static buffer.
    static char body[WIRE_MAX_BODY_BYTES + 1];
    // At most one retry (recv timeout 2 s, web.c): a client that announces a
    // body and stops sending would otherwise hold the one httpd task, and the
    // grinder's polls, for as long as it keeps the socket open. ~4 s stays
    // inside the 6 s Finishing hold.
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 1) continue;
        if (r == HTTPD_SOCK_ERR_TIMEOUT) return refuse(req, ip, "408 Request Timeout", "a body that stopped coming");
        if (r <= 0) return refuse(req, ip, "400 Bad Request", "a broken request body");
        got += r;
    }
    body[got] = 0;

    // The path without the query.
    char path[96];
    size_t plen = strcspn(req->uri, "?");
    if (plen >= sizeof path) plen = sizeof path - 1;
    memcpy(path, req->uri, plen);
    path[plen] = 0;

    wire_request_t xr;
    const char *method = http_method_str(req->method);
    wire_classify(method, path, (const uint8_t *)body, got, &xr);

    char reply[512] = "{}";
    int64_t t = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (wire_grinder_path(path)) s_requests++;
    switch (xr.kind) {
        case WREQ_IDENTITY:
            wire_identity_json(&s_m.config.identity, reply, sizeof reply);
            break;
        case WREQ_POLL_MAKO: {
            mako_state_t s;
            machine_mako(&s_m, t, &s);
            wire_mako_json(&s, reply, sizeof reply);
            s_polls++;
            break;
        }
        case WREQ_GRIND_RESULT:
            machine_on_grind_result(&s_m, t, &xr.grind);
            break;
        case WREQ_START_BREW:
            machine_on_start_request(&s_m, t, xr.has_script_id, xr.script_id);
            break;
        case WREQ_MALFORMED:
        case WREQ_OTHER:
            break;
    }
    // grinder_api.rs:124-130: only the grinder's own requests tell where it is.
    if (xr.kind <= WREQ_START_BREW) {
        strlcpy(s_grinder_ip, ip, sizeof s_grinder_ip);
        s_grinder_at_ms = t;
    }
    s_seq++;
    drain_events();
    xSemaphoreGive(s_lock);

    // Serial log of every request and reply, for debugging the grinder link.
    char logbody[160], uri[96];
    printable(body, got, logbody, sizeof logbody);
    printable(req->uri, strlen(req->uri), uri, sizeof uri);
    printf("GRINDER %lld %s %s %s%s%s%s -> %s\n", (long long)t, ip, method, uri, got ? " " : "", logbody,
           xr.kind == WREQ_MALFORMED ? " (unparsable)" : "", reply);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, reply);
}

// ------------------------------------------------------------------ API

esp_err_t grinder_api_start(httpd_handle_t server, QueueHandle_t events) {
    s_lock = xSemaphoreCreateMutex();
    s_events = events;
    machine_config_t cfg;
    machine_config_default(&cfg);
    cfg.finishing_hold_ms = CONFIG_CS_FINISHING_HOLD_S * 1000u;
    cfg.shot_window_ms = CONFIG_CS_SHOT_WINDOW_S * 1000u;
    machine_init(&s_m, &cfg);

    const esp_timer_create_args_t targs = {.callback = tick_cb, .name = "grinder_tick"};
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, TICK_MS * 1000));

    if (!server) {
        ESP_LOGW(TAG, "No HTTP server: the grinder can't reach the machine");
        return ESP_ERR_INVALID_STATE;
    }
    // Needs the server's uri_match_fn = httpd_uri_match_wildcard.
    const httpd_uri_t any = {.uri = "/*", .method = HTTP_ANY, .handler = h_any};
    esp_err_t err = httpd_register_uri_handler(server, &any);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Grinder API on port 80 (hold %u s, shot window %u s)", CONFIG_CS_FINISHING_HOLD_S,
             CONFIG_CS_SHOT_WINDOW_S);
    return ESP_OK;
}

void grinder_api_get_status(grinder_api_status_t *o) {
    memset(o, 0, sizeof *o);
    if (!s_lock) return;
    int64_t t = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    mako_state_t s;
    machine_mako(&s_m, t, &s);  // ticks too
    drain_events();
    o->phase = s_m.phase;
    o->grind_blocked = mako_grind_blocked(&s);
    if (s_m.phase == MACHINE_RUNNING) {
        o->running_ms = s.real_extraction_time_ms;
        o->requested_by_grinder = s_m.requested_by_grinder;
    } else if (s_m.phase == MACHINE_FINISHING) {
        o->result = s_m.result;
        o->aborted = s_m.aborted;
        o->finishing_left_ms = s_m.until_ms > t ? (uint32_t)(s_m.until_ms - t) : 0;
    }
    o->has_recipe = machine_recipe_weight(&s_m, &o->recipe_g);
    if (s_m.has_last_grind && s_m.last_grind.has_filter) strlcpy(o->filter, s_m.last_grind.filter, sizeof o->filter);
    o->has_last_shot = s_m.has_last_shot;
    o->last_shot = s_m.last_shot;
    o->last_shot_age_ms = s_reported_at_ms < 0 ? -1 : t - s_reported_at_ms;
    strlcpy(o->grinder_ip, s_grinder_ip, sizeof o->grinder_ip);
    o->grinder_age_ms = s_grinder_at_ms < 0 ? -1 : t - s_grinder_at_ms;
    o->polls = s_polls;
    o->requests = s_requests;
    o->refused = s_refused;
    o->dropped = s_dropped;
    o->finishing_hold_ms = s_m.config.finishing_hold_ms;
    o->shot_window_ms = s_m.config.shot_window_ms;
    o->seq = s_seq;
    xSemaphoreGive(s_lock);
}

machine_err_t grinder_api_report_shot(uint32_t time_ms, double weight_g) {
    if (!s_lock) return MACHINE_ERR_NOT_RUNNING;
    shot_result_t r = {time_ms, weight_g};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    machine_err_t e = machine_report_shot(&s_m, now_ms(), r);
    drain_events();
    xSemaphoreGive(s_lock);
    return e;
}

machine_err_t grinder_api_abort(void) {
    if (!s_lock) return MACHINE_ERR_NOT_RUNNING;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    machine_err_t e = machine_abort(&s_m, now_ms());
    drain_events();
    xSemaphoreGive(s_lock);
    return e;
}
