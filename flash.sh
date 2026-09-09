#!/bin/bash
# MAC-verified flashing for the Frankenstein boards.
#
# Usage: ./flash.sh board1|board2|board3
#
# The boards have CP2102 USB chips with IDENTICAL serial numbers, so
# /dev/cu.usbserial-* names shuffle when cables are replugged — flashing by
# port name once put board 1's firmware on the lights board. This script
# reads each connected board's factory-unique ESP32 MAC and flashes ONLY the
# physical board whose MAC matches the target. No match = no flash.
# (Written for macOS's bash 3.2 — no associative arrays.)

set -euo pipefail

# Known board MACs (lowercase). Blank until first sighting; the script
# auto-detects an unrecorded target and prints its MAC to record here.
MAC_board1="70:4b:ca:6e:94:88"   # meters board (replaced 2026-09-09; old brownout-prone board was 30:76:f5:92:3d:c0)
MAC_board2="30:76:f5:91:97:80"   # lights board (red dot)
MAC_board3="8c:94:df:4d:13:74"   # lightning controller
ALL_BOARDS="board1 board2 board3"

TARGET="${1:-}"
case " $ALL_BOARDS " in
    *" $TARGET "*) ;;
    *) echo "usage: ./flash.sh board1|board2|board3"; exit 1 ;;
esac

PYTHON=$(ls -d /opt/homebrew/Cellar/platformio/*/libexec/bin/python | head -1)
ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
eval "WANT=\$MAC_$TARGET"

# Is this MAC a known OTHER board (not the target)?
is_other() {
    local m="$1" b bmac
    for b in $ALL_BOARDS; do
        [ "$b" = "$TARGET" ] && continue
        eval "bmac=\$MAC_$b"
        [ -n "$bmac" ] && [ "$bmac" = "$m" ] && return 0
    done
    return 1
}

PORTS=$(ls /dev/cu.usbserial-* 2>/dev/null || true)
if [ -z "$PORTS" ]; then echo "no ESP32 boards on USB"; exit 1; fi

FOUND_PORT=""
NEW_MAC=""
for port in $PORTS; do
    mac=$("$PYTHON" "$ESPTOOL" --port "$port" read_mac 2>/dev/null \
          | grep -im1 '^MAC:' | awk '{print tolower($2)}') || true
    if [ -z "$mac" ]; then
        echo "  $port: could not read MAC (busy? unplugged mid-read?)"; continue
    fi
    echo "  $port -> MAC $mac"
    if [ -n "$WANT" ] && [ "$mac" = "$WANT" ]; then
        FOUND_PORT="$port"
    elif [ -z "$WANT" ] && ! is_other "$mac"; then
        echo "  ! unrecorded MAC — if this is $TARGET, add it to flash.sh"
        FOUND_PORT="$port"; NEW_MAC="$mac"
    fi
done

if [ -z "$FOUND_PORT" ]; then
    echo "x $TARGET is not connected (no MAC match). NOT flashing anything."
    exit 1
fi

echo "OK $TARGET verified at $FOUND_PORT — flashing..."
pio run -e "$TARGET" -t upload --upload-port "$FOUND_PORT"
[ -n "$NEW_MAC" ] && echo "-> RECORD THIS: $TARGET MAC is $NEW_MAC (update flash.sh)"
echo "done."
