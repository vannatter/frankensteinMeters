#!/usr/bin/env python3
"""Frankenstein lab sound box.

Runs on a Raspberry Pi, WiFi-only — no wire to the ESP32 / LED system, so amp
noise can't reach the flood/rope data.

Audio is LAYERED via two mpv instances the Pi's audio stack mixes:
  * a "bed" that always loops the ambient track
  * an "fx" that plays the frantic clip ON TOP during a freakout, then stops.

State comes from two sources:
  * PUSH  — board 1 fires a UDP "freak"/"idle" packet the instant it changes;
            near-instant and immune to network lag (fire-and-forget).
  * POLL  — as a fallback, it also polls a board's /status; a poll won't override
            a recent push (so a laggy /status can't undo an instant push).

Only dependency is mpv (apt install mpv); everything else is Python stdlib.
"""

import json
import os
import socket
import subprocess
import threading
import time
import urllib.request

# --- config -----------------------------------------------------------------
STATUS_URL = "http://192.168.71.203/status"   # board 3 (fast /status); poll fallback
UDP_PORT = 4210                               # board 1 pushes "freak"/"idle" here
SND_DIR = os.path.expanduser("~/franken")
IDLE_FILE = os.path.join(SND_DIR, "idle.mp3")       # ambient bed (loops forever)
FREAK_FILE = os.path.join(SND_DIR, "freakout.wav")  # frantic clip (over the bed)
BED_VOL = 80        # idle ambient bed
FX_VOL = 100        # freakout = full
DUCK_VOL = 80       # bed volume while a freakout plays (lower to duck it)
POLL_S = 0.3
PUSH_HOLD_S = 3.0   # after a push, ignore poll readings this long
BED_SOCK = "/tmp/frank_bed.sock"
FX_SOCK = "/tmp/frank_fx.sock"
# ----------------------------------------------------------------------------

_lock = threading.Lock()
_freaking = False
_last_push = 0.0


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


def set_freak(want):
    """Idempotent: overlay the frantic clip (want=True) or stop it (False)."""
    global _freaking
    with _lock:
        if want == _freaking:
            return
        _freaking = want
        if want:
            set_vol(BED_SOCK, DUCK_VOL)
            mpv_cmd(FX_SOCK, "set_property", "loop-file", "inf")
            set_vol(FX_SOCK, FX_VOL)
            mpv_cmd(FX_SOCK, "loadfile", FREAK_FILE, "replace")
        else:
            mpv_cmd(FX_SOCK, "stop")
            set_vol(BED_SOCK, BED_VOL)


def udp_listener():
    """Board 1 pushes 'freak' / 'idle' here — act instantly."""
    global _last_push
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", UDP_PORT))
    while True:
        try:
            data, _ = s.recvfrom(64)
        except OSError:
            continue
        msg = data.decode(errors="ignore").strip().lower()
        if msg in ("freak", "idle"):
            _last_push = time.time()
            set_freak(msg == "freak")


def get_mode():
    try:
        with urllib.request.urlopen(STATUS_URL, timeout=3) as r:
            return json.load(r).get("mode")
    except Exception:
        return None


def main():
    start_mpv(BED_SOCK)
    start_mpv(FX_SOCK)
    time.sleep(0.5)
    bed_start()
    threading.Thread(target=udp_listener, daemon=True).start()
    while True:                       # poll fallback (won't override a recent push)
        mode = get_mode()
        if mode is not None and (time.time() - _last_push) > PUSH_HOLD_S:
            set_freak(mode == "freakout")
        time.sleep(POLL_S)


if __name__ == "__main__":
    main()
