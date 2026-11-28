// SPDX-License-Identifier: AGPL-3.0-or-later
#include "web.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "aku.h"
#include "build_info.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "autoshot.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "results.h"
#include "grinder_api.h"

static const char *TAG = "web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

#define PUSH_PERIOD_MS 100   // max live update rate
#define HEARTBEAT_MS 1000    // push at least this often (ages, uptime)
#define MAX_CLIENTS 8
#define STATUS_JSON_MAX 2048

static httpd_handle_t s_server;

// The last log lines, so a page opened later (or after a missed shot) still
// shows them: GET /api/log. RAM only, gone after a restart.
#define LOG_LINES 80
#define LOG_LINE_MAX 160
typedef struct {
    int64_t uptime_ms;
    char line[LOG_LINE_MAX];
} log_entry_t;
static log_entry_t *s_ring;  // PSRAM, web_start
static uint32_t s_ring_n;    // lines ever written
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

// ------------------------------------------------------------------ JSON

static const char *phase_name(machine_phase_t p) {
    return p == MACHINE_RUNNING ? "running" : p == MACHINE_FINISHING ? "finishing" : "ready";
}

// JSON string body: quotes and backslashes escaped, control characters dropped.
static void json_text(const char *in, char *out, size_t size) {
    size_t n = 0;
    for (; *in && n + 3 < size; in++) {
        if (*in == '"' || *in == '\\') out[n++] = '\\';
        if ((unsigned char)*in >= 0x20) out[n++] = *in;
    }
    out[n] = 0;
}

static int machine_json(char *buf, size_t size) {
    grinder_api_status_t x;
    grinder_api_get_status(&x);
    char filter[140], recipe[24] = "null", last[96] = "null";
    json_text(x.filter, filter, sizeof filter);
    if (x.has_recipe) snprintf(recipe, sizeof recipe, "%.2f", x.recipe_g);
    if (x.has_last_shot)
        snprintf(last, sizeof last, "{\"time_ms\":%lu,\"weight_g\":%.2f,\"age_ms\":%lld}",
                 (unsigned long)x.last_shot.time_ms, x.last_shot.weight_g, (long long)x.last_shot_age_ms);
    return snprintf(
        buf, size,
        "{\"phase\":\"%s\",\"running_ms\":%lu,\"requested_by_grinder\":%s,"
        "\"result\":{\"time_ms\":%lu,\"weight_g\":%.2f,\"aborted\":%s},\"finishing_left_ms\":%lu,"
        "\"recipe_g\":%s,\"filter\":\"%s\",\"last_shot\":%s,"
        "\"grind_blocked\":\"%s\",\"grinder\":{\"ip\":\"%s\",\"age_ms\":%lld},"
        "\"polls\":%lu,\"requests\":%lu,\"refused\":%lu,\"dropped\":%lu,"
        "\"finishing_hold_ms\":%lu,\"shot_window_ms\":%lu}",
        phase_name(x.phase), (unsigned long)x.running_ms, x.requested_by_grinder ? "true" : "false",
        (unsigned long)x.result.time_ms, x.result.weight_g, x.aborted ? "true" : "false",
        (unsigned long)x.finishing_left_ms, recipe, filter, last, grind_blocked_code(x.grind_blocked),
        x.grinder_ip, (long long)x.grinder_age_ms, (unsigned long)x.polls, (unsigned long)x.requests,
        (unsigned long)x.refused, (unsigned long)x.dropped, (unsigned long)x.finishing_hold_ms, (unsigned long)x.shot_window_ms);
}

static int auto_json(char *buf, size_t size) {
    autoshot_status_t a;
    autoshot_get_status(&a);
    char last[128] = "null", target[24] = "null";
    if (a.has_last)
        snprintf(last, sizeof last, "{\"time_ms\":%lu,\"weight_g\":%.2f,\"by\":\"%s\",\"time_from\":\"%s\"}",
                 (unsigned long)a.last_time_ms, a.last_grams, report_mode_code(a.last_by),
                 a.last_from_scale ? "scale" : "board");
    if (a.armed && a.watching == REPORT_TARGET) snprintf(target, sizeof target, "%.2f", a.target_g);
    return snprintf(buf, size,
                    "{\"mode\":\"%s\",\"watching\":\"%s\",\"target_g\":%s,\"timing_mode\":%d,\"armed\":%s,"
                    "\"progress\":\"%s\",\"grams\":%.2f,\"last\":%s}",
                    report_mode_code(a.mode), a.armed ? report_mode_code(a.watching) : "", target, a.timing_mode,
                    a.armed ? "true" : "false", shot_progress_name(a.progress), a.grams, last);
}

