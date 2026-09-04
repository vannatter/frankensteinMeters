// Frankenstein Meters — random needle flicker for old analog panel meters.
//
// Each meter is driven by high-frequency PWM through a series resistor; the
// meter movement averages the duty cycle into a deflection. Normal mode layers
// three behaviors per meter:
//   1. a slowly wandering idle baseline,
//   2. occasional surges toward full scale that decay back down,
//   3. constant fine jitter,
// then slews the output toward that target so the motion looks mechanical
// rather than digital.
//
// Freakout mode (triggered over the network or 'f' on serial) pins every
// needle high with violent thrashing until the timer runs out or /calm.
//
// HTTP API (also works from a browser):
//   /            → status
//   /freakout    → start a freakout (optional ?seconds=N, 0 = until /calm)
//   /sweep       → slow 0-100-0% calibration ramp on all meters, until /calm
//   /calm        → back to normal immediately
// Serial keys for testing without the network: f = freakout, s = sweep,
// c = calm.

#include <Arduino.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "secrets.h"

static const uint32_t TICK_MS = 10;

static float frand(float lo, float hi) {
    return lo + (hi - lo) * (esp_random() / 4294967295.0f);
}

class FlickerMeter {
public:
    void begin(const MeterProfile& profile, int channel) {
        p_ = &profile;
        channel_ = channel;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcAttach(p_->pin, PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
#else
        ledcSetup(channel_, PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
        ledcAttachPin(p_->pin, channel_);
#endif
        position_ = frand(p_->idleMin, p_->idleMax);
        wanderTarget_ = frand(p_->idleMin, p_->idleMax);
        speed_ = p_->speed * frand(0.85f, 1.15f);
        scheduleNextSurge();
        scheduleNextWander();
    }

    // Ease the needle down to zero and hold it there.
    void tickOff() {
        position_ += (0.0f - position_) * 0.1f;
        writeDuty(position_);
    }

    // Custom choreography: move to each position, hold for its duration, loop.
    // Fuzzy mode plays the same sequence loosely — positions wander ±10%,
    // hold times stretch/shrink, and the needle trembles — so the pattern
    // reads as intent rather than a metronome.
    void tickSeq(const uint8_t* pos, const uint16_t* dur, uint8_t n, bool fuzzy) {
        if (n == 0) { tickOff(); return; }
        uint32_t now = millis();
        if (stepIdx_ >= n) stepIdx_ = 0;
        if ((int32_t)(now - strobeFlipAt_) >= 0) {
            stepIdx_ = (uint8_t)((stepIdx_ + 1) % n);
            if (fuzzy) {
                strobeFlipAt_ = now + (uint32_t)(dur[stepIdx_] * frand(0.65f, 1.45f));
                wanderTarget_ = constrain(pos[stepIdx_] / 100.0f + frand(-0.10f, 0.10f), 0.0f, 1.0f);
            } else {
                strobeFlipAt_ = now + dur[stepIdx_];
                wanderTarget_ = pos[stepIdx_] / 100.0f;
            }
        }
        float target = wanderTarget_ + (fuzzy ? frand(-0.012f, 0.012f) : 0.0f);
        position_ += (target - position_) * min(speed_ * 3.0f, 0.5f);
        writeDuty(position_);
    }

    // The heartbeat as a preset any gauge can run.
    void tickBeat() { tickHeartbeat(millis(), false, false); }

    // Pegged: pinned near the top, trembling like it's overloaded.
    void tickPegged() {
        float target = 0.95f + frand(-0.02f, 0.02f);
        position_ += (target - position_) * speed_ * 2.0f;
        writeDuty(position_);
    }

    // Scan: slow patrol sweep, bottom to top and back, ~7s per cycle.
    void tickScan() {
        float ph = (millis() % 7000) / 7000.0f;
        float target = ph < 0.5f ? ph * 2.0f : 2.0f - ph * 2.0f;
        position_ += (target - position_) * 0.08f;
        writeDuty(position_);
    }

    // Sputter: dying instrument — slumped near zero with weak twitches.
    void tickSputter() {
        uint32_t now = millis();
        if ((int32_t)(now - nextWanderAt_) >= 0) {
            wanderTarget_ = frand(0.0f, 1.0f) < 0.3f ? frand(0.10f, 0.28f)
                                                     : frand(0.02f, 0.06f);
            nextWanderAt_ = now + (uint32_t)frand(300.0f, 1800.0f);
        }
        position_ += (wanderTarget_ - position_) * 0.12f;
        writeDuty(position_);
    }

    // Light channel: show the selected pattern — except panic always strobes
    // and coma always darkens, regardless of the pattern.
    void tickLight(LightPattern pat, bool freakout, bool coma,
                   const uint16_t* steps = nullptr, uint8_t nsteps = 0) {
        uint32_t now = millis();
        if (freakout) pat = LP_STROBE;
        else if (coma) pat = LP_DARK;

        switch (pat) {
            case LP_DARK:
                strobeOn_ = false;
                writeDuty(0.0f);
                break;
            case LP_STEADY:
                writeDuty(1.0f);
                break;
            case LP_DOUBLE: {
                // on-on-off: blink, blink, longer pause. ~1.1 s cycle.
                uint32_t t = now % 1100;
                writeDuty((t < 200 || (t >= 350 && t < 550)) ? 1.0f : 0.0f);
                break;
            }
            case LP_BREATHE: {
                float ph = (now % 2600) / 2600.0f;
                writeDuty(0.05f + 0.475f * (1.0f - cosf(2.0f * PI * ph)));
                break;
            }
            case LP_CANDLE:
                // Organic gas-lamp wander between 35% and 100%.
                if ((int32_t)(now - nextWanderAt_) >= 0) {
                    wanderTarget_ = frand(0.35f, 1.0f);
                    nextWanderAt_ = now + (uint32_t)frand(60.0f, 240.0f);
                }
                position_ += (wanderTarget_ - position_) * 0.15f;
                writeDuty(position_);
                break;
            case LP_STROBE:
                if ((int32_t)(now - strobeFlipAt_) >= 0) {
                    strobeOn_ = !strobeOn_;
                    strobeFlipAt_ = now + (uint32_t)(strobeOn_
                        ? frand(STROBE_ON_MS_MIN, STROBE_ON_MS_MAX)
                        : frand(STROBE_OFF_MS_MIN, STROBE_OFF_MS_MAX));
                    writeDuty(strobeOn_ ? 1.0f : 0.0f);
                }
                break;
            case LP_RANDOM:
                // Unpredictable organic blinking, slower than the strobe.
                if ((int32_t)(now - strobeFlipAt_) >= 0) {
                    strobeOn_ = !strobeOn_;
                    strobeFlipAt_ = now + (uint32_t)frand(90.0f, 1400.0f);
                    writeDuty(strobeOn_ ? 1.0f : 0.0f);
                }
                break;
            case LP_CUSTOM:
                // User-edited rhythm: alternating on/off durations.
                if (!steps || nsteps == 0) { writeDuty(0.0f); break; }
                if (stepIdx_ >= nsteps) stepIdx_ = 0;
                if ((int32_t)(now - strobeFlipAt_) >= 0) {
                    stepIdx_ = (uint8_t)((stepIdx_ + 1) % nsteps);
                    strobeFlipAt_ = now + steps[stepIdx_];
                    writeDuty((stepIdx_ % 2 == 0) ? 1.0f : 0.0f);
                }
                break;
        }
    }

    void tick(bool freakout, bool coma) {
        uint32_t now = millis();
        float target;

        if (p_->style == STYLE_HEARTBEAT) {
            tickHeartbeat(now, freakout, coma);
            return;
        }

        // (STYLE_LIGHT channels are driven via tickLight from the main loop.)

        if (p_->style == STYLE_LUNG && freakout) {
            // Panting: rapid frantic breaths instead of generic slams.
            if (beatStart_ == 0 || (int32_t)(now - nextWanderAt_) >= 0) {
                beatVary_ = frand(0.8f, 1.15f);
                beatStart_ = now;
                nextWanderAt_ = now + (uint32_t)(950.0f * beatVary_);
            }
            float ph = (now - beatStart_) / (950.0f * beatVary_);
            target = 0.15f + 0.375f * (1.0f - cosf(2.0f * PI * ph)) + frand(-0.02f, 0.02f);
            position_ += (target - position_) * 0.25f;
            writeDuty(position_);
            return;
        }

        if (freakout) {
            // Being shocked: bursts of full-scale slams, then a seizure hold —
            // locked up trembling near the top — then back to slamming.
            surge_ = 0.0f;
            if (seizing_) {
                if ((int32_t)(now - seizeEndsAt_) >= 0) {
                    seizing_ = false;
                    slamsLeft_ = SLAMS_PER_BURST_MIN +
                                 (int)frand(0.0f, (float)(SLAMS_PER_BURST_MAX - SLAMS_PER_BURST_MIN));
                    nextWanderAt_ = now;
                } else {
                    // Rigid tremble pinned high, retargeted every tick (100 Hz).
                    target = frand(SEIZE_MIN, SEIZE_MAX);
                    position_ += (target - position_) * FREAK_SPEED;
                    writeDuty(position_);
                    return;
                }
            }
            if ((int32_t)(now - nextWanderAt_) >= 0) {
                freakHigh_ = !freakHigh_;
                if (freakHigh_ && --slamsLeft_ <= 0) {
                    seizing_ = true;
                    seizeEndsAt_ = now + (uint32_t)frand(SEIZE_MS_MIN, SEIZE_MS_MAX);
                }
                wanderTarget_ = freakHigh_ ? frand(FREAK_HIGH_MIN, FREAK_HIGH_MAX)
                                           : frand(FREAK_LOW_MIN, FREAK_LOW_MAX);
                nextWanderAt_ = now + (uint32_t)frand(FREAK_FLIP_MS_MIN, FREAK_FLIP_MS_MAX);
            }
            target = wanderTarget_ + frand(-FREAK_JITTER, FREAK_JITTER);
            position_ += (target - position_) * FREAK_SPEED;
        } else if (coma && p_->style == STYLE_LUNG) {
            // Still breathing, barely: slow shallow breaths, 3-20%.
            if (beatStart_ == 0 || (int32_t)(now - nextWanderAt_) >= 0) {
                beatVary_ = frand(0.9f, 1.25f);
                beatStart_ = now;
                nextWanderAt_ = now + (uint32_t)(6200.0f * beatVary_);
            }
            float ph = (now - beatStart_) / (6200.0f * beatVary_);
            target = 0.03f + 0.085f * (1.0f - cosf(2.0f * PI * ph)) + frand(-0.004f, 0.004f);
            position_ += (target - position_) * 0.06f;
        } else if (coma && p_->style == STYLE_SPASTIC) {
            // Weak dying twitches near the bottom.
            if ((int32_t)(now - nextWanderAt_) >= 0) {
                wanderTarget_ = frand(0.0f, 1.0f) < 0.35f ? frand(0.08f, 0.22f)
                                                          : frand(0.01f, 0.05f);
                nextWanderAt_ = now + (uint32_t)frand(600.0f, 2500.0f);
            }
            position_ += (wanderTarget_ - position_) * 0.15f;
        } else if (coma) {
            // Barely alive: slow low drift with a faint stir now and then.
            if ((int32_t)(now - nextWanderAt_) >= 0) {
                wanderTarget_ = frand(COMA_MIN, COMA_MAX);
                nextWanderAt_ = now + (uint32_t)frand(2000.0f, 6000.0f);
            }
            if (surge_ > 0.001f) {
                surge_ *= 0.995f;
            } else if ((int32_t)(now - nextSurgeAt_) >= 0) {
                surge_ = frand(COMA_STIR_MIN, COMA_STIR_MAX) - wanderTarget_;
                nextSurgeAt_ = now + (uint32_t)(frand(COMA_STIR_INTERVAL_MIN_S,
                                                      COMA_STIR_INTERVAL_MAX_S) * 1000.0f);
            }
            target = wanderTarget_ + surge_ + frand(-COMA_JITTER, COMA_JITTER);
            position_ += (target - position_) * COMA_SPEED;
        } else if (p_->style == STYLE_SCAN) {
            // Patrol sweep at rest: slow full-range triangle with a tremble.
            // Overshoot the ends slightly so the lagging needle truly reaches
            // zero and full scale instead of turning around early.
            float ph = (now % 7000) / 7000.0f;
            float trit = ph < 0.5f ? ph * 2.0f : 2.0f - ph * 2.0f;
            trit = constrain(trit * 1.12f - 0.06f, 0.0f, 1.0f);
            target = constrain(trit + frand(-p_->jitter, p_->jitter), 0.0f, 1.0f);
            position_ += (target - position_) * 0.08f;
        } else if (p_->style == STYLE_LUNG) {
            // Breathing: slow inhale/exhale, each breath slightly different.
            if (beatStart_ == 0 || (int32_t)(now - nextWanderAt_) >= 0) {
                beatVary_ = frand(0.85f, 1.2f);
                beatStart_ = now;
                nextWanderAt_ = now + (uint32_t)(4400.0f * beatVary_);
            }
            float ph = (now - beatStart_) / (4400.0f * beatVary_);
            target = 0.10f + 0.375f * (1.0f - cosf(2.0f * PI * ph)) + frand(-p_->jitter, p_->jitter);
            position_ += (target - position_) * 0.10f;
        } else if (p_->style == STYLE_SPASTIC) {
            // Twitchy patrol: fast sweep constantly convulsing off-course.
            if ((int32_t)(now - nextWanderAt_) >= 0) {
                wanderTarget_ = frand(-0.18f, 0.18f);
                nextWanderAt_ = now + (uint32_t)frand(90.0f, 400.0f);
            }
            float ph = (now % 3500) / 3500.0f;
            float trit = ph < 0.5f ? ph * 2.0f : 2.0f - ph * 2.0f;
            target = constrain(trit + wanderTarget_ + frand(-p_->jitter, p_->jitter), 0.0f, 1.0f);
            position_ += (target - position_) * 0.20f;
        } else {
            // Slow idle wander: pick a new spot in the idle band now and then.
            if ((int32_t)(now - nextWanderAt_) >= 0) {
                wanderTarget_ = frand(p_->idleMin, p_->idleMax);
                scheduleNextWander();
            }

            // Surges: kick toward full scale, then decay back to baseline.
            if (surge_ > 0.001f) {
                surge_ *= surgeDecay_;
            } else if ((int32_t)(now - nextSurgeAt_) >= 0) {
                surge_ = frand(p_->surgeMin, p_->surgeMax) - wanderTarget_;
                surgeDecay_ = frand(0.96f, 0.99f);
                scheduleNextSurge();
            }

            target = wanderTarget_ + surge_ + frand(-p_->jitter, p_->jitter);
            position_ += (target - position_) * speed_;
        }

        writeDuty(position_);
    }

    // Last commanded output level, 0..1 — what the physical needle/light is
    // being told to do right now. Feeds the dashboard's live visualization.
    float level() const { return lastDuty_; }

    void writeDuty(float frac) {
        lastDuty_ = constrain(frac, 0.0f, 1.0f);
        int duty = (int)(constrain(frac, 0.0f, 1.0f) * p_->fullScaleDuty);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
        ledcWrite(p_->pin, duty);
#else
        ledcWrite(channel_, duty);
#endif
    }

private:
    // Lub-dub: sharp main pulse, dip, smaller second pulse, then rest until
    // the next beat. Beat length varies a little so it reads as organic.
    void tickHeartbeat(uint32_t now, bool freakout, bool coma) {
        float period = HEART_NORMAL_MS, amp = HEART_NORMAL_AMP, base = HEART_NORMAL_BASE;
        if (freakout) { period = HEART_FREAK_MS; amp = HEART_FREAK_AMP; base = HEART_FREAK_BASE; }
        else if (coma) { period = HEART_COMA_MS; amp = HEART_COMA_AMP; base = HEART_COMA_BASE; }

        if (now - beatStart_ >= (uint32_t)(period * beatVary_)) {
            beatStart_ = now;
            beatVary_ = frand(0.92f, 1.08f);
        }
        // bump-bump ... pause: small sine hump, breath, bigger sine hump,
        // then flat rest. Humps are kept wide and the slew gentle so a heavy
        // needle rides the curve instead of getting yanked and ringing.
        float ph = (now - beatStart_) / period;
        float target = base;
        if (ph < 0.20f)      target = base + (amp * 0.68f - base) * sinf(PI * (ph / 0.20f));
        else if (ph < 0.26f) target = base;
        else if (ph < 0.49f) target = base + (amp - base) * sinf(PI * ((ph - 0.26f) / 0.23f));
        position_ += (target - position_) * 0.22f;
        writeDuty(position_);
    }

    void scheduleNextSurge() {
        nextSurgeAt_ = millis() + (uint32_t)(frand(p_->surgeSMin, p_->surgeSMax) * 1000.0f);
    }

    void scheduleNextWander() {
        nextWanderAt_ = millis() + (uint32_t)frand(400.0f, 2500.0f);
    }

    const MeterProfile* p_ = nullptr;
    int channel_ = 0;
    float position_ = 0.0f;
    float wanderTarget_ = 0.0f;
    float surge_ = 0.0f;
    float surgeDecay_ = 0.98f;
    float speed_ = 0.08f;
    float lastDuty_ = 0.0f;
    bool freakHigh_ = false;
    bool seizing_ = false;
    int slamsLeft_ = SLAMS_PER_BURST_MIN;
    uint32_t beatStart_ = 0;
    float beatVary_ = 1.0f;
    bool strobeOn_ = false;
    uint32_t strobeFlipAt_ = 0;
    uint8_t stepIdx_ = 0;
    uint32_t seizeEndsAt_ = 0;
    uint32_t nextSurgeAt_ = 0;
    uint32_t nextWanderAt_ = 0;
};

static FlickerMeter meters[METER_COUNT];
static WebServer server(80);
static uint32_t lastTick = 0;

// Rolling log: everything worth seeing lands here as well as on serial, so
// the web control panel can show it.
static String logLines[24];
static int logHead = 0;

static void logMsg(const String& msg) {
    Serial.println(msg);
    logLines[logHead] = String(millis() / 1000) + "s  " + msg;
    logHead = (logHead + 1) % 24;
}

// Freakout state: 0 = calm. Otherwise the millis() deadline, or UINT32_MAX
// for "until /calm".
static uint32_t freakoutUntil = 0;

// Calibration sweep mode: all meters ramp 0-100-0% slowly until /calm.
static bool sweepMode = false;

// Coma mode: barely-alive baseline. Persists through freakouts — a shock
// convulsion ends and he sinks back into the coma.
static bool comaMode = false;

// Per-meter override: FOLLOW obeys the board mode; the rest pin that one
// meter to a specific behavior regardless of what the lab is doing.
enum MeterOverride { OVR_FOLLOW, OVR_FLICKER, OVR_FREAKOUT, OVR_COMA, OVR_OFF,
                     OVR_CUSTOM, OVR_FUZZY, OVR_HEARTBEAT,
                     OVR_PEGGED, OVR_SCAN, OVR_SPUTTER };
static const char* OVR_NAMES[] = {"follow", "flicker", "freakout", "coma", "off",
                                  "custom", "fuzzy", "heartbeat",
                                  "pegged", "scan", "sputter"};
static MeterOverride overrides[METER_COUNT] = {};

// Current pattern of each STYLE_LIGHT channel (dashboard-selectable).
static const char* LP_NAMES[] = {"dark", "steady", "doubleblink", "breathe",
                                 "candle", "strobe", "random", "custom"};
static const int LP_COUNT = 8;
static LightPattern lightPatterns[METER_COUNT] = {};

// Channel names: defaults from the config table, renameable live from the
// dashboard (persisted in flash).
static String chanNames[METER_COUNT];

#define MAX_STEPS 16

// Custom gauge sequences: position (0-100%) held for a duration (ms), looped.
// Edited live from the dashboard, persisted in flash.
static uint8_t seqPos[METER_COUNT][MAX_STEPS];
static uint16_t seqDur[METER_COUNT][MAX_STEPS];
static uint8_t seqLen[METER_COUNT] = {};

static String seqToString(int idx) {
    String s;
    for (int i = 0; i < seqLen[idx]; i++) {
        if (i) s += ",";
        s += seqPos[idx][i];
        s += ":";
        s += seqDur[idx][i];
    }
    return s;
}

static bool parseSeq(const String& s, int idx) {
    uint8_t tp[MAX_STEPS];
    uint16_t td[MAX_STEPS];
    uint8_t n = 0;
    int start = 0;
    while (start < (int)s.length() && n < MAX_STEPS) {
        int comma = s.indexOf(',', start);
        if (comma < 0) comma = s.length();
        int colon = s.indexOf(':', start);
        if (colon < 0 || colon >= comma) return false;
        long p = s.substring(start, colon).toInt();
        long d = s.substring(colon + 1, comma).toInt();
        if (p < 0 || p > 100 || d < 20 || d > 20000) return false;
        tp[n] = (uint8_t)p;
        td[n] = (uint16_t)d;
        n++;
        start = comma + 1;
    }
    if (n == 0) return false;
    memcpy(seqPos[idx], tp, sizeof(tp));
    memcpy(seqDur[idx], td, sizeof(td));
    seqLen[idx] = n;
    return true;
}

// Custom blink rhythms: on/off durations in ms, alternating, starting with
// ON. Edited live from the dashboard, persisted in flash.
static uint16_t customSteps[METER_COUNT][MAX_STEPS];
static uint8_t customLen[METER_COUNT] = {};
static Preferences prefs;

static String stepsToString(int idx) {
    String s;
    for (int i = 0; i < customLen[idx]; i++) {
        if (i) s += ",";
        s += customSteps[idx][i];
    }
    return s;
}

static bool parseSteps(const String& s, int idx) {
    uint16_t tmp[MAX_STEPS];
    uint8_t n = 0;
    int start = 0;
    while (start < (int)s.length() && n < MAX_STEPS) {
        int comma = s.indexOf(',', start);
        if (comma < 0) comma = s.length();
        long v = s.substring(start, comma).toInt();
        if (v < 20 || v > 20000) return false;  // 20ms..20s per step
        tmp[n++] = (uint16_t)v;
        start = comma + 1;
    }
    if (n == 0) return false;
    memcpy(customSteps[idx], tmp, sizeof(tmp));
    customLen[idx] = n;
    return true;
}

static void savePatternPrefs(int idx) {
    prefs.putUChar((String("lp") + idx).c_str(), (uint8_t)lightPatterns[idx]);
    prefs.putString((String("cs") + idx).c_str(), stepsToString(idx));
}

// Kill switch: every pin dark/zero until rekindled.
static bool allOff = false;

// When nonzero, the Try-Me trigger output is high until this millis() time.
static uint32_t trymeOffAt = 0;

static void pulseTryme() {
#ifdef TRYME_PIN
    digitalWrite(TRYME_PIN, HIGH);
    trymeOffAt = millis() + TRYME_PULSE_MS;
    logMsg("try-me trigger pulsed");
#endif
}

static bool freakingOut() {
    if (freakoutUntil == 0) return false;
    if (freakoutUntil != UINT32_MAX && (int32_t)(millis() - freakoutUntil) >= 0) {
        freakoutUntil = 0;
        logMsg("freakout over, back to normal flicker");
        return false;
    }
    return true;
}

static void startFreakout(long seconds) {
    freakoutUntil = seconds <= 0 ? UINT32_MAX : millis() + (uint32_t)seconds * 1000;
    logMsg(seconds <= 0 ? String("FREAKOUT! (until calm)")
                        : "FREAKOUT! (" + String(seconds) + "s)");
    pulseTryme();
}

static void sendToBoard(int octet, const String& path) {
    HTTPClient http;
    String url = BOARD_IP_PREFIX + String(octet) + path;
    http.begin(url);
    http.setConnectTimeout(1500);
    http.setTimeout(1500);
    int code = http.GET();
    http.end();
    logMsg("→ board ." + String(octet) + " " + path +
           (code > 0 ? " ok" : " unreachable"));
}

// If the request carried ?all=1, repeat it to every other board in
// ALL_BOARD_OCTETS (without the all flag, so it doesn't bounce around).
static void forwardToPeers(const char* path) {
    if (!server.hasArg("all") || WiFi.status() != WL_CONNECTED) return;
    for (unsigned i = 0; i < sizeof(ALL_BOARD_OCTETS) / sizeof(int); i++) {
        if (ALL_BOARD_OCTETS[i] == 200 + BOARD_ID) continue;
        sendToBoard(ALL_BOARD_OCTETS[i], path);
    }
}

// If the request carried ?board=N and N is some other board, relay the
// command there instead of acting locally. Returns true when relayed.
static bool relayedToBoard(const String& path) {
    if (!server.hasArg("board")) return false;
    int b = server.arg("board").toInt();
    if (b == BOARD_ID) return false;
    sendToBoard(200 + b, path);
    server.send(200, "application/json", "{\"relayed\":" + String(b) + "}\n");
    return true;
}

static const char* currentModeName() {
    if (allOff) return "off";
    if (sweepMode) return "sweep";
    if (freakingOut()) return "freakout";
    if (comaMode) return "coma";
    return "flicker";
}

static void handleStatus() {
    String body = "{\"board\":";
    body += BOARD_ID;
    body += ",\"mode\":\"";
    body += currentModeName();
    body += "\",\"meters\":[";
    for (int i = 0; i < METER_COUNT; i++) {
        bool isLight = METERS[i].style == STYLE_LIGHT;
        if (i) body += ",";
        body += "{\"name\":\"";
        body += chanNames[i];
        body += "\",\"pin\":";
        body += METERS[i].pin;
        body += ",\"type\":\"";
        body += isLight ? "light" : "meter";
        body += "\",\"style\":\"";
        body += METERS[i].style == STYLE_HEARTBEAT ? "heartbeat"
                : METERS[i].style == STYLE_SCAN ? "scan"
                : METERS[i].style == STYLE_LUNG ? "lung"
                : METERS[i].style == STYLE_SPASTIC ? "spastic"
                : (isLight ? "light" : "flicker");
        body += "\",\"mode\":\"";
        body += isLight ? LP_NAMES[lightPatterns[i]] : OVR_NAMES[overrides[i]];
        body += "\",\"steps\":\"";
        body += isLight ? stepsToString(i) : seqToString(i);
        body += "\"}";
    }
    body += "]}\n";
    server.send(200, "application/json", body);
}

// /set?meter=<1-4 or name>&mode=<value> — for meter channels the value is an
// override (follow|flicker|freakout|coma|off); for light channels it's a
// pattern (dark|steady|doubleblink|breathe|candle|strobe).
static void handleSet() {
    String m = server.arg("meter");
    String mode = server.arg("mode");
    int idx = -1;
    for (int i = 0; i < METER_COUNT; i++) {
        if (m == chanNames[i] || m == METERS[i].name || m.toInt() == i + 1) { idx = i; break; }
    }
    if (idx < 0) {
        server.send(400, "application/json", "{\"error\":\"bad meter\"}\n");
        return;
    }
    if (METERS[idx].style == STYLE_LIGHT) {
        for (unsigned i = 0; i < sizeof(LP_NAMES) / sizeof(char*); i++) {
            if (mode == LP_NAMES[i]) {
                lightPatterns[idx] = (LightPattern)i;
                savePatternPrefs(idx);
                logMsg(chanNames[idx] + " → " + mode);
                server.send(200, "application/json", "{\"ok\":true}\n");
                return;
            }
        }
    } else {
        for (unsigned i = 0; i < sizeof(OVR_NAMES) / sizeof(char*); i++) {
            if (mode == OVR_NAMES[i]) {
                overrides[idx] = (MeterOverride)i;
                logMsg(chanNames[idx] + " → " + mode);
                server.send(200, "application/json", "{\"ok\":true}\n");
                return;
            }
        }
    }
    server.send(400, "application/json", "{\"error\":\"bad mode\"}\n");
}

// /pattern?meter=<1-4 or name>&steps=200,150,200,600 — set a light's custom
// blink rhythm (on/off ms, alternating, starts ON) and switch it to that
// pattern. Live, no reflash; persisted in flash.
static void handlePattern() {
    String m = server.arg("meter");
    int idx = -1;
    for (int i = 0; i < METER_COUNT; i++) {
        if (m == chanNames[i] || m == METERS[i].name || m.toInt() == i + 1) { idx = i; break; }
    }
    if (idx < 0 || METERS[idx].style != STYLE_LIGHT) {
        server.send(400, "application/json", "{\"error\":\"not a light\"}\n");
        return;
    }
    if (!parseSteps(server.arg("steps"), idx)) {
        server.send(400, "application/json",
                    "{\"error\":\"steps must be 1-16 comma-separated ms values, 20-20000\"}\n");
        return;
    }
    lightPatterns[idx] = LP_CUSTOM;
    savePatternPrefs(idx);
    logMsg(chanNames[idx] + " custom: " + stepsToString(idx));
    server.send(200, "application/json", "{\"ok\":true}\n");
}

// /mpattern?meter=<1-N or name>&steps=80:500,20:300,... — set a gauge's
// custom choreography (position%:hold-ms pairs, looped) and switch the
// channel to it. Persisted in flash.
static void handleMPattern() {
    String m = server.arg("meter");
    int idx = -1;
    for (int i = 0; i < METER_COUNT; i++) {
        if (m == chanNames[i] || m == METERS[i].name || m.toInt() == i + 1) { idx = i; break; }
    }
    if (idx < 0 || METERS[idx].style == STYLE_LIGHT) {
        server.send(400, "application/json", "{\"error\":\"not a gauge\"}\n");
        return;
    }
    if (!parseSeq(server.arg("steps"), idx)) {
        server.send(400, "application/json",
                    "{\"error\":\"steps must be 1-16 pos:ms pairs (pos 0-100, ms 20-20000)\"}\n");
        return;
    }
    // Keep fuzzy playback if that's what the channel was set to.
    if (overrides[idx] != OVR_FUZZY) overrides[idx] = OVR_CUSTOM;
    prefs.putString((String("mq") + idx).c_str(), seqToString(idx));
    logMsg(chanNames[idx] + " choreography: " + seqToString(idx));
    server.send(200, "application/json", "{\"ok\":true}\n");
}

// /rename?meter=<1-N or name>&name=<newname> — rename a channel slot live.
// Persisted in flash; the config-table name remains the reset default.
static void handleRename() {
    String m = server.arg("meter");
    int idx = -1;
    for (int i = 0; i < METER_COUNT; i++) {
        if (m == chanNames[i] || m == METERS[i].name || m.toInt() == i + 1) { idx = i; break; }
    }
    String name = server.arg("name");
    String clean;
    for (unsigned i = 0; i < name.length() && clean.length() < 14; i++) {
        char c = name[i];
        if (isalnum(c) || c == '-' || c == '_') clean += c;
    }
    if (idx < 0 || clean.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"bad meter or name\"}\n");
        return;
    }
    logMsg(chanNames[idx] + " renamed to " + clean);
    chanNames[idx] = clean;
    prefs.putString((String("nm") + idx).c_str(), clean);
    server.send(200, "application/json", "{\"ok\":true}\n");
}

// /herd — the configured board roster (single source of truth: config.h's
// ALL_BOARD_OCTETS). The dashboard builds its herd card, live poller, and
// target dropdown from this instead of hardcoding addresses.
static void handleHerd() {
    String body = "{\"prefix\":\"" BOARD_IP_PREFIX "\",\"boards\":[";
    for (unsigned i = 0; i < sizeof(ALL_BOARD_OCTETS) / sizeof(int); i++) {
        if (i) body += ",";
        body += "{\"n\":";
        body += ALL_BOARD_OCTETS[i] - 200;
        body += ",\"o\":";
        body += ALL_BOARD_OCTETS[i];
        body += "}";
    }
    body += "]}\n";
    server.send(200, "application/json", body);
}

// /live — current output level of every channel, 0-100. Polled fast by the
// dashboard's lab visualization.
static void handleLive() {
    String body = "{\"v\":[";
    for (int i = 0; i < METER_COUNT; i++) {
        if (i) body += ",";
        body += (int)(meters[i].level() * 100.0f + 0.5f);
    }
    body += "]}\n";
    server.send(200, "application/json", body);
}

static void handleLog() {
    String body;
    for (int i = 0; i < 24; i++) {
        const String& line = logLines[(logHead + i) % 24];
        if (line.length()) body += line + "\n";
    }
    server.send(200, "text/plain", body);
}

static void handleFreakout() {
    long seconds = FREAKOUT_DEFAULT_S;
    if (server.hasArg("seconds")) seconds = server.arg("seconds").toInt();
    String path = seconds == FREAKOUT_DEFAULT_S
                      ? String("/freakout")
                      : String("/freakout?seconds=") + seconds;
    if (relayedToBoard(path)) return;
    sweepMode = false;
    allOff = false;
    startFreakout(seconds);
    forwardToPeers(path.c_str());
    server.send(200, "application/json", "{\"mode\":\"freakout\"}\n");
}

static void handleCalm() {
    if (relayedToBoard("/calm")) return;
    freakoutUntil = 0;
    sweepMode = false;
    comaMode = false;
    allOff = false;
    logMsg("calmed by request");
    forwardToPeers("/calm");
    server.send(200, "application/json", "{\"mode\":\"flicker\"}\n");
}

static void handleOff() {
    if (relayedToBoard("/off")) return;
    allOff = true;
    freakoutUntil = 0;
    sweepMode = false;
    logMsg("all pins extinguished");
    forwardToPeers("/off");
    server.send(200, "application/json", "{\"mode\":\"off\"}\n");
}

static void handleComa() {
    if (relayedToBoard("/coma")) return;
    comaMode = true;
    sweepMode = false;
    freakoutUntil = 0;
    allOff = false;
    logMsg("coma — barely alive");
    forwardToPeers("/coma");
    server.send(200, "application/json", "{\"mode\":\"coma\"}\n");
}

static void handleSweep() {
    if (relayedToBoard("/sweep")) return;
    sweepMode = true;
    freakoutUntil = 0;
    allOff = false;
    logMsg("calibration sweep on");
    forwardToPeers("/sweep");
    server.send(200, "application/json", "{\"mode\":\"sweep\"}\n");
}

// Phone/browser control panel with live log, served at /.
static const char PANEL_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Frankenstein Meters</title>
<link href="https://fonts.googleapis.com/css2?family=IM+Fell+English:ital@0;1&family=IM+Fell+English+SC&display=swap" rel="stylesheet">
<style>
:root{--ink:#e4d5b3;--dim:#9d8b66;--edge:#4a3a26;--edge2:#2b2115;
--green:#3e5238;--red:#6e2a2e;--blue:#394663;--amber:#6e5629}
*{box-sizing:border-box}
body{margin:0;padding:1.2rem 1rem;color:var(--ink);
background:#0c0906 radial-gradient(ellipse 120% 80% at 50% -10%,#241a10 0%,#120d08 55%,#0c0906 100%) no-repeat;
min-height:100vh;font-family:'IM Fell English',Georgia,serif}
.wrap{max-width:26rem;margin:0 auto}
.masthead{text-align:center;margin-bottom:1.1rem}
h1{font-family:'IM Fell English SC',Georgia,serif;font-weight:400;font-size:2.1rem;
letter-spacing:.1em;margin:0;text-shadow:0 0 22px rgba(228,190,110,.22)}
.sub{font-style:italic;color:var(--dim);font-size:.95rem;margin:.1rem 0 .6rem}
#pill{display:inline-block;font-variant:small-caps;letter-spacing:.14em;font-size:.95rem;
padding:.2rem 1.1rem;border:1px solid var(--edge);border-radius:2px;color:#f0e6cc;
background:#333;box-shadow:inset 0 0 12px rgba(0,0,0,.55);transition:background .4s}
.orn{text-align:center;color:#6b5837;font-size:1.15rem;margin:.75rem 0;user-select:none}
.card{background:linear-gradient(#17120d,#110d09);border:3px double var(--edge);
border-radius:3px;padding:1rem 1.1rem;box-shadow:0 2px 26px rgba(0,0,0,.65),inset 0 0 46px rgba(0,0,0,.45)}
label{display:block;text-align:center;font-variant:small-caps;letter-spacing:.28em;
color:var(--dim);font-size:.85rem;margin-bottom:.55rem}
select{width:100%;font-family:inherit;font-size:1rem;padding:.55rem .7rem;border-radius:2px;
border:1px solid var(--edge);background:#14100b;color:var(--ink);cursor:pointer}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:.65rem;margin-top:.85rem}
button{font-family:inherit;font-variant:small-caps;font-size:1.1rem;letter-spacing:.08em;
display:flex;flex-direction:column;align-items:center;gap:.1rem;
padding:.7rem .4rem;border-radius:2px;color:var(--ink);cursor:pointer;user-select:none;
background:#161009;border:1px solid var(--edge);outline:1px solid var(--edge2);outline-offset:-5px;
transition:transform .06s,filter .15s,box-shadow .2s}
button small{font-variant:normal;text-transform:uppercase;letter-spacing:.22em;
font-size:.58rem;color:var(--dim)}
button:hover{filter:brightness(1.25);box-shadow:0 0 16px rgba(222,170,80,.18)}
button:active{transform:translateY(2px)}
.b-freak{color:#d69090;border-color:#6e2a2e}
.b-calm{color:#a9bf99;border-color:#3e5238}
.b-coma{color:#9daccf;border-color:#394663}
.b-sweep{color:#d3b075;border-color:#6e5629}
.b-off{grid-column:1/-1;color:#8f8f8f;border-color:#3c3c3c}
.b-off.lit{color:#ffd98a;border-color:#6e5629}
button.on{box-shadow:inset 0 0 18px rgba(0,0,0,.7),0 0 14px currentColor;filter:brightness(1.2)}
.chan{padding:.5rem 0;border-bottom:1px solid var(--edge2)}
.chan:last-child{border-bottom:0}
.mrow{display:flex;align-items:center;justify-content:space-between}
.pbar{display:flex;align-items:flex-end;height:15px;border:1px solid var(--edge2);
border-radius:2px;overflow:hidden;cursor:pointer;margin-top:.45rem;background:#0d0a07}
.pbar .on{background:#c9a44a;height:100%}.pbar .off{background:#241c12;height:100%}
.pbar .gs{background:#8fa8c9}
.hrow{display:flex;justify-content:space-between;align-items:center;padding:.35rem 0;
border-bottom:1px solid var(--edge2)}
.hrow:last-child{border-bottom:0}
.hrow .on2{color:#8fd18f;font-size:.85rem}
.hrow .off2{color:#6b6b6b;font-size:.85rem}
.wstrip{display:flex;gap:.7rem;flex-wrap:wrap;padding:.45rem 0 .55rem;
border-bottom:1px solid var(--edge2)}
.witem{text-align:center}
.gw{width:54px;height:30px;position:relative;overflow:hidden;margin:0 auto;
border:1px solid var(--edge2);border-radius:54px 54px 0 0;background:#0d0a07}
.gn{position:absolute;left:50%;bottom:-1px;width:2px;height:25px;margin-left:-1px;
background:#e6d8b8;transform-origin:bottom center;transform:rotate(-80deg)}
.lw{width:22px;height:22px;border-radius:50%;margin:4px auto;
background:#241c12;border:1px solid var(--edge2)}
.wname{font-size:.58rem;letter-spacing:.12em;color:var(--dim);margin-top:.2rem;
text-transform:uppercase}
#pgauge{height:12px;border:1px solid var(--edge);border-radius:2px;background:#0d0a07;
margin:.6rem 0 .2rem;overflow:hidden}
#pfill{height:100%;width:0%;background:#8fa8c9;transition:width .25s}
#edmodal{position:fixed;inset:0;background:rgba(5,4,2,.85);display:flex;
align-items:center;justify-content:center;z-index:9;padding:1rem}
#edmodal[hidden]{display:none}
.edcard{background:linear-gradient(#17120d,#110d09);border:3px double var(--edge);
border-radius:3px;padding:1rem 1.1rem;width:100%;max-width:24rem;max-height:90vh;overflow-y:auto}
.erow{display:flex;align-items:center;gap:.5rem;margin:.35rem 0}
.erow span{width:2.4rem;font-size:.78rem;letter-spacing:.1em;color:var(--dim)}
.erow input[type=range]{flex:1;accent-color:#c9a44a}
.erow b{width:3.6rem;text-align:right;font-size:.78rem;font-weight:400}
.erow button{flex:none;display:inline;padding:.1rem .5rem;font-size:.8rem;outline:none}
#plamp{width:18px;height:18px;border-radius:50%;background:#241c12;
border:1px solid var(--edge);margin:.6rem auto .2rem;transition:background .05s}
#plamp.lit{background:#ffd257;box-shadow:0 0 14px #ffd257}
.ebtns{display:flex;gap:.5rem;margin-top:.6rem}
.ebtns button{flex:1;padding:.55rem;font-size:.9rem;outline:none}
#edtext{width:100%;font:.8rem Menlo,monospace;padding:.4rem .5rem;margin-top:.5rem;
border-radius:2px;border:1px solid var(--edge2);background:#0d0a07;color:var(--ink);box-sizing:border-box}
.mname{font-variant:small-caps;letter-spacing:.12em;font-size:1.05rem}
.mpin{color:var(--dim);font:.68rem Menlo,Consolas,monospace;font-style:normal;
margin-left:.4rem;letter-spacing:.03em}
.mrow select{width:9.5rem}
.mctl{display:flex;align-items:center;gap:.45rem}
.pwr{display:inline;flex:none;width:1.9rem;height:1.9rem;padding:0;line-height:1;
font-size:.95rem;border-radius:50%;outline:none}
.pwr.on3{color:#8fd18f;border-color:#3e5238}
.pwr.off3{color:#555;border-color:#333}
pre{background:#0a0805;border:1px solid var(--edge2);border-radius:2px;color:#a8b3a0;
text-align:left;padding:.8rem;margin:0;min-height:6rem;max-height:11rem;overflow:auto;
font:.72rem/1.6 Menlo,Consolas,monospace;white-space:pre-wrap}
footer{text-align:center;color:#5d4c30;font-style:italic;font-size:.8rem;margin:1rem 0 .3rem}
.cols{display:block}
@media(min-width:880px){
.wrap{max-width:58rem}
.cols{display:grid;grid-template-columns:26rem 1fr;gap:1.4rem;align-items:start}
.colR{position:sticky;top:1rem}
.colR pre{max-height:calc(100vh - 13rem);min-height:22rem}
.orn.mobile{display:none}}
</style></head><body><div class="wrap">
<div class="masthead"><h1>Frankenstein</h1>
<div class="sub">&mdash; or, the Modern Prometheus &mdash;</div>
<span id="pill">&hellip;</span></div>
<div class="cols"><div class="colL">
<div class="card">
<label>The Experiment</label>
<select id="tgt" onchange="refresh()">
<option value="all" selected>The whole laboratory</option>
</select>
<div class="grid">
<button class="b-freak" data-m="freakout" onclick="hit('/freakout')">&#9889; Galvanize<small>freak out</small></button>
<button class="b-calm" data-m="flicker" onclick="hit('/calm')">Calm<small>steady flicker</small></button>
<button class="b-coma" data-m="coma" onclick="hit('/coma')">&#9790; Coma<small>barely alive</small></button>
<button class="b-sweep" data-m="sweep" onclick="hit('/sweep')">Calibrate<small>slow sweep</small></button>
<button class="b-off" data-m="off" id="offbtn" onclick="toggleOff()">&#9760; Extinguish All<small>kill every pin</small></button>
</div></div>
<div class="orn">&#10087;</div>
<div class="card"><label>The Herd</label><div id="herd"></div></div>
<div class="orn">&#10087;</div>
<div class="card"><label id="instlabel">The Instruments</label><div id="meters"></div></div>
<div class="orn mobile">&#10087;</div>
</div><div class="colR">
<div class="card"><label id="jlabel">The Journal</label><pre id="log"></pre></div>
</div></div>
<footer>&ldquo;It was on a dreary night of November&hellip;&rdquo;</footer>
</div>
<div id="edmodal" hidden><div class="edcard">
<label id="edtitle">Rhythm Editor</label>
<div id="edbar" class="pbar" style="cursor:default"></div>
<div id="plamp"></div>
<div id="pgauge" hidden><div id="pfill"></div></div>
<div id="edrows"></div>
<div class="ebtns"><button onclick="edAdd()">+ Add Step</button></div>
<input id="edtext" onchange="edFromText()">
<div class="ebtns">
<button class="b-calm" onclick="edApply()">Apply</button>
<button onclick="edClose()">Cancel</button>
</div></div></div>
<script>
const OVRS=['follow','flicker','freakout','coma','off','custom','fuzzy','heartbeat','pegged','scan','sputter'];
let HERD=[],PREFIX='http://192.168.71.';
let herdMeta={},herdMode={},herdSig='';
async function loadHerd(){
 try{
  const h=await (await fetch('/herd')).json();
  PREFIX=h.prefix;
  HERD=h.boards.map(b=>[b.n,b.o]);
  const t=document.getElementById('tgt');
  HERD.forEach(([n,o])=>{
   const op=document.createElement('option');
   op.value=n;op.textContent='Board '+n+' (.'+o+')';
   t.appendChild(op);});
 }catch(e){}
 refresh();}
async function herdCheck(){
 const parts=await Promise.all(HERD.map(async([n,o])=>{
  try{
   const c=new AbortController();const t=setTimeout(()=>c.abort(),1500);
   const s=await (await fetch(PREFIX+o+'/status',{signal:c.signal})).json();
   clearTimeout(t);
   herdMeta[o]=s.meters;herdMode[o]=s.mode;
   return {n:n,o:o,ok:1,mode:s.mode,ms:s.meters};
  }catch(e){delete herdMeta[o];delete herdMode[o];return {n:n,o:o,ok:0};}
 }));
 const sig=JSON.stringify(parts.map(p=>[p.n,p.ok,p.mode,(p.ms||[]).map(m=>m.name+m.type)]));
 if(sig===herdSig)return;
 herdSig=sig;
 document.getElementById('herd').innerHTML=parts.map(p=>{
  let h='<div class="hrow" style="border-bottom:0"><span class="mname">BOARD '+p.n+
   '<span class="mpin">.'+p.o+'</span></span>'+
   (p.ok?'<span class="on2">&#9679; '+p.mode+'</span>'
        :'<span class="off2">&#9675; unreachable</span>');
  h+='</div>';
  if(p.ok)h+='<div class="wstrip">'+p.ms.map((m,i)=>
   '<div class="witem">'+(m.type==='light'
    ?'<div class="lw" id="lv'+p.o+'_'+i+'"></div>'
    :'<div class="gw"><div class="gn" id="lv'+p.o+'_'+i+'"></div></div>')+
   '<div class="wname">'+m.name+'</div></div>').join('')+'</div>';
  return h;}).join('');}
// The lab miniature: a 60fps client-side twin of the firmware's pattern
// engines. Deterministic patterns (heartbeat, blinks, breathe, sweep,
// custom sequences) use the same math as the boards; random behaviors
// (flicker, freakout, candle...) roll their own dice with the same
// character. Modes/patterns come from the 2s status refresh.
const R=(a,b)=>a+Math.random()*(b-a);
const tri=(t,p)=>{const x=(t%p)/p;return x<0.5?x*2:2-x*2;};
let simS={};
function heartSim(t,bm){
 let per=1700,amp=.65,base=.06;
 if(bm==='freakout'){per=550;amp=1;base=.1}
 else if(bm==='coma'){per=2800;amp=.3;base=.03}
 const ph=(t%per)/per;
 if(ph<0.20)return (base+(amp*0.68-base)*Math.sin(Math.PI*ph/0.20))*100;
 if(ph<0.26)return base*100;
 if(ph<0.49)return (base+(amp-base)*Math.sin(Math.PI*(ph-0.26)/0.23))*100;
 return base*100;}
function seqAt(steps,t,gauge){
 const st=(steps||'').split(',').map(s=>gauge?s.split(':').map(Number):Number(s))
  .filter(a=>gauge?a.length===2&&a[1]>0:a>0);
 if(!st.length)return 0;
 const tot=st.reduce((a,b)=>a+(gauge?b[1]:b),0);
 let x=t%tot;
 for(let j=0;j<st.length;j++){
  const d=gauge?st[j][1]:st[j];
  if(x<d)return gauge?st[j][0]:(j%2?0:100);
  x-=d;}
 return 0;}
function gTarget(s,m,bm,t){
 const md=m.mode==='follow'?bm:m.mode;
 if(bm==='off'||md==='off')return 0;
 if(bm==='sweep')return tri(t,8000)*100;
 if(m.style==='heartbeat'&&m.mode==='follow')return heartSim(t,bm);
 if(m.style==='scan'&&m.mode==='follow'&&bm!=='freakout'&&bm!=='coma')
  return Math.max(0,Math.min(100,(tri(t,7000)*1.12-0.06)*100));
 if(m.style==='lung'&&m.mode==='follow'&&bm==='freakout'){
  const ph=(t%950)/950;return (0.15+0.375*(1-Math.cos(2*Math.PI*ph)))*100;}
 if(m.style==='lung'&&m.mode==='follow'&&bm==='coma'){
  const ph=(t%6200)/6200;return (0.03+0.085*(1-Math.cos(2*Math.PI*ph)))*100;}
 if(m.style==='lung'&&m.mode==='follow'){
  const ph=(t%4400)/4400;return (0.10+0.375*(1-Math.cos(2*Math.PI*ph)))*100;}
 if(m.style==='spastic'&&m.mode==='follow'&&bm==='coma'){
  if(t>s.nw){s.wt=Math.random()<0.35?R(8,22):R(1,5);s.nw=t+R(600,2500);}
  return s.wt||3;}
 if(m.style==='spastic'&&m.mode==='follow'&&bm!=='freakout'){
  if(t>s.nw){s.wt=R(-18,18);s.nw=t+R(90,400);}
  return Math.max(0,Math.min(100,tri(t,3500)*100+(s.wt||0)+R(-5,5)));}
 switch(md){
  case 'freakout':
   if(s.seizeTo>t)return R(90,100);
   if(t>s.flipAt){s.hi=!s.hi;s.flipAt=t+R(280,550);
    if(s.hi&&Math.random()<0.14)s.seizeTo=t+R(700,1600);
    s.wt=s.hi?R(88,100):R(0,12);}
   return s.wt||20;
  case 'coma':
   if(t>s.nw){s.wt=R(2,10);s.nw=t+R(2000,6000);}
   if(t>s.nsu){s.su=R(8,16);s.nsu=t+R(8000,25000);}
   s.su=(s.su||0)*0.995;
   return (s.wt||5)+s.su;
  case 'heartbeat':return heartSim(t,'flicker');
  case 'custom':return seqAt(m.steps,t,true);
  case 'fuzzy':
   if(t>s.fz){s.fz=t+R(400,900);s.fo=R(-8,8);}
   return Math.max(0,Math.min(100,seqAt(m.steps,t,true)+(s.fo||0)+R(-1,1)));
  case 'pegged':return 95+R(-2,2);
  case 'scan':return tri(t,7000)*100;
  case 'sputter':
   if(t>s.nw){s.wt=Math.random()<0.3?R(10,28):R(2,6);s.nw=t+R(300,1800);}
   return s.wt||4;
  default: // flicker
   if(t>s.nw){s.wt=R(15,45);s.nw=t+R(1200,3500);}
   if(t>s.nsu){s.su=R(35,70);s.nsu=t+R(4000,12000);}
   s.su=(s.su||0)*0.99;
   return (s.wt||25)+s.su+R(-2,2);
 }}
function lTarget(s,m,bm,t){
 if(bm==='off'||bm==='coma')return 0;
 if(bm==='freakout'){
  if(t>s.flipAt){s.on=!s.on;s.flipAt=t+(s.on?R(30,90):R(30,120));}
  return s.on?100:0;}
 if(bm==='sweep')return tri(t,8000)*100;
 switch(m.mode){
  case 'steady':return 100;
  case 'doubleblink':{const x=t%1100;return (x<200||(x>=350&&x<550))?100:0;}
  case 'breathe':{const ph=(t%2600)/2600;return (0.05+0.475*(1-Math.cos(2*Math.PI*ph)))*100;}
  case 'candle':
   if(t>s.nw){s.wt=R(35,100);s.nw=t+R(60,240);}
   s.c2=(s.c2===undefined?50:s.c2)+((s.wt||60)-(s.c2||50))*0.15;
   return s.c2;
  case 'strobe':
   if(t>s.flipAt){s.on=!s.on;s.flipAt=t+(s.on?R(30,90):R(30,120));}
   return s.on?100:0;
  case 'random':
   if(t>s.flipAt){s.on=!s.on;s.flipAt=t+R(90,1400);}
   return s.on?100:0;
  case 'custom':return seqAt(m.steps,t,false);
  default:return 0;}}
function animLab(){
 const t=performance.now();
 for(const o in herdMeta){
  const bm=(herdMode[o]||'flicker');
  herdMeta[o].forEach((m,i)=>{
   const el=document.getElementById('lv'+o+'_'+i);
   if(!el)return;
   const k=o+'_'+i;
   if(!simS[k])simS[k]={c:0,flipAt:0,nw:0,nsu:0,fz:0,seizeTo:0};
   const s=simS[k];
   if(m.type==='light'){
    const tv=lTarget(s,m,bm,t);
    s.c+=(tv-s.c)*0.6;
    el.style.background='rgba(255,213,87,'+(s.c/100)+')';
    el.style.boxShadow=s.c>15?'0 0 '+(4+s.c/8)+'px rgba(255,213,87,.8)':'none';
   }else{
    const tv=gTarget(s,m,bm,t);
    const md=m.mode==='follow'?bm:m.mode;
    s.c+=(tv-s.c)*(md==='freakout'?0.35:0.12);
    el.style.transform='rotate('+(-80+s.c*1.6)+'deg)';
   }});
 }
 requestAnimationFrame(animLab);}
requestAnimationFrame(animLab);
const LPATS=['dark','steady','doubleblink','breathe','candle','strobe','random','custom'];
const COLORS={flicker:'var(--green)',freakout:'var(--red)',coma:'var(--blue)',sweep:'var(--amber)',off:'#3a3a3a'};
let curMode='',curBase='',shownKey='x';
async function hit(p){
 const t=document.getElementById('tgt').value;
 const sep=p.includes('?')?'&':'?';
 try{await fetch(p+sep+(t==='all'?'all=1':'board='+t))}catch(e){}
 refresh()}
function toggleOff(){hit(curMode==='off'?'/calm':'/off')}
async function setM(n,v){try{await fetch(curBase+'/set?meter='+n+'&mode='+v)}catch(e){};refresh()}
async function renameM(n,i){
 const cur=metersCache[i]?metersCache[i].name:'';
 const v=prompt('Rename channel (letters/numbers/dashes, max 14):',cur);
 if(!v||v===cur)return;
 try{await fetch(curBase+'/rename?meter='+n+'&name='+encodeURIComponent(v))}catch(e){}
 shownKey='x';refresh()}
let metersCache=[];
function barHTML(str){
 const st=(str||'').split(',').map(Number).filter(v=>v>0);
 if(!st.length)return'';
 const tot=st.reduce((a,b)=>a+b,0);
 return st.map((v,j)=>'<div class="'+(j%2?'off':'on')+'" style="width:'+(v/tot*100)+'%"></div>').join('');
}
function barHTMLG(str){
 const st=(str||'').split(',').map(s=>s.split(':').map(Number)).filter(a=>a.length===2&&a[1]>0);
 if(!st.length)return'';
 const tot=st.reduce((a,b)=>a+b[1],0);
 return st.map(a=>'<div class="gs" style="width:'+(a[1]/tot*100)+'%;height:'+Math.max(a[0],5)+'%"></div>').join('');
}
function renderMeters(ms,key){
 metersCache=ms;
 const box=document.getElementById('meters');
 // Rebuild whenever names/pins/types change, not just row count — a pin
 // remap in firmware must refresh the labels.
 const sig=key+'|'+ms.map(m=>m.name+':'+m.pin+':'+m.type).join(',');
 if(shownKey!==sig){
  shownKey=sig;
  box.innerHTML='';
  ms.forEach((m,i)=>{
   const opts=m.type==='light'?LPATS:OVRS;
   const d=document.createElement('div');d.className='chan';
   const isOff=(m.mode==='off'||m.mode==='dark');
   let h='<div class="mrow"><span class="mname" id="nm'+i+'" title="tap to rename" '+
    'style="cursor:pointer" onclick="renameM('+(i+1)+','+i+')">'+(m.type==='light'?'&#128367; ':'')+m.name.toUpperCase()+
    '<span class="mpin">pin '+m.pin+'</span></span>'+
    '<span class="mctl">'+
    '<button class="pwr '+(isOff?'off3':'on3')+'" id="pw'+i+'" title="toggle on/off" '+
    'onclick="togM('+(i+1)+','+i+')">&#9211;</button>'+
    '<select onchange="setM('+(i+1)+',this.value)">'+
    opts.map(o=>'<option'+(o===m.mode?' selected':'')+'>'+o+'</option>').join('')+'</select></span></div>';
   h+='<div class="pbar" id="pb'+i+'" title="tap to edit" onclick="edOpen('+i+')">'+
    (m.type==='light'?barHTML(m.steps):barHTMLG(m.steps))+'</div>';
   d.innerHTML=h;
   box.appendChild(d);});
 }else{
  ms.forEach((m,i)=>{
   const s=box.children[i].querySelector('select');
   if(document.activeElement!==s)s.value=m.mode;
   const pw=document.getElementById('pw'+i);
   if(pw)pw.className='pwr '+((m.mode==='off'||m.mode==='dark')?'off3':'on3');
   const pb=document.getElementById('pb'+i);
   if(pb&&edIdx<0)pb.innerHTML=m.type==='light'?barHTML(m.steps):barHTMLG(m.steps);});
 }}
// Power toggle: off/dark <-> whatever the channel was doing before.
let prevMode={};
async function togM(n,i){
 const m=metersCache[i];
 if(!m)return;
 const offVal=m.type==='light'?'dark':'off';
 let v;
 if(m.mode===offVal){
  v=prevMode[curBase+'|'+i]||(m.type==='light'?'steady':'follow');
 }else{
  prevMode[curBase+'|'+i]=m.mode;
  v=offVal;
 }
 setM(n,v);}

// ---- visual rhythm/choreography editor (lights + gauges) ----
let edIdx=-1,edType='light',edSteps=[],pTimer=null,pPos=0;
function edOpen(i){
 edIdx=i;edType=metersCache[i].type;
 if(edType==='light'){
  edSteps=(metersCache[i].steps||'200,150,200,600').split(',').map(Number).filter(v=>v>0);
 }else{
  edSteps=(metersCache[i].steps||'20:900,85:500,45:1200').split(',')
   .map(s=>s.split(':').map(Number)).filter(a=>a.length===2&&a[1]>0);
 }
 document.getElementById('edtitle').textContent=
  (edType==='light'?'Rhythm — ':'Choreography — ')+metersCache[i].name.toUpperCase();
 document.getElementById('plamp').hidden=edType!=='light';
 document.getElementById('pgauge').hidden=edType==='light';
 document.getElementById('edmodal').hidden=false;
 edDraw();pRestart();}
function edStr(){return edType==='light'?edSteps.join(','):edSteps.map(a=>a[0]+':'+a[1]).join(',');}
function edBar(){
 const b=document.getElementById('edbar');
 if(edType==='light'){
  const tot=edSteps.reduce((a,b2)=>a+b2,0);
  b.innerHTML=edSteps.map((v,j)=>'<div class="'+(j%2?'off':'on')+'" style="width:'+(v/tot*100)+'%"></div>').join('');
 }else{
  const tot=edSteps.reduce((a,b2)=>a+b2[1],0);
  b.innerHTML=edSteps.map(a=>'<div class="gs" style="width:'+(a[1]/tot*100)+'%;height:'+Math.max(a[0],5)+'%"></div>').join('');
 }}
function edDraw(){
 edBar();
 const rows=document.getElementById('edrows');
 if(edType==='light'){
  rows.innerHTML=edSteps.map((v,j)=>
   '<div class="erow"><span>'+(j%2?'OFF':'ON')+'</span>'+
   '<input type="range" min="20" max="2000" step="10" value="'+Math.min(v,2000)+'" oninput="edVal('+j+',this.value)">'+
   '<b id="ev'+j+'">'+v+'ms</b><button onclick="edDel('+j+')">&#10005;</button></div>').join('');
 }else{
  rows.innerHTML=edSteps.map((a,j)=>
   '<div class="erow"><span>'+(j+1)+'</span>'+
   '<input type="range" min="0" max="100" value="'+a[0]+'" oninput="edValG('+j+',0,this.value)">'+
   '<b id="ep'+j+'">'+a[0]+'%</b>'+
   '<input type="range" min="20" max="3000" step="10" value="'+Math.min(a[1],3000)+'" oninput="edValG('+j+',1,this.value)">'+
   '<b id="ed'+j+'">'+a[1]+'ms</b><button onclick="edDel('+j+')">&#10005;</button></div>').join('');
 }
 document.getElementById('edtext').value=edStr();}
function edVal(j,v){edSteps[j]=+v;document.getElementById('ev'+j).textContent=v+'ms';
 document.getElementById('edtext').value=edStr();edBar();pRestart();}
function edValG(j,k,v){edSteps[j][k]=+v;
 document.getElementById(k?'ed'+j:'ep'+j).textContent=v+(k?'ms':'%');
 document.getElementById('edtext').value=edStr();edBar();pRestart();}
function edDel(j){if(edSteps.length>1){edSteps.splice(j,1);edDraw();pRestart();}}
function edAdd(){if(edSteps.length<16){edSteps.push(edType==='light'?200:[50,500]);edDraw();pRestart();}}
function edFromText(){
 const t=document.getElementById('edtext').value;
 if(edType==='light'){
  const st=t.split(',').map(Number).filter(v=>v>=20&&v<=20000);
  if(st.length)edSteps=st.slice(0,16);
 }else{
  const st=t.split(',').map(s=>s.split(':').map(Number))
   .filter(a=>a.length===2&&a[0]>=0&&a[0]<=100&&a[1]>=20&&a[1]<=20000);
  if(st.length)edSteps=st.slice(0,16);
 }
 edDraw();pRestart();}
function pTick(){
 if(edType==='light'){
  document.getElementById('plamp').classList.toggle('lit',pPos%2===0);
  pTimer=setTimeout(()=>{pPos=(pPos+1)%edSteps.length;pTick()},edSteps[pPos]);
 }else{
  document.getElementById('pfill').style.width=edSteps[pPos][0]+'%';
  pTimer=setTimeout(()=>{pPos=(pPos+1)%edSteps.length;pTick()},edSteps[pPos][1]);
 }}
function pRestart(){clearTimeout(pTimer);pPos=0;pTick();}
function edClose(){clearTimeout(pTimer);document.getElementById('edmodal').hidden=true;edIdx=-1;}
async function edApply(){
 const n=edIdx+1,ep=edType==='light'?'/pattern':'/mpattern';
 try{await fetch(curBase+ep+'?meter='+n+'&steps='+encodeURIComponent(edStr()))}catch(e){}
 edClose();refresh();}
async function refresh(){try{
 const s=await (await fetch('/status')).json();
 curMode=s.mode;
 const pill=document.getElementById('pill');
 pill.textContent='BOARD '+s.board+' · '+s.mode;
 pill.style.background=COLORS[s.mode]||'#333';
 document.querySelectorAll('.grid button').forEach(b=>
  b.classList.toggle('on',b.dataset.m===s.mode));
 const ob=document.getElementById('offbtn');
 ob.classList.toggle('lit',s.mode==='off');
 ob.innerHTML=s.mode==='off'
  ?'&#128293; Rekindle<small>restore the laboratory</small>'
  :'&#9760; Extinguish All<small>kill every pin</small>';
 // The Instruments card follows the target dropdown: picking another board
 // shows THAT board's channels and sends changes to it.
 const t=document.getElementById('tgt').value;
 const tb=HERD.find(x=>x[0]==t);
 curBase=(t==='all'||+t===s.board||!tb)?'':PREFIX+tb[1];
 let ms=s.meters,mb=s.board;
 if(curBase){try{const ts=await (await fetch(curBase+'/status')).json();
  ms=ts.meters;mb=ts.board}catch(e){ms=[];mb=t+' (unreachable)'}}
 document.getElementById('instlabel').textContent='The Instruments — Board '+mb;
 renderMeters(ms,curBase);
 herdCheck();
 document.getElementById('jlabel').textContent='The Journal — Board '+mb;
 const log=document.getElementById('log');
 try{log.textContent=await (await fetch(curBase+'/log')).text();
 }catch(e){log.textContent='(board unreachable)'}
 log.scrollTop=log.scrollHeight;
}catch(e){const p=document.getElementById('pill');p.textContent='OFFLINE';p.style.background='#555'}}
loadHerd();setInterval(refresh,2000);
</script></body></html>)html";

static void handlePanel() {
    server.send_P(200, "text/html", PANEL_HTML);
}

static void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(WIFI_HOSTNAME);
#ifdef USE_STATIC_IP
    WiFi.config(IPAddress(STATIC_IP), IPAddress(STATIC_GATEWAY),
                IPAddress(STATIC_SUBNET), IPAddress(STATIC_GATEWAY));
#endif

    // List every 2.4 GHz network in range — the definitive word on what SSID
    // the ESP32 can actually see and how strong it is.
    int n = WiFi.scanNetworks();
    Serial.printf("visible 2.4GHz networks (%d):\n", n);
    for (int i = 0; i < n; i++) {
        Serial.printf("  \"%s\"  ch%d  %ddBm\n", WiFi.SSID(i).c_str(),
                      WiFi.channel(i), WiFi.RSSI(i));
    }
    WiFi.scanDelete();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("connecting to WiFi \"%s\"", WIFI_SSID);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(250);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println();
        logMsg("connected: http://" + WiFi.localIP().toString() +
               "/ (or http://" WIFI_HOSTNAME ".local/)");
        if (MDNS.begin(WIFI_HOSTNAME)) MDNS.addService("http", "tcp", 80);
    } else {
        Serial.println();
        logMsg("WiFi failed — flicker runs anyway; will keep retrying");
    }
}

void setup() {
    Serial.begin(115200);
    prefs.begin("franken");
    for (int i = 0; i < METER_COUNT; i++) {
        meters[i].begin(METERS[i], i);
        // Default rhythm: on-on-off. Overridden by anything saved in flash.
        customSteps[i][0] = 200; customSteps[i][1] = 150;
        customSteps[i][2] = 200; customSteps[i][3] = 600;
        customLen[i] = 4;
        uint8_t lp = prefs.getUChar((String("lp") + i).c_str(), (uint8_t)METERS[i].defPattern);
        lightPatterns[i] = lp < LP_COUNT ? (LightPattern)lp : METERS[i].defPattern;
        String saved = prefs.getString((String("cs") + i).c_str(), "");
        if (saved.length()) parseSteps(saved, i);
        chanNames[i] = prefs.getString((String("nm") + i).c_str(), METERS[i].name);
        // Default gauge choreography: low simmer, spike, settle.
        seqPos[i][0] = 20; seqDur[i][0] = 900;
        seqPos[i][1] = 85; seqDur[i][1] = 500;
        seqPos[i][2] = 45; seqDur[i][2] = 1200;
        seqLen[i] = 3;
        String sq = prefs.getString((String("mq") + i).c_str(), "");
        if (sq.length()) parseSeq(sq, i);
    }
    Serial.printf("Frankenstein Meters: flickering %d meter(s)\n", METER_COUNT);

    connectWiFi();
    // Lets the dashboard on one board read and set another board's channels.
    server.enableCORS(true);
    server.on("/", handlePanel);
    server.on("/set", handleSet);
    server.on("/pattern", handlePattern);
    server.on("/mpattern", handleMPattern);
    server.on("/rename", handleRename);
    server.on("/live", handleLive);
    server.on("/herd", handleHerd);
    server.on("/status", handleStatus);
    server.on("/log", handleLog);
    server.on("/freakout", handleFreakout);
    server.on("/coma", handleComa);
    server.on("/sweep", handleSweep);
    server.on("/calm", handleCalm);
    server.on("/off", handleOff);
#ifdef TRYME_PIN
    pinMode(TRYME_PIN, OUTPUT);
    digitalWrite(TRYME_PIN, LOW);
    // Manual test: /tryme pulses the prop trigger without a freakout.
    server.on("/tryme", []() {
        pulseTryme();
        server.send(200, "application/json", "{\"ok\":true}\n");
    });
#endif
    server.begin();
}

void loop() {
    server.handleClient();

#ifdef TRYME_PIN
    if (trymeOffAt && (int32_t)(millis() - trymeOffAt) >= 0) {
        digitalWrite(TRYME_PIN, LOW);
        trymeOffAt = 0;
    }
#endif

    // Reconnect WiFi if it drops (router reboot, etc.).
    static uint32_t lastWiFiCheck = 0;
    if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiCheck > 30000) {
        lastWiFiCheck = millis();
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }

    // Serial triggers for testing without the network.
    if (Serial.available()) {
        char ch = Serial.read();
        if (ch == 'f') { sweepMode = false; startFreakout(FREAKOUT_DEFAULT_S); }
        if (ch == 's') { sweepMode = true; freakoutUntil = 0; }
        if (ch == 'z') { comaMode = true; sweepMode = false; freakoutUntil = 0; }
        if (ch == 'c') { freakoutUntil = 0; sweepMode = false; comaMode = false; }
    }

    uint32_t now = millis();
    if (now - lastTick >= TICK_MS) {
        lastTick = now;
        if (allOff) {
            // Kill switch: everything eases to dark/zero.
            for (int i = 0; i < METER_COUNT; i++) {
                if (METERS[i].style == STYLE_LIGHT) meters[i].tickLight(LP_DARK, false, false, nullptr, 0);
                else meters[i].tickOff();
            }
        } else if (sweepMode) {
            // Slow 0-100-0% triangle, 8 s per cycle, for resistor calibration.
            float phase = (now % 8000) / 8000.0f;
            float frac = phase < 0.5f ? phase * 2.0f : 2.0f - phase * 2.0f;
            for (int i = 0; i < METER_COUNT; i++) {
                meters[i].writeDuty(frac);
            }
            static uint32_t lastPrint = 0;
            if (now - lastPrint >= 500) {
                lastPrint = now;
                Serial.printf("sweep: %3.0f%%\n", frac * 100.0f);
            }
        } else {
            bool boardFreak = freakingOut();
            for (int i = 0; i < METER_COUNT; i++) {
                if (METERS[i].style == STYLE_LIGHT) {
                    // Lights show their pattern; panic always strobes them.
                    meters[i].tickLight(lightPatterns[i], boardFreak, comaMode,
                                        customSteps[i], customLen[i]);
                    continue;
                }
                switch (overrides[i]) {
                    case OVR_FOLLOW:   meters[i].tick(boardFreak, comaMode); break;
                    case OVR_FLICKER:  meters[i].tick(false, false); break;
                    case OVR_FREAKOUT: meters[i].tick(true, false); break;
                    case OVR_COMA:     meters[i].tick(false, true); break;
                    case OVR_OFF:      meters[i].tickOff(); break;
                    case OVR_CUSTOM:   meters[i].tickSeq(seqPos[i], seqDur[i], seqLen[i], false); break;
                    case OVR_FUZZY:    meters[i].tickSeq(seqPos[i], seqDur[i], seqLen[i], true); break;
                    case OVR_HEARTBEAT: meters[i].tickBeat(); break;
                    case OVR_PEGGED:   meters[i].tickPegged(); break;
                    case OVR_SCAN:     meters[i].tickScan(); break;
                    case OVR_SPUTTER:  meters[i].tickSputter(); break;
                }
            }
        }
    }
}
