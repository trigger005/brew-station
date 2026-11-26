// SPDX-License-Identifier: AGPL-3.0-or-later
// Firmware update over WiFi: POST /api/ota with the app .bin as the body and
// the token from menuconfig in an X-OTA-Token header. tools/ota.sh does that.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers POST /api/ota on `server`. Call it before grinder_api_start, whose "/*"
// would otherwise answer first. With an empty token the route refuses everything.
esp_err_t ota_start(httpd_handle_t server);

// After an update the new app boots on probation: if it doesn't call this
// within CONFIG_CS_OTA_CONFIRM_S, it reboots and the bootloader goes back to
// the previous app. main calls it once WiFi is up and the server runs.
// Does nothing when the running app isn't on probation.
void ota_confirm_when_healthy(bool (*healthy)(void));

#ifdef __cplusplus
}
#endif
