// SPDX-License-Identifier: AGPL-3.0-or-later
// NimBLE central for the Varia AKU, ported from GaggiMate's NimBLE-Arduino
// driver (refs/esp-arduino-ble-scales/src/scales/varia.cpp) to the ESP-IDF
// NimBLE host. Flow: scan -> connect -> discover FFF0 -> FFF1/FFF2 -> CCCD ->
// notifications. Any failure or disconnect goes back to scanning: non-stop
// for a few minutes, then a short look every so often (or none) until woken.
#include <stdio.h>
#include <string.h>

#include "aku.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "sdkconfig.h"

static const char *TAG = "aku";

// varia.cpp:4-6
#define UUID_SVC 0xFFF0
#define UUID_NOTIFY 0xFFF1
#define UUID_WRITE 0xFFF2
#define UUID_CCCD 0x2902

#define CONNECT_TIMEOUT_MS 5000
// Scan in windows and restart: with duplicate filtering on, a missed scan
// response (the name lives there; WiFi shares the radio) would otherwise hide
// the scale for the rest of an endless scan. Restarting clears the filter.
#define SCAN_WINDOW_MS 10000
// After the fast search: look this long every CONFIG_CS_SCALE_SLOW_SEARCH_EVERY_S.
// The AKU advertises while awake, so a few seconds finds it (ours, not measured).
#define SLOW_SCAN_WINDOW_MS 3000
#define FAST_SEARCH_US ((int64_t)CONFIG_CS_SCALE_FAST_SEARCH_MIN * 60 * 1000000)

// Name prefixes, case-sensitive, as GaggiMate matches them (varia.h:58-61).
static const char *const NAME_PREFIXES[] = {"AKU MINI SCALE", "VARIA AKU", "Varia AKU", "AKU SCALE"};

static QueueHandle_t s_queue;
static uint8_t s_own_addr_type;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static aku_status_t s_st;

// Connection state (NimBLE host task only)
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start, s_svc_end;
static uint16_t s_notify_handle, s_write_handle, s_cccd_handle;
static uint8_t s_notify_props;
static bool s_connecting;
static char s_pending_name[32];
static bool s_synced;
static struct ble_npl_callout s_retry;  // next slow look, or a wake (runs on the host task)

// Rate / interval measurement
static int64_t s_last_notif_us;
static uint64_t s_interval_sum_ms;
static uint32_t s_interval_count;
static int64_t s_rate_window_start_us;
static uint32_t s_rate_window_count;

static void start_scan(void);
static void scan_next(void);
static int gap_event(struct ble_gap_event *event, void *arg);

// ------------------------------------------------------------------ helpers

static void post(const aku_event_t *ev) {
    if (s_queue && xQueueSend(s_queue, ev, 0) != pdTRUE) {
        // Consumer too slow; dropped. The parse stats still count frames.
    }
}

static void status_changed_locked(void) { s_st.seq++; }

static bool fast_now(void) {
    portENTER_CRITICAL(&s_lock);
    bool fast = esp_timer_get_time() < s_st.fast_until_us;
    portEXIT_CRITICAL(&s_lock);
    return fast;
}

static void set_search(aku_search_t m) {
    portENTER_CRITICAL(&s_lock);
    aku_search_t prev = (aku_search_t)s_st.search;
    s_st.search = (uint8_t)m;
    if (prev != m) status_changed_locked();
    portEXIT_CRITICAL(&s_lock);
    if (prev != m) {
        aku_event_t ev = {.kind = AKU_EV_SEARCH, .t_us = esp_timer_get_time()};
        ev.search.mode = (uint8_t)m;
        ev.search.prev = (uint8_t)prev;
        post(&ev);
    }
}

static bool name_matches(const uint8_t *name, uint8_t len) {
    for (size_t i = 0; i < sizeof NAME_PREFIXES / sizeof NAME_PREFIXES[0]; i++) {
        size_t n = strlen(NAME_PREFIXES[i]);
        if (len >= n && memcmp(name, NAME_PREFIXES[i], n) == 0) return true;
    }
    return false;
}

static void addr_str(const ble_addr_t *a, char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", a->val[5], a->val[4], a->val[3], a->val[2],
             a->val[1], a->val[0]);
}

