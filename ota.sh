#!/usr/bin/env bash
# Wireless (OTA) reflash for the Frankenstein herd.
#
# Builds with the NORMAL board env, then uploads the resulting binary over WiFi
# with espota.py. We deliberately do NOT use a PlatformIO `upload_protocol =
# espota` env: the espressif32 platform compiles ~450KB larger in OTA mode,
# which overflows the app partition. Uploading the normal binary avoids that.
#
# Usage:  ./ota.sh board1|board2|board3
# Password: $OTA_PASSWORD (defaults to frankenlab; must match OTA_PASSWORD in code).

set -euo pipefail

B="${1:-}"
case "$B" in
    board1) IP=192.168.68.125 ;;   # DHCP-reserved
    board2) IP=192.168.71.202 ;;   # static
    board3) IP=192.168.71.203 ;;   # static
    *) echo "usage: $0 board1|board2|board3"; exit 1 ;;
esac

PW="${OTA_PASSWORD:-frankenlab}"
ESPOTA="$(ls "$HOME"/.platformio/packages/framework-arduinoespressif32/tools/espota.py 2>/dev/null | head -1)"
[ -n "$ESPOTA" ] || { echo "espota.py not found (build once with PlatformIO first)"; exit 1; }

echo "Building $B ..."
pio run -e "$B"

BIN=".pio/build/$B/firmware.bin"
echo "OTA upload -> $B ($IP) ..."
python3 "$ESPOTA" -i "$IP" -p 3232 --auth="$PW" -f "$BIN"
echo "done — $B reflashed over WiFi."
