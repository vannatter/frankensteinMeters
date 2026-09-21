# Frankenstein lab sound box (Raspberry Pi 3 B+)

A WiFi-only audio player for the lab. It watches board 1's `/status` and plays
audio to match the mode — ambient buzz at rest, the frantic clip during a
freakout — with **gapless looping and instant switching** via `mpv`.

Because it's **WiFi-only (no wire to the ESP/LED system)**, the amp noise that
corrupted the floods/ropes with the DY module has no path back in. Power it from
its own supply and keep it away from the LED runs.

## 1. Flash the SD card (Raspberry Pi Imager, on your Mac)

- Device: **Raspberry Pi 3**, OS: **Raspberry Pi OS Lite (64-bit)** (no desktop
  needed).
- Click the **gear / "Edit Settings"** before writing and set:
  - **Hostname:** `frankensound`
  - **Enable SSH** (password auth)
  - **Username / password** (remember these)
  - **WiFi:** SSID `VANNATTER` + password (2.4 GHz)
- Write it, put the card in the Pi, plug in power. It boots headless onto WiFi.

## 2. Copy the files over (from your Mac, in this folder)

```
scp franken_sound.py franken-sound.service <user>@frankensound.local:~/
ssh <user>@frankensound.local
```

## 3. On the Pi

```
sudo apt update && sudo apt install -y mpv
mkdir -p ~/franken
mv ~/franken_sound.py ~/franken/

# your two sound files (any name -> these names):
#   ~/franken/idle.wav      (ambient buzz, loops)
#   ~/franken/freakout.wav  (frantic clip, loops during a freakout)
# copy them over with scp too, e.g. from the Mac:
#   scp idle.wav freakout.wav <user>@frankensound.local:~/franken/

# force audio out the 3.5mm jack (not HDMI):
sudo raspi-config    # System Options -> Audio -> Headphones/3.5mm
# quick test:
mpv ~/franken/idle.wav

# install the auto-start service (replace USERNAME in the file first):
sed -i "s/USERNAME/$USER/g" ~/franken-sound.service
sudo mv ~/franken-sound.service /etc/systemd/system/
sudo systemctl enable --now franken-sound
```

Check it: `systemctl status franken-sound` and `journalctl -u franken-sound -f`.

## Tuning

- Volumes: `IDLE_VOL` / `FREAK_VOL` at the top of `franken_sound.py`.
- Board 1 address: `STATUS_URL` (currently `http://192.168.68.125/status`).
- After edits: `sudo systemctl restart franken-sound`.

## Notes

- `mpv` loops **gaplessly**, so a short clip repeats cleanly — no more DY-style
  gap. Even so, a frantic clip ~= the freakout length (20 s) plays once through.
- Files can be `.wav` or `.mp3` (rename to `idle`/`freakout` accordingly and
  update the paths, or just keep `.wav`).
- No `pip` packages — only `mpv`. Everything else is Python stdlib.