// Why the board last started, for Diagnostics (a hang or crash shows here).
static const char *reset_text(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power on";
        case ESP_RST_EXT: return "reset pin";
        case ESP_RST_SW: return net_restarted_by_watchdog() ? "WiFi watchdog (WiFi down)" : "restart (update or command)";
        case ESP_RST_PANIC: return "crash (core dump saved)";
        case ESP_RST_INT_WDT: return "interrupt watchdog (crash)";
        case ESP_RST_TASK_WDT: return "task watchdog (a task was stuck)";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_BROWNOUT: return "power dip (brownout)";
        case ESP_RST_USB: return "USB (serial port opened or flashed)";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        default: return "unknown";
    }
}

static int status_json(char *buf, size_t size) {
    aku_status_t s;
    aku_get_status(&s);
    int64_t now = esp_timer_get_time();
    long long age_ms = s.weight_t_us ? (now - s.weight_t_us) / 1000 : -1;
    long long lost_ms = s.lost_t_us && !s.connected ? (now - s.lost_t_us) / 1000 : -1;
    long long fast_left_ms = s.search == AKU_SEARCH_FAST && s.fast_until_us > now ? (s.fast_until_us - now) / 1000 : 0;
    char machine[768];
    machine_json(machine, sizeof machine);
    char fw[160];
    snprintf(fw, sizeof fw, "{\"version\":\"%.32s\",\"build\":%d,\"built\":\"%s\",\"reset\":\"%s\"}",
             esp_app_get_description()->version, BUILD_NUMBER, BUILD_TIME, reset_text());
    char autos[320];
    auto_json(autos, sizeof autos);
    // From the BLE advertisement: escaped, a quote in it would break the JSON.
    char name[72], addr[40];
    json_text(s.name, name, sizeof name);
    json_text(s.addr, addr, sizeof addr);
    return snprintf(
        buf, size,
        "{\"t\":\"status\",\"uptime_ms\":%lld,"
        "\"scale\":{\"connected\":%s,\"name\":\"%s\",\"addr\":\"%s\",\"rssi\":%d,"
        "\"search\":\"%s\",\"fast_left_ms\":%lld,\"lost_ms\":%lld,"
        "\"weight_g\":%.2f,\"weight_age_ms\":%lld,\"battery_pct\":%d,\"timer_s\":%d,"
        "\"rate_hz\":%.1f,\"notif_ms\":{\"min\":%lu,\"avg\":%lu,\"max\":%lu},"
        "\"notifications\":%lu,\"weights_per_notif_max\":%lu,\"connects\":%lu,"
        "\"parse\":{\"frames\":%lu,\"bad_xor\":%lu,\"bad_len\":%lu,\"truncated\":%lu,"
        "\"garbage\":%lu,\"unknown\":%lu}},"
        "\"wifi\":{\"ip\":\"%s\",\"rssi\":%d,\"host\":\"%s\"},\"machine\":%s,\"auto\":%s,\"fw\":%s}",
        (long long)(now / 1000), s.connected ? "true" : "false", name, addr, s.rssi,
        aku_search_code((aku_search_t)s.search), fast_left_ms, lost_ms,
        (double)s.weight_g, age_ms, s.battery_pct, s.timer_s, (double)s.weight_rate_hz,
        (unsigned long)s.notif_interval_min_ms, (unsigned long)s.notif_interval_avg_ms,
        (unsigned long)s.notif_interval_max_ms, (unsigned long)s.notifications,
        (unsigned long)s.weights_per_notif_max,
        (unsigned long)s.connects, (unsigned long)s.stats.frames, (unsigned long)s.stats.bad_xor,
        (unsigned long)s.stats.bad_len, (unsigned long)s.stats.truncated,
        (unsigned long)s.stats.garbage, (unsigned long)s.stats.unknown,
        net_ip(), net_rssi(), net_hostname(), machine, autos, fw);
}

// ------------------------------------------------------------------ WebSocket push

static void ws_broadcast_work(void *arg) {
    char *msg = arg;
    size_t fds_n = MAX_CLIENTS;
    int fds[MAX_CLIENTS];
    if (httpd_get_client_list(s_server, &fds_n, fds) == ESP_OK) {
        httpd_ws_frame_t frame = {
            .final = true,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)msg,
            .len = strlen(msg),
        };
        for (size_t i = 0; i < fds_n; i++) {
            if (httpd_ws_get_fd_info(s_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                // A client that can't take it (asleep, out of range) is
                // dropped, so it doesn't hold up the grinder's requests.
                if (httpd_ws_send_frame_async(s_server, fds[i], &frame) != ESP_OK)
                    httpd_sess_trigger_close(s_server, fds[i]);
            }
        }
    }
    free(msg);
}

static void broadcast(const char *msg) {
    if (!s_server) return;
    char *copy = strdup(msg);
    if (!copy) return;
    if (httpd_queue_work(s_server, ws_broadcast_work, copy) != ESP_OK) free(copy);
}

static void push_task(void *arg) {
    static char buf[STATUS_JSON_MAX];
    uint32_t last_seq = UINT32_MAX, last_mseq = UINT32_MAX, last_aseq = UINT32_MAX;
    int64_t last_push = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PUSH_PERIOD_MS));
        aku_status_t s;
        aku_get_status(&s);
        int64_t now = esp_timer_get_time();
        grinder_api_status_t x;
        grinder_api_get_status(&x);
        autoshot_status_t a;
        autoshot_get_status(&a);
        if (s.seq == last_seq && x.seq == last_mseq && a.seq == last_aseq && now - last_push < HEARTBEAT_MS * 1000LL)
            continue;
        last_seq = s.seq;
        last_mseq = x.seq;
        last_aseq = a.seq;
        last_push = now;
        if (status_json(buf, sizeof buf) < (int)sizeof buf) broadcast(buf);
    }
}

void web_log(const char *fmt, ...) {
    char line[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    int64_t up = esp_timer_get_time() / 1000;
    printf("LOG %lld %s\n", (long long)up, line);
    if (s_ring) {
        portENTER_CRITICAL(&s_ring_mux);
        log_entry_t *e = &s_ring[s_ring_n % LOG_LINES];
        e->uptime_ms = up;
        memcpy(e->line, line, sizeof line);
        s_ring_n++;
        portEXIT_CRITICAL(&s_ring_mux);
    }

    // JSON-escape quotes and backslashes; drop control characters.
    char msg[384];
    int n = snprintf(msg, sizeof msg, "{\"t\":\"log\",\"uptime_ms\":%lld,\"msg\":\"", (long long)up);
    for (const char *p = line; *p && n < (int)sizeof msg - 4; p++) {
        if (*p == '"' || *p == '\\') msg[n++] = '\\';
        if ((unsigned char)*p >= 0x20) msg[n++] = *p;
    }
    msg[n++] = '"';
    msg[n++] = '}';
    msg[n] = 0;
    broadcast(msg);
}

// ------------------------------------------------------------------ handlers

static esp_err_t h_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start - 1);  // EMBED_TXTFILES adds a NUL
}

static esp_err_t h_status(httpd_req_t *req) {
    static char buf[STATUS_JSON_MAX];  // httpd runs one handler at a time
    status_json(buf, sizeof buf);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

// {"uptime_ms":..,"log":[{"uptime_ms":..,"msg":".."},...]}, oldest first.
static esp_err_t h_log(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char head[48];
    snprintf(head, sizeof head, "{\"uptime_ms\":%lld,\"log\":[", (long long)(esp_timer_get_time() / 1000));
    httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);
    if (s_ring) {
        portENTER_CRITICAL(&s_ring_mux);
        uint32_t end = s_ring_n;
        portEXIT_CRITICAL(&s_ring_mux);
        uint32_t start = end > LOG_LINES ? end - LOG_LINES : 0;
        for (uint32_t i = start; i < end; i++) {
            log_entry_t e = {0};
            portENTER_CRITICAL(&s_ring_mux);
            bool gone = s_ring_n - i > LOG_LINES;  // overwritten meanwhile
            if (!gone) e = s_ring[i % LOG_LINES];
            portEXIT_CRITICAL(&s_ring_mux);
            if (gone) continue;
            e.line[LOG_LINE_MAX - 1] = 0;
            char text[2 * LOG_LINE_MAX], item[2 * LOG_LINE_MAX + 48];
            json_text(e.line, text, sizeof text);
            snprintf(item, sizeof item, "%s{\"uptime_ms\":%lld,\"msg\":\"%s\"}", i > start ? "," : "",
                     (long long)e.uptime_ms, text);
            if (httpd_resp_send_chunk(req, item, HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
        }
    }
    httpd_resp_send_chunk(req, "]}", 2);
    return httpd_resp_send_chunk(req, NULL, 0);
}

// The results the grinder got, saved in flash (results.h), newest first.
static esp_err_t h_results(httpd_req_t *req) {
    result_entry_t r[RESULTS_MAX];
    int n = results_get(r, RESULTS_MAX);
    static char buf[160 + RESULTS_MAX * 128];  // httpd runs one handler at a time
    int len = snprintf(buf, sizeof buf, "{\"boot\":%lu,\"uptime_s\":%lld,\"results\":[", (unsigned long)results_boot(),
                       (long long)(esp_timer_get_time() / 1000000));
    for (int i = 0; i < n && len < (int)sizeof buf; i++)
        len += snprintf(buf + len, sizeof buf - len,
                        "%s{\"boot\":%lu,\"uptime_s\":%lu,\"time_ms\":%lu,\"weight_g\":%.1f,\"kind\":\"%s\",\"src\":\"%s\"}",
                        i ? "," : "", (unsigned long)r[i].boot, (unsigned long)r[i].uptime_s, (unsigned long)r[i].time_ms,
                        (double)r[i].weight_g, result_kind_code(r[i].kind), result_src_code(r[i].src));
    if (len < (int)sizeof buf) snprintf(buf + len, sizeof buf - len, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, buf);
}

static esp_err_t h_ws(httpd_req_t *req) {
    if (req->method == HTTP_GET) {  // handshake: someone opened the page
        if (aku_wake()) web_log("Page opened: looking for the scale");
        return ESP_OK;
    }
    // Drain anything the page sends; commands go through the REST routes.
    httpd_ws_frame_t f = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) return err;
    if (f.len >= 256) return ESP_FAIL;  // nothing big is expected: close rather than desync
    if (f.len > 0) {
        uint8_t tmp[256];
        f.payload = tmp;
        err = httpd_ws_recv_frame(req, &f, f.len);
    }
    return err;
}

static bool query_value(httpd_req_t *req, const char *key, char *out, size_t size) {
    char q[96];
    return httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
           httpd_query_key_value(q, key, out, size) == ESP_OK;
}

static esp_err_t reply_json(httpd_req_t *req, const char *status, const char *body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// A browser sends Origin with every cross-site POST, and a plain POST with
// the arguments in the query needs no preflight: without this, any page open
// on a phone on this network could abort a shot. The status page's own
// requests carry an Origin equal to the Host they go to; curl sends none.
// (gbs-anywhere src/server/browser.rs refuses writes from other sites too.)
static bool cross_site(httpd_req_t *req) {
    size_t olen = httpd_req_get_hdr_value_len(req, "Origin");
    if (olen == 0) return false;
    char origin[96], host[72];
    if (olen >= sizeof origin || httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof origin) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) != ESP_OK)
        return true;
    const char *o = strstr(origin, "://");
    o = o ? o + 3 : origin;
    return strcasecmp(o, host) != 0;
}

#define REFUSE_CROSS_SITE(req)                                                                         \
    do {                                                                                               \
        if (cross_site(req))                                                                           \
            return reply_json(req, "403 Forbidden", "{\"error\":\"not from another site\",\"code\":\"cross_site\"}"); \
    } while (0)

// POST /api/scale/cmd?c=tare|start|stop|reset|connect
static esp_err_t h_cmd(httpd_req_t *req) {
    REFUSE_CROSS_SITE(req);
    char c[16] = "";
    query_value(req, "c", c, sizeof c);

    if (!strcmp(c, "connect")) {  // Connect scale: search non-stop again
        aku_wake();
        web_log("Connect scale: looking for the scale");
        return reply_json(req, "200 OK", "{}");
    }
    aku_cmd_t cmd;
    if (!strcmp(c, "tare")) cmd = AKU_CMD_TARE;
    else if (!strcmp(c, "start")) cmd = AKU_CMD_TIMER_START;
    else if (!strcmp(c, "stop")) cmd = AKU_CMD_TIMER_STOP;
    else if (!strcmp(c, "reset")) cmd = AKU_CMD_TIMER_RESET;
    else return reply_json(req, "400 Bad Request", "{\"error\":\"unknown command\",\"code\":\"bad_command\"}");

    esp_err_t err = aku_send(cmd);
    web_log("TX %s%s", c, err == ESP_OK ? "" : " failed");
    if (err == ESP_ERR_INVALID_STATE)
        return reply_json(req, "409 Conflict", "{\"error\":\"scale not connected\",\"code\":\"not_connected\"}");
    if (err != ESP_OK) return reply_json(req, "500 Internal Server Error", "{\"error\":\"write failed\",\"code\":\"write_failed\"}");
    return reply_json(req, "200 OK", "{}");
}

static esp_err_t machine_error(httpd_req_t *req, machine_err_t e) {
    char body[96];
    snprintf(body, sizeof body, "{\"error\":\"%s\",\"code\":\"%s\"}",
             e == MACHINE_ERR_NO_WEIGHT ? "no weight given, and the grinder sent no recipe weight"
                                        : "no shot is running",
             machine_err_code(e));
    return reply_json(req, "409 Conflict", body);
}

