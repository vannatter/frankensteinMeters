#!/usr/bin/env python3
"""Frankenstein lab sound box.

Runs on a Raspberry Pi, WiFi-only — there is NO wire to the ESP32 / LED system,
so the amp noise that plagued the DY module can't reach the flood/rope data.

It polls a board's /status a few times a second and LAYERS audio:

  * a "bed" mpv instance always loops the ambient track (never stops)
  * an "fx" mpv instance plays the frantic clip ON TOP during a freakout,
    then goes silent — so the freakout rides over the background.

The Pi's audio stack mixes the two streams. Only dependency is mpv
(apt install mpv) — no pip packages; talks to mpv over its JSON IPC sockets.
"""

import json
import os
import socket
import subprocess
import time
import urllib.request

# --- config -----------------------------------------------------------------
STATUS_URL = "http://192.168.71.203/status"   # board 3 (lightning) — fastest /status;
                                              # freakout fans out to every board.
SND_DIR = os.path.expanduser("~/franken")
IDLE_FILE = os.path.join(SND_DIR, "idle.mp3")       # ambient bed (loops forever)
FREAK_FILE = os.path.join(SND_DIR, "freakout.wav")  # frantic clip (over the bed)
BED_VOL = 100      # 0-100  (ambient bed)
FX_VOL = 100       # 0-100  (freakout overlay)
DUCK_VOL = 100     # bed volume WHILE a freakout plays (lower this to duck it)
POLL_S = 0.15
BED_SOCK = "/tmp/frank_bed.sock"
FX_SOCK = "/tmp/frank_fx.sock"
# ----------------------------------------------------------------------------


def start_mpv(sock):
    if os.path.exists(sock):
        os.remove(sock)
    subprocess.Popen(
        ["mpv", "--no-terminal", "--no-video", "--idle=yes",
         "--input-ipc-server=" + sock],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    for _ in range(60):
        if os.path.exists(sock):
            break
        time.sleep(0.1)


def mpv_cmd(sock, *cmd):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1)
        s.connect(sock)
        s.sendall((json.dumps({"command": list(cmd)}) + "\n").encode())
        s.close()
    except OSError:
        pass


def set_vol(sock, vol):
    mpv_cmd(sock, "set_property", "volume", vol)


def bed_start():
    mpv_cmd(BED_SOCK, "set_property", "loop-file", "inf")
    set_vol(BED_SOCK, BED_VOL)
    mpv_cmd(BED_SOCK, "loadfile", IDLE_FILE, "replace")


def fx_start():
    mpv_cmd(FX_SOCK, "set_property", "loop-file", "inf")
    set_vol(FX_SOCK, FX_VOL)
    mpv_cmd(FX_SOCK, "loadfile", FREAK_FILE, "replace")


def fx_stop():
    mpv_cmd(FX_SOCK, "stop")


def get_mode():
    try:
        with urllib.request.urlopen(STATUS_URL, timeout=0.6) as r:
            return json.load(r).get("mode")
    except Exception:
        return None


def main():
    start_mpv(BED_SOCK)
    start_mpv(FX_SOCK)
    time.sleep(0.5)
    bed_start()                 # ambient bed runs continuously
    freaking = False
    while True:
        mode = get_mode()
        want = (mode == "freakout")
        if mode is not None and want != freaking:
            freaking = want
            if want:
                set_vol(BED_SOCK, DUCK_VOL)   # optionally duck the bed
                fx_start()                    # frantic clip over the top
            else:
                fx_stop()
                set_vol(BED_SOCK, BED_VOL)    # bed back to full
        time.sleep(POLL_S)


if __name__ == "__main__":
    main()
