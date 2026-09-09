# Frankenstein Meters

A Halloween mad-scientist lab prop: a bench of antique analog meters, flickering
lab lights, a real Edison bulb, two lightning "shock towers," and an
animatronic Frankenstein — all driven by a small fleet of ESP32s and conducted
from a single gothic web dashboard. Hit **Galvanize** and the whole tableau
convulses: needles slam and seize, lights strobe, the Edison browns out,
lightning bolts race up the towers into the creature's head, and he jerks awake.

Built with PlatformIO + Arduino on generic ESP32 DevKits.

---

## The lab (three boards)

Each board runs the *same* firmware; a `BOARD_ID` build flag selects its role,
static IP, and hardware. They discover each other, and a command sent to one
(with `?all=1`) fans out to the whole "herd."

| Board | Role | Address | Drives |
|-------|------|---------|--------|
| **1** | Meters | `192.168.71.201` | 7 antique panel meters, each with a personality |
| **2** | Lights | `192.168.71.202` | 12 LED channels (2× ULN2803), Edison bulb (Shelly dimmer), animatronic Try-Me trigger |
| **3** | Lightning | `192.168.71.203` | 2 WS2812B neon ropes up the shock towers (FastLED) |

Dashboard: **http://frankenstein.vannatter.com** (a local DNS record → board 1;
any board's IP also serves it).

---

## Modes

Set lab-wide from the dashboard, or per-channel:

- **Flicker** — the resting state. Each instrument idles in character.
- **Galvanize (freakout)** — the shock. Meters slam and seize, lights strobe,
  Edison surges bright↔black, lightning fires, the animatronic triggers. Runs
  20 s (covers the prop's full animation).
- **Coma** — barely alive: shallow breathing, faint heartbeat, dim ember.
- **Calibrate (sweep)** — slow 0→100→0% ramp for sizing meter resistors.
- **All On** — every channel full, for connectivity checks.
- **Extinguish All** — kill every pin; toggle back to Rekindle.
- **Identify** — blink one channel, dark the rest, to trace physical wiring.

### Meter "personalities" (board 1)
`flicker` (wandering + surges), `heartbeat` (lub-dub), `scan` (patrol sweep),
`lung` (breathing), `spastic` (twitchy) — plus per-channel overrides and
custom position/duration choreography. Every channel is renamable live.

### Lights (board 2)
Each of 12 channels picks a pattern: `dark`, `steady`, `doubleblink`,
`breathe`, `candle`, `strobe`, `random`, or `custom` (a live rhythm editor).
Panic strobes them all; coma darkens them. The **Edison bulb** runs on a Shelly
Dimmer G4 over HTTP — a dim failing-mains brown-out at rest, violent throb on
panic. The **Try-Me trigger** (GPIO 32 → isolated MOSFET) presses the
animatronic's button when panic starts.

### Lightning (board 3)
Two addressable neon ropes, one up each shock tower, fed from a central box and
mirrored so both towers fire identically. Idle: a green energy ring drifts up
toward the creature. Panic: a green spark bursts at his head, then bright white
bolts race up the ropes and flare on impact.

---

## Dashboard

A single self-hosted page (served from each board) in an IM Fell gothic theme:

- **The Herd** — every board's status + an animated miniature mirroring what the
  hardware is doing right now (needle arcs, glowing light dots).
- **Per-channel control** — name (tap to rename), power toggle, mode/pattern
  dropdown, identify, and visual rhythm/choreography editors.
- **Targeting** — act on the whole lab or one board.
- Responsive; runs from any phone/laptop on the house network.

---

## Build & flash

```
./flash.sh board1        # or board2 / board3
```

`flash.sh` is **MAC-verified**: the boards' USB chips share identical serial
numbers, so it reads each board's factory ESP32 MAC and flashes only the one
you asked for (no more wrong-board disasters). It auto-detects a new board's
MAC on first flash.

Raw PlatformIO also works: `pio run -e board2 -t upload`.
Serial monitor: `pio device monitor` (115200).

WiFi credentials live in `src/secrets.h` (gitignored). All per-board hardware
(pins, meter map, light channels, Edison, lightning) is configured in
`src/config.h`.

---

## Board 1 — meters (hardware)

Each meter connects between a GPIO and GND through a series resistor:

```
GPIO ──[ R ]──(+) meter (−)── GND
```

**Sizing R** — the ESP32 outputs 3.3 V; for a movement with full-scale current
`I_fs` and coil resistance `R_coil`: `R = 3.3 / I_fs − R_coil`.

| Full scale | R (approx) |
|------------|-----------|
| 1 mA | ≈2.7–3.3 kΩ |
| 100 µA | ≈33 kΩ |
| 50 µA (VU) | ≈68 kΩ |

Don't know the rating? **Start at 100 kΩ**, hit **Calibrate**, and step down
until the needle just reaches full scale — never wire a bare movement straight
to a GPIO. A meter marked in *volts* has its multiplier built in (try ~1 kΩ).
Fine-trim in software via each channel's `fullScaleDuty`.

## Board 2 — lights (hardware)

12V LEDs switch through two ULN2803 drivers: `GPIO → IN`, `bulb − → the OUT
straight across`, `bulb + → 12V+`. All grounds common (ESP GND + chip pin 9 +
12V−). The Edison is mains on a Shelly Dimmer G4 (WiFi). The Try-Me trigger
uses an opto-isolated MOSFET module so the animatronic stays isolated.

## Board 3 — lightning (hardware)

Central box below the creature drives two data lines — `GPIO 4 → tower 1`,
`GPIO 5 → tower 2`. Each rope: `data`, `+5V` and `GND` from its own 5V/10A
brick; all grounds common with the board. FastLED caps total current to stay
within the supplies. (WS2812 note: single-ended data is good to ~10 ft here;
mind wire colors — on these ropes **green is data**, red +5V, white GND.)

---

*It was on a dreary night of November…*