static void reset_link(void) {
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_svc_start = s_svc_end = 0;
    s_notify_handle = s_write_handle = s_cccd_handle = 0;
    s_notify_props = 0;
    s_connecting = false;
}

static void fail_and_disconnect(const char *why, int rc) {
    ESP_LOGW(TAG, "%s (rc=%d), disconnecting", why, rc);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);  // DISCONNECT event restarts the scan
    } else {
        reset_link();
        start_scan();
    }
}

// ------------------------------------------------------------------ frames

// Times for the frames of one notification. All of it arrives at once, so
// with several weight frames in it, they are spread evenly over the gap since
// the previous notification instead of sharing the arrival time (the last one
// keeps it). The scale sends no clock of its own; this is an estimate.
typedef struct {
    int64_t t_us;    // arrival
    int64_t gap_us;  // since the previous notification, 0 = unknown
    int weights;     // weight frames in this notification
    int seen;        // weight frames stamped so far
} frame_times_t;

static void count_weight(const aku_frame_t *fr, void *ctx) {
    if (fr->type == AKU_TYPE_WEIGHT) (*(int *)ctx)++;
}

static void on_frame(const aku_frame_t *fr, void *ctx) {
    frame_times_t *ft = ctx;
    int64_t t_us = ft->t_us;
    if (fr->type == AKU_TYPE_WEIGHT) {
        if (ft->weights > 1 && ft->gap_us > 0) t_us -= (int64_t)(ft->weights - 1 - ft->seen) * ft->gap_us / ft->weights;
        ft->seen++;
    }
    aku_event_t ev = {.kind = AKU_EV_FRAME, .t_us = t_us};
    ev.frame.type = fr->type;
    ev.frame.raw_len = fr->raw_len < sizeof ev.frame.raw ? fr->raw_len : sizeof ev.frame.raw;
    memcpy(ev.frame.raw, fr->raw, ev.frame.raw_len);

    portENTER_CRITICAL(&s_lock);
    switch (fr->type) {
        case AKU_TYPE_WEIGHT:
            ev.frame.weight_g = fr->weight_g;
            s_st.weight_g = fr->weight_g;
            s_st.weight_t_us = t_us;
            s_rate_window_count++;
            break;
        case AKU_TYPE_BATTERY:
            ev.frame.battery_pct = fr->battery_pct;
            s_st.battery_pct = fr->battery_pct;
            break;
        case AKU_TYPE_TIMER:
            ev.frame.timer_s = fr->timer_s;
            s_st.timer_s = fr->timer_s;
            break;
        case AKU_TYPE_TIMER_RESET:  // the scale's timer is back at 0
            ev.frame.event_arg = fr->event_arg;
            s_st.timer_s = 0;
            break;
        default:
            ev.frame.event_arg = fr->event_arg;
            break;
    }
    status_changed_locked();
    portEXIT_CRITICAL(&s_lock);

    post(&ev);
}

static void on_notification(struct os_mbuf *om) {
    int64_t t_us = esp_timer_get_time();
    uint8_t buf[AKU_RAW_MAX];
    uint16_t len = OS_MBUF_PKTLEN(om);
    if (len > sizeof buf) len = sizeof buf;
    if (os_mbuf_copydata(om, 0, len, buf) != 0) return;

    aku_event_t raw = {.kind = AKU_EV_RAW, .t_us = t_us};
    raw.raw.len = (uint8_t)len;
    memcpy(raw.raw.bytes, buf, len);
    post(&raw);

    int weights = 0;
    aku_parse(buf, len, count_weight, &weights, NULL);
    // A gap over 1 s is the scale pausing, not frames held back: don't spread.
    int64_t gap = s_last_notif_us && t_us - s_last_notif_us < 1000000 ? t_us - s_last_notif_us : 0;

    portENTER_CRITICAL(&s_lock);
    s_st.notifications++;
    if ((uint32_t)weights > s_st.weights_per_notif_max) s_st.weights_per_notif_max = (uint32_t)weights;
    if (s_last_notif_us) {
        uint32_t dt = (uint32_t)((t_us - s_last_notif_us) / 1000);
        if (s_interval_count == 0 || dt < s_st.notif_interval_min_ms) s_st.notif_interval_min_ms = dt;
        if (dt > s_st.notif_interval_max_ms) s_st.notif_interval_max_ms = dt;
        s_interval_sum_ms += dt;
        s_interval_count++;
        s_st.notif_interval_avg_ms = (uint32_t)(s_interval_sum_ms / s_interval_count);
    }
    s_last_notif_us = t_us;
    if (t_us - s_rate_window_start_us >= 1000000) {
        s_st.weight_rate_hz = s_rate_window_count * 1e6f / (float)(t_us - s_rate_window_start_us);
        s_rate_window_start_us = t_us;
        s_rate_window_count = 0;
    }
    portEXIT_CRITICAL(&s_lock);

    aku_parse_stats_t delta = {0};
    frame_times_t ft = {.t_us = t_us, .gap_us = gap, .weights = weights};
    aku_parse(buf, len, on_frame, &ft, &delta);

    portENTER_CRITICAL(&s_lock);
    s_st.stats.frames += delta.frames;
    s_st.stats.bad_xor += delta.bad_xor;
    s_st.stats.bad_len += delta.bad_len;
    s_st.stats.truncated += delta.truncated;
    s_st.stats.garbage += delta.garbage;
    s_st.stats.unknown += delta.unknown;
    status_changed_locked();
    portEXIT_CRITICAL(&s_lock);
}