// POST /api/shot/report?time_s=28.5[&weight_g=36.2]
// The measured shot, typed on the page (M1). No weight: the recipe's.
static esp_err_t h_shot_report(httpd_req_t *req) {
    REFUSE_CROSS_SITE(req);
    char ts[16] = "", ws[16] = "";
    query_value(req, "time_s", ts, sizeof ts);
    query_value(req, "weight_g", ws, sizeof ws);
    char *end;
    double t = strtod(ts, &end);
    // machine.rs:105-109: 0 to 600 s
    if (!*ts || *end || !(t >= 0.0 && t <= MACHINE_MAX_TYPED_TIME_MS / 1000.0))
        return reply_json(req, "400 Bad Request", "{\"error\":\"time_s must be 0 to 600\",\"code\":\"bad_time\"}");
    double w = 0.0;
    if (*ws) {
        w = strtod(ws, &end);
        if (*end || !(w >= 0.0 && w < 10000.0))
            return reply_json(req, "400 Bad Request", "{\"error\":\"bad weight_g\",\"code\":\"bad_weight\"}");
    }
    results_set_source(RESULT_SRC_HAND);
    machine_err_t e = grinder_api_report_shot((uint32_t)(t * 1000.0 + 0.5), w);
    if (e != MACHINE_OK) return machine_error(req, e);
    return reply_json(req, "200 OK", "{}");
}

// POST /api/shot/abort
// POST /api/mode?m=auto|target|manual: the report mode, kept in NVS.
static esp_err_t h_mode(httpd_req_t *req) {
    REFUSE_CROSS_SITE(req);
    char m[12] = "";
    report_mode_t mode;
    if (!query_value(req, "m", m, sizeof m) || !report_mode_parse(m, &mode))
        return reply_json(req, "400 Bad Request", "{\"error\":\"m must be auto, target or manual\",\"code\":\"bad_mode\"}");
    grinder_api_status_t x;
    grinder_api_get_status(&x);
    if (x.phase == MACHINE_RUNNING)
        return reply_json(req, "409 Conflict", "{\"error\":\"not during a shot\",\"code\":\"shot_running\"}");
    esp_err_t err = autoshot_set_mode(mode);
    if (err == ESP_ERR_INVALID_STATE)
        return reply_json(req, "409 Conflict", "{\"error\":\"not during a shot\",\"code\":\"shot_running\"}");
    if (err != ESP_OK)
        return reply_json(req, "500 Internal Server Error", "{\"error\":\"could not set the mode\",\"code\":\"mode_failed\"}");
    return reply_json(req, "200 OK", "{}");
}

static esp_err_t h_shot_abort(httpd_req_t *req) {
    REFUSE_CROSS_SITE(req);
    machine_err_t e = grinder_api_abort();
    if (e != MACHINE_OK) return machine_error(req, e);
    return reply_json(req, "200 OK", "{}");
}

// ------------------------------------------------------------------ start

esp_err_t web_start(void) {
    s_ring = heap_caps_calloc(LOG_LINES, sizeof *s_ring, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) ESP_LOGW(TAG, "No memory for the log history");
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 14;
    // Exact routes here; grinder_api adds "/*" last for everything else.
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 6144;
    // One send may block the single httpd task this long (default 5 s): a
    // half-dead WebSocket client then stalls the grinder's polls less.
    cfg.send_wait_timeout = 2;
    // Same for a stalled request body (grinder_api.c retries once; OTA counts its own).
    cfg.recv_wait_timeout = 2;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/scale", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/log", .method = HTTP_GET, .handler = h_log},
        {.uri = "/api/results", .method = HTTP_GET, .handler = h_results},
        {.uri = "/api/scale/cmd", .method = HTTP_POST, .handler = h_cmd},
        {.uri = "/api/shot/report", .method = HTTP_POST, .handler = h_shot_report},
        {.uri = "/api/shot/abort", .method = HTTP_POST, .handler = h_shot_abort},
        {.uri = "/api/mode", .method = HTTP_POST, .handler = h_mode},
        {.uri = "/ws", .method = HTTP_GET, .handler = h_ws, .is_websocket = true},
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }

    xTaskCreate(push_task, "web_push", 8192, NULL, 4, NULL);  // 4096 overflowed (status JSON)
    ESP_LOGI(TAG, "Status page on port 80, firmware %s build %d (%s)", esp_app_get_description()->version,
             BUILD_NUMBER, BUILD_TIME);
    return ESP_OK;
}

httpd_handle_t web_httpd(void) { return s_server; }
