# Frankenstein Meters

Random needle flicker on old analog panel meters, driven by an ESP32
(PlatformIO + Arduino framework, generic ESP32 DevKit).

Each meter gets its own "personality": a slowly wandering idle baseline in the
lower part of the scale, occasional surges toward full scale that bleed back
off, and constant fine jitter — all slewed so the needle moves like a
mechanical instrument, not a stepper.

## Wiring

Each meter connects between a GPIO and GND through a series resistor:

```
GPIO 25 ──[ R ]──(+) meter (−)── GND
GPIO 26 ──[ R ]──(+) meter (−)── GND
GPIO 27 ──[ R ]──(+) meter (−)── GND
GPIO 33 ──[ R ]──(+) meter (−)── GND
```

### Picking the series resistor R

The ESP32 outputs 3.3 V. For a moving-coil meter with full-scale current
`I_fs` and coil resistance `R_coil`:

```
R = 3.3 V / I_fs − R_coil
```

Typical values:

| Meter full scale | R (approx) |
|------------------|-----------|
| 1 mA             | 3.3 kΩ minus coil resistance (≈2.7–3.3 kΩ) |
| 100 µA           | 33 kΩ |
| 50 µA (VU-style) | 66 kΩ (68 kΩ standard value) |

If you don't know the meter's rating, **start big** (100 kΩ), watch the
deflection, and step down until full scale is reached — never connect a bare
meter straight to the GPIO. A meter marked in volts usually has its multiplier
resistor built in; try it through a small resistor (330 Ω) first.

The needle coil itself smooths the 5 kHz PWM mechanically. If you see any
buzz, add an RC filter: 1 kΩ series + 10 µF from the meter's (+) terminal to
GND.

Keep total current well under the ESP32's ~40 mA absolute max per pin (any
meter of 20 mA full scale or less is fine).

## Calibration

1. Uncomment `#define TEST_SWEEP_MODE` in `src/config.h` and flash. All meters
   sweep 0→100→0% over 8 s while the serial monitor prints the duty.
2. Adjust the series resistor until the needle just reaches full scale at
   100%.
3. For fine software trim, lower that meter's entry in `FULL_SCALE_DUTY`.
4. Re-comment `TEST_SWEEP_MODE` and flash again.

## Tuning the flicker

Everything lives in `src/config.h`:

- `METER_COUNT`, `METER_PINS` — how many meters and where.
- `IDLE_MIN`/`IDLE_MAX` — the band the needle idles in.
- `SURGE_*` — how high and how often the needle spikes.
- `JITTER_AMPLITUDE` — fine trembling.
- `NEEDLE_SPEED` — how snappy the needle is.

## Build & flash

```
pio run                 # compile
pio run -t upload       # flash
pio device monitor      # serial output at 115200
```
