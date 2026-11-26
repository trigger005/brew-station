// SPDX-License-Identifier: AGPL-3.0-or-later
// WiFi station + mDNS. Fixed IP comes in M1 (the grinder needs it).
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts WiFi with the credentials from menuconfig (Brew Station menu) and
// announces <hostname>.local over mDNS. Reconnects by itself. Needs NVS and
// the default event loop to be initialised.
esp_err_t net_start(void);

bool net_connected(void);
// Current IPv4 address as text, "" when not connected.
const char *net_ip(void);
const char *net_hostname(void);
int net_rssi(void);

// The last reboot was the WiFi watchdog: WiFi stayed down for
// CONFIG_CS_WIFI_WATCHDOG_S. Read once at boot (then cleared).
bool net_restarted_by_watchdog(void);

#ifdef __cplusplus
}
#endif
