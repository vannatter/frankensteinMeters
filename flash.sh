#!/bin/bash
# MAC-verified flashing for the Frankenstein boards.
#
# Usage: ./flash.sh board1|board2
#
# Both boards have CP2102 USB chips with IDENTICAL serial numbers, so
# /dev/cu.usbserial-* names shuffle when cables are replugged — flashing by
# port name once put board 1's firmware on the lights board. This script
# reads each connected board's factory-unique ESP32 MAC address and flashes
# ONLY the physical board whose MAC matches the target. No match = no flash.

set -euo pipefail

MAC_BOARD1="30:76:f5:92:3d:c0"      # meters board
MAC_BOARD2="30:76:f5:91:97:80"      # lights board (red dot)

TARGET="${1:-}"
if [[ "$TARGET" != "board1" && "$TARGET" != "board2" ]]; then
    echo "usage: ./flash.sh board1|board2"; exit 1
fi

PYTHON=$(ls -d /opt/homebrew/Cellar/platformio/*/libexec/bin/python | head -1)
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"

want_mac() { [[ "$TARGET" == "board1" ]] && echo "$MAC_BOARD1" || echo "$MAC_BOARD2"; }
other_mac() { [[ "$TARGET" == "board1" ]] && echo "$MAC_BOARD2" || echo "$MAC_BOARD1"; }

PORTS=$(ls /dev/cu.usbserial-* 2>/dev/null || true)
if [[ -z "$PORTS" ]]; then echo "no ESP32 boards on USB"; exit 1; fi

FOUND_PORT=""
for port in $PORTS; do
    mac=$("$PYTHON" "$ESPTOOL" --port "$port" read_mac 2>/dev/null \
          | grep -im1 '^MAC:' | awk '{print tolower($2)}') || true
    if [[ -z "$mac" ]]; then
        echo "  $port: could not read MAC (busy? unplugged mid-read?)"; continue
    fi
    echo "  $port → MAC $mac"
    if [[ "$mac" == "$(want_mac)" ]]; then
        FOUND_PORT="$port"
    elif [[ -z "$(want_mac)" && "$mac" != "$(other_mac)" ]]; then
        # Target's MAC not recorded yet, but this board is NOT the other one.
        echo "  ⚠ unrecorded MAC — if this is $TARGET, add it to MAC_BOARD1/2 in flash.sh"
        FOUND_PORT="$port"
        NEW_MAC="$mac"
    fi
done

if [[ -z "$FOUND_PORT" ]]; then
    echo "✗ $TARGET is not connected (no MAC match). NOT flashing anything."
    exit 1
fi

echo "✓ $TARGET verified at $FOUND_PORT — flashing..."
pio run -e "$TARGET" -t upload --upload-port "$FOUND_PORT"
[[ -n "${NEW_MAC:-}" ]] && echo "→ RECORD THIS: $TARGET MAC is $NEW_MAC (update flash.sh)"
echo "done."