// ------------------------------------------------------------------ GATT discovery

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr,
                           void *arg) {
    if (err->status != 0) {
        fail_and_disconnect("CCCD write failed", err->status);
        return 0;
    }
    ESP_LOGI(TAG, "Subscribed to FFF1, scale ready");
    aku_event_t ev = {.kind = AKU_EV_CONNECTED, .t_us = esp_timer_get_time()};
    portENTER_CRITICAL(&s_lock);
    s_st.connected = true;
    s_st.connects++;
    s_st.search = AKU_SEARCH_CONNECTED;
    strlcpy(ev.conn.name, s_st.name, sizeof ev.conn.name);
    s_last_notif_us = 0;
    s_interval_sum_ms = 0;
    s_interval_count = 0;
    s_st.notif_interval_min_ms = s_st.notif_interval_max_ms = s_st.notif_interval_avg_ms = 0;
    s_st.weights_per_notif_max = 0;
    s_rate_window_start_us = esp_timer_get_time();
    s_rate_window_count = 0;
    status_changed_locked();
    portEXIT_CRITICAL(&s_lock);
    post(&ev);
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg) {
    if (err->status == 0) {
        if (s_cccd_handle == 0 && ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(UUID_CCCD)) == 0) {
            s_cccd_handle = dsc->handle;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        fail_and_disconnect("descriptor discovery failed", err->status);
        return 0;
    }
    if (s_cccd_handle == 0) {
        fail_and_disconnect("FFF1 has no CCCD", 0);
        return 0;
    }
    // Notify if supported, else indicate (GaggiMate subscribes with notify, varia.cpp:85-87).
    uint8_t value[2] = {(s_notify_props & BLE_GATT_CHR_PROP_NOTIFY) ? 0x01 : 0x02, 0x00};
    int rc = ble_gattc_write_flat(conn, s_cccd_handle, value, sizeof value, on_cccd_written, NULL);
    if (rc != 0) fail_and_disconnect("CCCD write start failed", rc);
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr,
                  void *arg) {
    if (err->status == 0) {
        if (ble_uuid_cmp(&chr->uuid.u, BLE_UUID16_DECLARE(UUID_NOTIFY)) == 0) {
            s_notify_handle = chr->val_handle;
            s_notify_props = chr->properties;
        } else if (ble_uuid_cmp(&chr->uuid.u, BLE_UUID16_DECLARE(UUID_WRITE)) == 0) {
            s_write_handle = chr->val_handle;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        fail_and_disconnect("characteristic discovery failed", err->status);
        return 0;
    }
    if (!s_notify_handle || !s_write_handle) {
        fail_and_disconnect("FFF1/FFF2 not found", 0);
        return 0;
    }
    int rc = ble_gattc_disc_all_dscs(conn, s_notify_handle, s_svc_end, on_dsc, NULL);
    if (rc != 0) fail_and_disconnect("descriptor discovery start failed", rc);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc,
                  void *arg) {
    if (err->status == 0) {
        s_svc_start = svc->start_handle;
        s_svc_end = svc->end_handle;
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        fail_and_disconnect("service discovery failed", err->status);
        return 0;
    }
    if (!s_svc_start) {
        fail_and_disconnect("service FFF0 not found", 0);
        return 0;
    }
    int rc = ble_gattc_disc_all_chrs(conn, s_svc_start, s_svc_end, on_chr, NULL);
    if (rc != 0) fail_and_disconnect("characteristic discovery start failed", rc);
    return 0;
}

// ------------------------------------------------------------------ GAP

static void handle_disc(const struct ble_gap_disc_desc *d) {
    if (s_connecting) return;
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return;
    if (!f.name) return;
    if (!name_matches(f.name, f.name_len)) {
        ESP_LOGD(TAG, "seen \"%.*s\" RSSI %d", f.name_len, (const char *)f.name, d->rssi);
        return;
    }

    size_t n = f.name_len < sizeof s_pending_name - 1 ? f.name_len : sizeof s_pending_name - 1;
    memcpy(s_pending_name, f.name, n);
    s_pending_name[n] = 0;

    char addr[18];
    addr_str(&d->addr, addr);
    ESP_LOGI(TAG, "Found \"%s\" %s RSSI %d, connecting", s_pending_name, addr, d->rssi);

    portENTER_CRITICAL(&s_lock);
    strlcpy(s_st.name, s_pending_name, sizeof s_st.name);
    strlcpy(s_st.addr, addr, sizeof s_st.addr);
    s_st.rssi = d->rssi;
    status_changed_locked();
    portEXIT_CRITICAL(&s_lock);

    s_connecting = true;
    ble_gap_disc_cancel();
    int rc = ble_gap_connect(s_own_addr_type, &d->addr, CONNECT_TIMEOUT_MS, NULL, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "connect start failed rc=%d", rc);
        s_connecting = false;
        start_scan();
    }
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            handle_disc(&event->disc);
            return 0;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            if (!s_connecting && s_conn == BLE_HS_CONN_HANDLE_NONE) scan_next();
            return 0;

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
                reset_link();
                start_scan();
                return 0;
            }
            s_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected, discovering FFF0");
            {
                int rc = ble_gattc_disc_svc_by_uuid(s_conn, BLE_UUID16_DECLARE(UUID_SVC), on_svc, NULL);
                if (rc != 0) fail_and_disconnect("service discovery start failed", rc);
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT: {
            ESP_LOGW(TAG, "Disconnected, reason=0x%x", event->disconnect.reason);
            aku_event_t ev = {.kind = AKU_EV_DISCONNECTED, .t_us = esp_timer_get_time()};
            ev.conn.reason = event->disconnect.reason;
            portENTER_CRITICAL(&s_lock);
            bool was = s_st.connected;
            s_st.connected = false;
            // No scale: the page shows --:--, --.- and ---% instead of the last values.
            s_st.timer_s = -1;
            s_st.battery_pct = -1;
            s_st.weight_g = 0;
            s_st.weight_t_us = 0;
            if (was) {
                s_st.lost_t_us = ev.t_us;
                s_st.fast_until_us = ev.t_us + FAST_SEARCH_US;
            }
            strlcpy(ev.conn.name, s_st.name, sizeof ev.conn.name);
            status_changed_locked();
            portEXIT_CRITICAL(&s_lock);
            if (was) post(&ev);
            reset_link();
            start_scan();
            return 0;
        }

        case BLE_GAP_EVENT_NOTIFY_RX:
            if (event->notify_rx.conn_handle == s_conn && event->notify_rx.attr_handle == s_notify_handle) {
                on_notification(event->notify_rx.om);
            }
            return 0;  // the host frees om

        default:
            return 0;
    }
}

// One scan window: 10 s while searching fast, 3 s in the slow search.
static void start_scan(void) {
    if (ble_gap_disc_active()) return;
    bool fast = fast_now();
    set_search(fast ? AKU_SEARCH_FAST : AKU_SEARCH_SLOW);
    // Units of 0.625 ms. NimBLE's defaults (0) scan with window == interval,
    // which keeps the shared 2.4 GHz radio on BLE nearly all the time while the
    // scale sleeps: WiFi then crawls (1 s pings, updates and pages time out).
    // 30 ms of every 100 ms leaves WiFi room and still finds the scale in a few s.
    struct ble_gap_disc_params p = {
        .itvl = 160,    // 100 ms
        .window = 48,   // 30 ms
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,   // active: the name can be in the scan response
        .filter_duplicates = 1,
    };
    int rc = ble_gap_disc(s_own_addr_type, fast ? SCAN_WINDOW_MS : SLOW_SCAN_WINDOW_MS, &p, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan start failed rc=%d", rc);
    } else {
        ESP_LOGD(TAG, "Scanning for AKU...");
    }
}

// A scan window ended without finding the scale.
static void scan_next(void) {
    if (fast_now()) {
        start_scan();
    } else if (CONFIG_CS_SCALE_SLOW_SEARCH_EVERY_S == 0) {
        set_search(AKU_SEARCH_OFF);
    } else {
        set_search(AKU_SEARCH_SLOW);
        int32_t wait_ms = CONFIG_CS_SCALE_SLOW_SEARCH_EVERY_S * 1000 - SLOW_SCAN_WINDOW_MS;
        if (wait_ms < 100) wait_ms = 100;
        ble_npl_callout_reset(&s_retry, ble_npl_time_ms_to_ticks32((uint32_t)wait_ms));
    }
}

