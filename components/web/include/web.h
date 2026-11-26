// SPDX-License-Identifier: AGPL-3.0-or-later
// Status page on port 80: live scale data over a WebSocket, plus scale
// commands, and manual shot results. grinder_api adds the grinder's /api/v2/* routes
// to the same server (web_httpd()).
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_start(void);

// The server web_start started (NULL before), for grinder_api_start.
httpd_handle_t web_httpd(void);

// Sends a line to the event log of every open page (thread-safe).
void web_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif
