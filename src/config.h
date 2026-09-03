#pragma once

// ---------------------------------------------------------------------------
// Frankenstein Meters — configuration
// ---------------------------------------------------------------------------

// Channels per board. Board 1 carries the full 7-meter map; board 2 is the
// 4-channel lighting board.
#ifndef BOARD_ID
#define BOARD_ID 1
#endif
#if BOARD_ID == 1
#define METER_COUNT 7
#else
#define METER_COUNT 4
#endif

// PWM settings. 5 kHz is far above the needle's mechanical response, so the
// meter averages the duty cycle into a steady deflection.
#define PWM_FREQ_HZ 5000
#define PWM_RESOLUTION_BITS 12
#define PWM_MAX_DUTY ((1 << PWM_RESOLUTION_BITS) - 1)

// Calibration sweep (slow 0..100%..0 ramp for choosing series resistors) is
// a runtime mode — no reflash needed. Trigger with 's' over serial or
// http://<meter-ip>/sweep; stop with 'c' or /calm.

// ---------------------------------------------------------------------------
// Per-meter personalities. Each meter has its own row — bands are 0.0..1.0 of
// full scale. Fields:
//   pin            GPIO driving this meter (through its series resistor)
//   fullScaleDuty  0..PWM_MAX_DUTY trim so 1.0 = the needle's actual max
//   idleMin/Max    band the needle wanders in between surges
//   surgeMin/Max   band surges spike into
//   surgeSMin/Max  seconds between surges
//   jitter         constant fine trembling amplitude
//   speed          needle responsiveness (higher = snappier)
// ---------------------------------------------------------------------------

// STYLE_FLICKER: wandering + surges (the normal lab-instrument look).
// STYLE_HEARTBEAT: lub-dub pulse — ~65 BPM normally, pounding during
// freakout, slow and faint in coma. Tuning in the HEART_* defines below.
// STYLE_SCAN: patrol sweep at rest — slow bottom-to-top-and-back with a
// slight tremble — but still slams in freakout and sinks in coma.
// STYLE_LIGHT: a light, not a meter — shows its selected pattern (changeable
// from the dashboard), except panic/freakout ALWAYS strobes every light and
// coma darkens them.
enum MeterStyle { STYLE_FLICKER, STYLE_HEARTBEAT, STYLE_SCAN, STYLE_LIGHT };

// Patterns a STYLE_LIGHT channel can show (dashboard-selectable):
//   dark        off until a panic
//   steady      simply on
//   doubleblink on-on-off ... on-on-off
//   breathe     slow smooth pulse
//   candle      organic gas-lamp flicker
//   strobe      the panic flash, running all the time
//   random      unpredictable on/off blinking
//   custom      user-edited rhythm (on/off ms list, editable live on the
//               dashboard, stored in flash — survives reboots)
enum LightPattern { LP_DARK, LP_STEADY, LP_DOUBLE, LP_BREATHE, LP_CANDLE, LP_STROBE, LP_RANDOM, LP_CUSTOM };

struct MeterProfile {
    const char* name;
    int pin;
    int fullScaleDuty;
    float idleMin, idleMax;
    float surgeMin, surgeMax;
    float surgeSMin, surgeSMax;
    float jitter;
    float speed;
    MeterStyle style;
    LightPattern defPattern;  // STYLE_LIGHT only: pattern at boot
};

