#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# Sends build/<project>.bin to the bridge over WiFi (POST /api/ota).
# Usage: tools/ota.sh [host]   (default <hostname>.local from sdkconfig; run idf.py build first)
# The token is read from sdkconfig and never printed.
set -eu
cd "$(dirname "$0")/.."

HOST=${1:-$(sed -n 's/^CONFIG_CS_HOSTNAME="\(.*\)"$/\1/p' sdkconfig).local}
TOKEN=$(sed -n 's/^CONFIG_CS_OTA_TOKEN="\(.*\)"$/\1/p' sdkconfig)
BIN=$(ls build/*.bin 2>/dev/null | grep -v -e ota_data_initial | head -1)

[ -n "$TOKEN" ] || { echo "No CONFIG_CS_OTA_TOKEN in sdkconfig (idf.py menuconfig -> Brew Station)"; exit 1; }
[ -n "$BIN" ] || { echo "No build/*.bin: run idf.py build first"; exit 1; }

echo "Sending $BIN ($(wc -c < "$BIN" | tr -d ' ') bytes) to http://$HOST/api/ota"
# Token via a header file on stdin, so it doesn't show in the process list.
printf 'X-OTA-Token: %s\n' "$TOKEN" | curl -sS --fail-with-body -m 180 \
    -H @- -H 'Content-Type: application/octet-stream' \
    --data-binary "@$BIN" "http://$HOST/api/ota"
echo "Rebooting. It's back in ~10 s; a firmware that can't reach WiFi rolls back by itself."
