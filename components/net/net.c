// SPDX-License-Identifier: AGPL-3.0-or-later
#include "net.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "mdns.h"
#include "sdkconfig.h"

static const char *TAG = "net";

static esp_netif_t *s_netif;
static volatile bool s_connected;
static char s_ip[16];
static TimerHandle_t s_retry_timer;
static uint32_t s_retry_ms = 1000;

// WiFi watchdog: the board went off the network once and stayed off (no
// reply to ping) until reset. If WiFi stays down this long, reboot.
#define WD_MAGIC 0x57494649u  // "WIFI"
RTC_NOINIT_ATTR static uint32_t s_wd_mark;  // survives esp_restart()
static bool s_wd_restarted;
static volatile int64_t s_down_since_us;

__attribute__((unused)) static void wd_cb(void *arg) {
    if (s_connected) {
        s_down_since_us = 0;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (!s_down_since_us) s_down_since_us = now;
    if (now - s_down_since_us >= CONFIG_CS_WIFI_WATCHDOG_S * 1000000LL) {
        ESP_LOGE(TAG, "WiFi down for %d s: rebooting", CONFIG_CS_WIFI_WATCHDOG_S);
        s_wd_mark = WD_MAGIC;
        esp_restart();
    }
}

bool net_restarted_by_watchdog(void) { return s_wd_restarted; }

static void retry_cb(TimerHandle_t t) { esp_wifi_connect(); }

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        s_connected = false;
        s_ip[0] = 0;
        ESP_LOGW(TAG, "WiFi disconnected (reason %d), retry in %lu ms", d->reason, (unsigned long)s_retry_ms);
        xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(s_retry_ms), 0);
        xTimerStart(s_retry_timer, 0);
        if (s_retry_ms < 30000) s_retry_ms *= 2;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_retry_ms = 1000;
        ESP_LOGI(TAG, "WiFi connected, IP %s, http://%s.local/", s_ip, CONFIG_CS_HOSTNAME);
    }
}

// The grinder polls a fixed address, so either a DHCP reservation or this.
static void set_static_ip(void) {
    esp_netif_ip_info_t ip = {0};
    if (esp_netif_str_to_ip4(CONFIG_CS_STATIC_IP, &ip.ip) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_CS_STATIC_NETMASK, &ip.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_CS_STATIC_GATEWAY, &ip.gw) != ESP_OK) {
        ESP_LOGE(TAG, "Fixed IP: bad address/netmask/gateway in menuconfig, using DHCP");
        return;
    }
    esp_netif_dhcpc_stop(s_netif);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &ip));
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    const char *d = strlen(CONFIG_CS_STATIC_DNS) ? CONFIG_CS_STATIC_DNS : CONFIG_CS_STATIC_GATEWAY;
    if (esp_netif_str_to_ip4(d, &dns.ip.u_addr.ip4) == ESP_OK)
        esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
    ESP_LOGI(TAG, "Fixed IP %s", CONFIG_CS_STATIC_IP);
}

esp_err_t net_start(void) {
    if (strlen(CONFIG_CS_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "No WiFi SSID set. Run `idf.py menuconfig` -> Brew Station.");
        return ESP_ERR_INVALID_STATE;
    }

    s_wd_restarted = esp_reset_reason() == ESP_RST_SW && s_wd_mark == WD_MAGIC;
    s_wd_mark = 0;
    if (s_wd_restarted) ESP_LOGW(TAG, "Rebooted by the WiFi watchdog");
#if CONFIG_CS_WIFI_WATCHDOG_S > 0
    const esp_timer_create_args_t wd = {.callback = wd_cb, .name = "wifi_wd"};
    esp_timer_handle_t wd_timer;
    if (esp_timer_create(&wd, &wd_timer) == ESP_OK) esp_timer_start_periodic(wd_timer, 5 * 1000000LL);
#endif

    ESP_ERROR_CHECK(esp_netif_init());
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, CONFIG_CS_HOSTNAME);
    if (strlen(CONFIG_CS_STATIC_IP)) set_static_ip();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(1000), pdFALSE, NULL, retry_cb);

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_CS_WIFI_SSID, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, CONFIG_CS_WIFI_PASSWORD, sizeof wc.sta.password);
    wc.sta.threshold.authmode = strlen(CONFIG_CS_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Connecting to \"%s\"...", CONFIG_CS_WIFI_SSID);

    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        mdns_hostname_set(CONFIG_CS_HOSTNAME);
        mdns_instance_name_set("Brew Station");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    } else {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

bool net_connected(void) { return s_connected; }
const char *net_ip(void) { return s_ip; }
const char *net_hostname(void) { return CONFIG_CS_HOSTNAME; }

int net_rssi(void) {
    wifi_ap_record_t ap;
    return (s_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}