#if BOARD_ID == 1
// Names here are DEFAULTS — channels can be renamed live from the dashboard
// (tap the name); renames persist in flash.
static const MeterProfile METERS[METER_COUNT] = {
    // GPIO 25, big EICO: the star — calm slow wander, rare big surges.
    {"main", 25, PWM_MAX_DUTY, 0.15f, 0.35f, 0.70f, 1.00f, 6.0f, 18.0f, 0.02f, 0.06f, STYLE_FLICKER, LP_DARK},
    // GPIO 26: busier — higher band, twitchier.
    {"antenna", 26, PWM_MAX_DUTY, 0.20f, 0.55f, 0.75f, 1.00f, 1.5f, 6.0f, 0.05f, 0.12f, STYLE_FLICKER, LP_DARK},
    // GPIO 27, Weston 3V range: calmer, sits lower, rarer surges.
    {"weston", 27, PWM_MAX_DUTY, 0.10f, 0.35f, 0.60f, 0.95f, 5.0f, 15.0f, 0.02f, 0.10f, STYLE_FLICKER, LP_DARK},
    // GPIO 33: the heartbeat monitor.
    {"heartbeat", 33, PWM_MAX_DUTY, 0.15f, 0.40f, 0.65f, 0.90f, 4.0f, 12.0f, 0.02f, 0.05f, STYLE_HEARTBEAT, LP_DARK},
    // Slots 5-7 for the rest of the collection — generic until calibrated.
    // (12/13/14: same side of the DevKit as the other meter pins. GPIO 12 is
    // a strapping pin — safe with a meter-to-GND load, never tie it high.)
    {"slot5", 12, PWM_MAX_DUTY, 0.15f, 0.45f, 0.70f, 1.00f, 3.0f, 10.0f, 0.03f, 0.08f, STYLE_FLICKER, LP_DARK},
    {"slot6", 13, PWM_MAX_DUTY, 0.15f, 0.45f, 0.70f, 1.00f, 3.0f, 10.0f, 0.03f, 0.08f, STYLE_FLICKER, LP_DARK},
    // Slot 7 (pin 14): the ether-scanner — patrol sweep at rest.
    {"slot7", 14, PWM_MAX_DUTY, 0.15f, 0.45f, 0.70f, 1.00f, 3.0f, 10.0f, 0.02f, 0.08f, STYLE_SCAN, LP_DARK},
};
#else
// Board 2 is all lighting — no meters.
// GPIO 26 verified healthy (2026-09-01) — the earlier "dead pin" verdict was
// an artifact of the board-identity mixup. All of 25/26/27/33 work; 18 spare.
static const MeterProfile METERS[4] = {
    {"light1", 25, PWM_MAX_DUTY, 0, 0, 0, 0, 0, 0, 0, 0, STYLE_LIGHT, LP_CANDLE},
    // GPIO 33: the proven-working pin — the main lamp lives here.
    {"light2", 33, PWM_MAX_DUTY, 0, 0, 0, 0, 0, 0, 0, 0, STYLE_LIGHT, LP_DOUBLE},
    {"light3", 27, PWM_MAX_DUTY, 0, 0, 0, 0, 0, 0, 0, 0, STYLE_LIGHT, LP_BREATHE},
    {"light4", 26, PWM_MAX_DUTY, 0, 0, 0, 0, 0, 0, 0, 0, STYLE_LIGHT, LP_DARK},
};
#endif

#if BOARD_ID == 2
// Try-Me trigger for the animatronic: GPIO 32 drives an optocoupler (or
// relay) that momentarily "presses" the prop's 3.5mm Try-Me button whenever
// a freakout starts. See README wiring notes.
#define TRYME_PIN 32
#define TRYME_PULSE_MS 500
#endif

// Freakout strobe light: random on/off times (ms) so it reads as arcing
// electricity rather than a metronome strobe.
#define STROBE_ON_MS_MIN 30.0f
#define STROBE_ON_MS_MAX 90.0f
#define STROBE_OFF_MS_MIN 30.0f
#define STROBE_OFF_MS_MAX 120.0f

// Heartbeat tuning: {beat period ms, main-pulse height, rest baseline} per
// mode. The second (dub) pulse is 60% of the main one.
#define HEART_NORMAL_MS 1700.0f
#define HEART_NORMAL_AMP 0.65f
#define HEART_NORMAL_BASE 0.06f
#define HEART_FREAK_MS 550.0f
#define HEART_FREAK_AMP 1.00f
#define HEART_FREAK_BASE 0.10f
#define HEART_COMA_MS 2800.0f
#define HEART_COMA_AMP 0.30f
#define HEART_COMA_BASE 0.03f