static void on_retry(struct ble_npl_event *ev) {
    if (!s_synced || s_connecting || s_conn != BLE_HS_CONN_HANDLE_NONE) return;
    if (ble_gap_disc_active()) {
        // Woken during a slow look: it ends in <= 3 s and the next one is fast.
        if (fast_now()) set_search(AKU_SEARCH_FAST);
        return;
    }
    start_scan();
}

static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "address setup failed rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "Scanning for AKU (restarts every %d s)...", SCAN_WINDOW_MS / 1000);
    portENTER_CRITICAL(&s_lock);
    s_st.fast_until_us = esp_timer_get_time() + FAST_SEARCH_US;
    portEXIT_CRITICAL(&s_lock);
    s_synced = true;
    start_scan();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason); }

static void host_task(void *param) {
    nimble_port_run();  // returns only on nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ------------------------------------------------------------------ public

esp_err_t aku_start(QueueHandle_t queue) {
    s_queue = queue;
    memset(&s_st, 0, sizeof s_st);
    s_st.battery_pct = -1;
    s_st.timer_s = -1;
    reset_link();

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ble_npl_callout_init(&s_retry, nimble_port_get_dflt_eventq(), on_retry, NULL);
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

bool aku_wake(void) {
    portENTER_CRITICAL(&s_lock);
    bool sped = !s_st.connected && (s_st.search == AKU_SEARCH_SLOW || s_st.search == AKU_SEARCH_OFF);
    if (!s_st.connected) s_st.fast_until_us = esp_timer_get_time() + FAST_SEARCH_US;
    portEXIT_CRITICAL(&s_lock);
    // Start the next window now instead of at the next slow look.
    if (sped && s_synced) ble_npl_callout_reset(&s_retry, 1);
    return sped;
}

const char *aku_search_code(aku_search_t m) {
    switch (m) {
        case AKU_SEARCH_CONNECTED: return "connected";
        case AKU_SEARCH_FAST: return "fast";
        case AKU_SEARCH_SLOW: return "slow";
        default: return "off";
    }
}

void aku_get_status(aku_status_t *out) {
    portENTER_CRITICAL(&s_lock);
    *out = s_st;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t aku_send(aku_cmd_t cmd) {
    uint16_t conn = s_conn, handle = s_write_handle;
    if (conn == BLE_HS_CONN_HANDLE_NONE || !handle || !s_st.connected) return ESP_ERR_INVALID_STATE;
    const uint8_t *b = aku_cmd_bytes(cmd);
    // Write without response (Brew-Controller Varia.cpp; GaggiMate varia.cpp).
    int rc = ble_gattc_write_no_rsp_flat(conn, handle, b, 5);
    ESP_LOGI(TAG, "TX %02X %02X %02X %02X %02X rc=%d", b[0], b[1], b[2], b[3], b[4], rc);
    if (rc == 0 && cmd == AKU_CMD_TIMER_RESET) {
        // Show 0 at once; the scale may not send a timer frame after a reset.
        portENTER_CRITICAL(&s_lock);
        s_st.timer_s = 0;
        status_changed_locked();
        portEXIT_CRITICAL(&s_lock);
    }
    return rc == 0 ? ESP_OK : ESP_FAIL;
}
