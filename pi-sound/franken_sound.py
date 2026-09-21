#!/usr/bin/env python3
"""Frankenstein lab sound box.

Runs on a Raspberry Pi, WiFi-only — there is NO wire to the ESP32 / LED system,
so the amp noise that plagued the DY module can't reach the flood/rope data.

It polls board 1's /status a few times a second and plays audio to match the
lab's mode, using mpv for gapless looping and instant switching:

  freakout -> the frantic clip (loops, loud)
  anything else (flicker/idle/coma/sweep) -> the ambient buzz (loops, quieter)

Only dependency is mpv (apt install mpv) — no pip packages; talks to mpv over
its JSON IPC socket with the standard library.
"""

import json
import os
import socket
import subprocess
import time
import urllib.request

# --- config -----------------------------------------------------------------
STATUS_URL = "http://192.168.68.125/status"   # board 1 (the meters/knife board)
SND_DIR = os.path.expanduser("~/franken")
IDLE_FILE = os.path.join(SND_DIR, "idle.wav")       # ambient mad-scientist buzz
FREAK_FILE = os.path.join(SND_DIR, "freakout.wav")  # frantic clip
IDLE_VOL = 70      # 0-100
FREAK_VOL = 100
POLL_S = 0.15      # how often to check the lab's mode
SOCK = "/tmp/frankmpv.sock"
# ----------------------------------------------------------------------------


def start_mpv():
    """Launch a persistent, idle mpv we drive over its IPC socket."""
    if os.path.exists(SOCK):
        os.remove(SOCK)
    proc = subprocess.Popen(
        ["mpv", "--no-terminal", "--no-video", "--idle=yes",
         "--input-ipc-server=" + SOCK],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    for _ in range(60):                 # wait up to ~6s for the socket
        if os.path.exists(SOCK):
            break
        time.sleep(0.1)
    return proc


def mpv_cmd(*cmd):
    """Send one JSON IPC command to mpv; ignore failures (it'll retry next tick)."""
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1)
        s.connect(SOCK)
        s.sendall((json.dumps({"command": list(cmd)}) + "\n").encode())
        s.close()
    except OSError:
        pass


def play(path, vol):
    """Loop `path` gaplessly at `vol`, cutting off whatever was playing."""
    mpv_cmd("set_property", "loop-file", "inf")
    mpv_cmd("set_property", "volume", vol)
    mpv_cmd("loadfile", path, "replace")


def get_mode():
    try:
        with urllib.request.urlopen(STATUS_URL, timeout=1) as r:
            return json.load(r).get("mode")
    except Exception:
        return None


def main():
    start_mpv()
    time.sleep(0.5)
    cur = None
    while True:
        mode = get_mode()
        if mode == "freakout":
            want = "freak"
        elif mode is None:
            want = cur or "idle"        # lab unreachable: hold what we've got
        else:
            want = "idle"               # flicker / coma / sweep / all-on -> ambient
        if want != cur:
            cur = want
            if want == "freak":
                play(FREAK_FILE, FREAK_VOL)
            else:
                play(IDLE_FILE, IDLE_VOL)
        time.sleep(POLL_S)


if __name__ == "__main__":
    main()