// ---------------------------------------------------------------------------
// Freakout mode (triggered over WiFi: http://<meter-ip>/freakout)
// ---------------------------------------------------------------------------

// Freakout = panic: the needle whips between these two bands over and over,
// bottom to top, as fast as the movement can follow.
#define FREAK_LOW_MIN 0.00f
#define FREAK_LOW_MAX 0.12f
#define FREAK_HIGH_MIN 0.88f
#define FREAK_HIGH_MAX 1.00f

// How long the needle aims at one extreme before whipping to the other (ms).
// Randomized per swing so it reads as panic, not a metronome. The needle's
// own ballistics need ~150ms+ to cross the scale, so much below that and it
// hovers mid-scale instead of slamming.
#define FREAK_FLIP_MS_MIN 280.0f
#define FREAK_FLIP_MS_MAX 550.0f

// Trembling amplitude and needle speed while freaking out. Speed 1.0 = no
// smoothing at all: the coil current steps instantly and the needle slams as
// hard as its mechanics allow — the electric-shock look.
#define FREAK_JITTER 0.05f
#define FREAK_SPEED 1.00f

// Seizure holds: after a burst of slams the needle locks up trembling near the
// top — like the current has him rigid — then goes back to slamming.
#define SEIZE_MIN 0.90f
#define SEIZE_MAX 1.00f
#define SEIZE_MS_MIN 700.0f
#define SEIZE_MS_MAX 1600.0f

// How many full slams happen between seizure holds (randomized each burst).
#define SLAMS_PER_BURST_MIN 3
#define SLAMS_PER_BURST_MAX 8

// ---------------------------------------------------------------------------
// Coma mode — barely alive. Needle drifts very low, very slowly, with a faint
// stir every so often like shallow breathing.
// ---------------------------------------------------------------------------

#define COMA_MIN 0.02f
#define COMA_MAX 0.10f
#define COMA_SPEED 0.02f
#define COMA_JITTER 0.008f

// Occasional gentle stir: rises to this band, then sinks back.
#define COMA_STIR_MIN 0.10f
#define COMA_STIR_MAX 0.18f
#define COMA_STIR_INTERVAL_MIN_S 8.0f
#define COMA_STIR_INTERVAL_MAX_S 25.0f

// How long a freakout lasts if the request doesn't say (seconds).
#define FREAKOUT_DEFAULT_S 15

// ---------------------------------------------------------------------------
// Multi-board setup. BOARD_ID comes from platformio.ini (board1 env = 1,
// board2 env = 2, ...) and derives this board's identity:
//   board N  →  IP 192.168.71.(200+N)  →  board1 = .201, board2 = .202, ...
// The 192.168.71.2xx corner of this /22 network is far from the router's
// busy DHCP range (the 192.168.68.x addresses collided with other devices).
// ---------------------------------------------------------------------------

#ifndef BOARD_ID
#define BOARD_ID 1
#endif

#define XSTR(s) STR(s)
#define STR(s) #s
#define WIFI_HOSTNAME "frankenmeters-" XSTR(BOARD_ID)

// Static IP so each board is always at the same address (mDNS .local names
// don't resolve reliably on this mesh network). Comment out USE_STATIC_IP to
// go back to DHCP.
#define USE_STATIC_IP
#define BOARD_IP_PREFIX "http://192.168.71."
#define STATIC_IP 192, 168, 71, (200 + BOARD_ID)
#define STATIC_GATEWAY 192, 168, 68, 1
#define STATIC_SUBNET 255, 255, 252, 0

// Every board in the lab, by last IP octet (192.168.71.x). Commands sent
// with ?all=1 (which the control-panel buttons use) are forwarded to all the
// others, so pressing FREAKOUT on any board convulses the whole lab. Add an
// octet here when a new board joins.
static const int ALL_BOARD_OCTETS[] = {201, 202};
