/**
 * main.cpp
 *
 * Top-level firmware for the Home Power Monitor ESP32 node.
 *
 * This file is the "conductor" of the system:
 *  - Manages a simple ON/OFF state machine (simulated load pattern or TEST_MODE appliances).
 *  - Acquires (or synthesizes) one frame of voltage/current samples.
 *  - Runs DSP on that frame (RMS, real/apparent power, phasors, harmonics, THD, shape factors).
 *  - Detects events (ON/OFF steps on Irms).
 *  - Packs everything into a JSON "telemetry frame".
 *  - Streams frames over Serial, and optionally pushes them to Firebase.
 *
 * The actual math and helpers live in other modules:
 *  - dsp.*: FFT/DFT, RMS, power, THD, crest/form factors, windowing.
 *  - signals.*: synthetic test waveforms for TEST_MODE.
 *  - telemetry.*: FrameCore struct and pack_frame_json() for JSON formatting.
 *  - firebase.*: fb_push_frame() to send JSON to Firebase.
 *  - utils.*: time helpers, numeric helpers, etc.
 *
 * In the senior-design context:
 *  - This is where we integrate the AFE/ADC side (real CT/voltage input) into the DSP/ML pipeline.
 *  - The per-frame JSON is the contract between embedded firmware, backend, and ML model.
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include <WiFi.h>

#include "config.h"
#include "signals.h"
#include "dsp.h"
#include "telemetry.h"
#include "utils.h"
#include "firebase.h"

// ─────────────────────────────────────────────────────────────────────────────
// Shared event scratch
// ─────────────────────────────────────────────────────────────────────────────
/**
 * latest_event
 *
 *  - This JSON document is used as a "scratch pad" to describe the most recent
 *    event we detected (e.g., load turned ON/OFF).
 *  - It is declared here and exposed in telemetry.cpp via `extern`.
 *  - We keep it small and static to avoid heap allocation on each frame.
 *
 * Example content:
 *    { "type": "on", "t_ms": 123456789 }
 */
StaticJsonDocument<160> latest_event;

// ─────────────────────────────────────────────────────────────────────────────
// Configuration aliases
// ─────────────────────────────────────────────────────────────────────────────
/**
 * These constants mirror values from config.h.
 * They are hoisted into file-scope constexprs for readability and to avoid
 * sprinkling CFG_* everywhere in the logic.
 */
static constexpr float        FS           = CFG_FS_HZ;              // ADC sampling rate [Hz]
static constexpr uint32_t     N            = CFG_N_SAMPLES;          // samples per frame
static constexpr float        F0           = CFG_F0_HZ;              // mains fundamental [Hz]
static constexpr const char*  WINDOW_NAME  = CFG_WINDOW_NAME;        // e.g., "hann"
static constexpr float        FRAME_PERIOD = CFG_FRAME_PERIOD_S * 1000.0f; // frame period [ms]

// ON/OFF state pattern (used in both real-world and TEST_MODE)
static constexpr float ON_DURATION_S  = CFG_ON_DURATION_S;   // seconds to stay ON
static constexpr float OFF_DURATION_S = CFG_OFF_DURATION_S;  // seconds to stay OFF

// Base "generic device" characteristics (overridden by TEST_MODE appliance profiles)
static constexpr float I_RMS_OFF      = CFG_I_RMS_OFF_A;     // baseline idle current [A]
static constexpr float I_RMS_ON       = CFG_I_RMS_ON_A;      // generic ON current [A]
static constexpr float PHASE_DEG      = CFG_PHASE_DEG;       // current phase shift vs voltage [deg]
static constexpr float H2_OFF         = CFG_H2_OFF;          // 2nd harmonic level when OFF
static constexpr float H2_ON          = CFG_H2_ON;           // 2nd harmonic level when ON
static constexpr float V_RMS_TARGET   = CFG_V_RMS_TARGET;    // line voltage target [Vrms]

// Voltage THD placeholder (mirrors the Python sandbox until we compute v-harmonics onboard)
static constexpr float THDV_PLACEHOLDER = CFG_THDV_PLACEHOLDER;

// ─────────────────────────────────────────────────────────────────────────────
// Synthetic appliance profiles for TEST_MODE
// ─────────────────────────────────────────────────────────────────────────────
/**
 * When TEST_MODE is enabled, we don't read a real ADC.
 * Instead, we synthesize signals that look like different household appliances.
 *
 * Each profile encodes:
 *  - I_RMS_ON: how much current the appliance draws.
 *  - H2_ON:    second harmonic content of the current (distortion proxy).
 *  - PHASE_DEG:phase lag of current relative to voltage.
 *
 * This lets us:
 *  - Exercise the full DSP + telemetry pipeline without hardware.
 *  - Generate labeled training data for the ML classifier ("ground truth").
 */
#if TEST_MODE
struct ApplianceProfile {
  const char* name;   // Human-readable label (goes into "labels" array in JSON for training)
  float I_RMS_ON;     // Active current draw when ON [A]
  float H2_ON;        // 2nd harmonic ratio wrt fundamental
  float PHASE_DEG;    // phase lag [deg]
};

// Hard-coded library of common household loads with rough/approx characteristics
static const ApplianceProfile PROFILES[] = {
  // Big motor/compressor, high-ish current, moderate distortion
  {"Air Conditioner",          10.0f, 0.635f, 50.6f},

  // Small lighting loads
  {"Compact Fluorescent Lamp",  0.30f, 0.082f, 26.4f},
  {"Incandescent Light Bulb",   0.60f, 0.096f, 22.3f},

  // Small–medium motor loads
  {"Fan",                       1.00f, 0.121f, 20.0f},
  {"Fridge",                    3.00f, 0.800f, 44.7f},

  // High-power resistive / mixed
  {"Hairdryer",                12.00f, 0.119f, 16.7f},  // ~1400 W
  {"Heater",                   12.00f, 0.130f, 15.4f},  // ~1400 W

  // Electronics / SMPS
  {"Laptop",                    0.50f, 0.033f, 25.8f},

  // High-power mixed (magnetron etc.)
  {"Microwave",                 9.00f, 0.324f, 37.1f},  // ~1000 W

  // Other large motor-ish loads
  {"Vacuum",                   10.00f, 0.143f, 35.6f},  // ~1200 W
  {"Washing Machine",           8.00f, 0.800f, 87.3f},  // peaks + motor
};

static constexpr int NUM_PROFILES = sizeof(PROFILES) / sizeof(PROFILES[0]);

// Index of the current simulated appliance while in TEST_MODE
static int profile_idx = 0;
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Sample buffers
// ─────────────────────────────────────────────────────────────────────────────
/**
 * v     : per-frame voltage samples.
 * i     : per-frame current samples.
 * window: precomputed window coefficients (e.g., Hann).
 * i_win : windowed current (we keep it separate so i[] remains "raw" for RMS).
 */
static std::vector<float> v(N), i(N), window(N), i_win(N);

// ─────────────────────────────────────────────────────────────────────────────
// State variables (persist across loop() calls)
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t frame_id          = 0;     // monotonically increasing frame counter
static bool     state_on          = false; // "device" state: ON or OFF
static uint32_t state_switched_ms = 0;     // when we last toggled state_on (millis)
static float    prev_irms         = NAN;   // previous frame's Irms (for delta)
static float    prev_p            = NAN;   // previous frame's P (for delta)

// ─────────────────────────────────────────────────────────────────────────────
// Small helpers (internal linkage only)
// ─────────────────────────────────────────────────────────────────────────────
namespace {

/**
 * now_ms()
 * Convenience wrapper for millis(). We keep it as a function so that if we
 * ever change time base logic, it is centralized.
 */
inline uint32_t now_ms() {
  return millis();
}

/**
 * elapsed_since(t_start_ms)
 * Computes elapsed time in milliseconds since a given timestamp using
 * unsigned arithmetic so it is safe even when millis() rolls over.
 */
inline uint32_t elapsed_since(uint32_t t_start_ms) {
  return static_cast<uint32_t>(millis() - t_start_ms);
}

/**
 * update_on_off_state()
 *
 * Implements a simple two-state machine:
 *  - The system sits in ON or OFF for configurable durations.
 *  - After ON_DURATION_S it switches to OFF; after OFF_DURATION_S it switches back to ON.
 *  - Uses elapsed_since() so it remains correct over millis() rollover.
 *
 * This state drives:
 *  - Which synthetic appliance profile we use (in TEST_MODE).
 *  - What Irms target we pass to the signal generator.
 */
inline void update_on_off_state() {
  const uint32_t elapsed = elapsed_since(state_switched_ms);
  const uint32_t on_ms   = static_cast<uint32_t>(ON_DURATION_S  * 1000.0f);
  const uint32_t off_ms  = static_cast<uint32_t>(OFF_DURATION_S * 1000.0f);

  if (state_on) {
    // Device has been ON; switch OFF after on_ms
    if (elapsed >= on_ms) {
      state_on = false;
      state_switched_ms = now_ms();
    }
  } else {
    // Device has been OFF; switch ON after off_ms
    if (elapsed >= off_ms) {
      state_on = true;
      state_switched_ms = now_ms();
    }
  }
}

/**
 * zf(x) - "zero-fixed" wrapper
 *
 * We sometimes want to force extremely small numbers to exactly zero
 * (e.g., to avoid -0.000 in the JSON). utils::z() does that logic; this
 * wrapper just casts back to float.
 *
 * Behavior:
 *  - If |x| is below a threshold, return 0 (exact).
 *  - Otherwise, return x unchanged.
 */
inline float zf(float x) { return static_cast<float>(utils::z(x)); }

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Arduino setup()
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Some boards need a moment for USB CDC to enumerate
  while (!Serial) {
    delay(10);
  }

  // Compile-time sanity checks on core config
  static_assert(CFG_N_SAMPLES > 0, "CFG_N_SAMPLES must be > 0");
  static_assert(CFG_FS_HZ     > 0, "CFG_FS_HZ must be > 0");
  static_assert(CFG_F0_HZ     > 0, "CFG_F0_HZ must be > 0");

  // Initialize window coefficients (Hann by default).
  // We reuse the same window each frame to avoid recomputing.
  window.assign(N, 0.0f);
  dsp::hann(window);

  // Initialize ON/OFF state machine
  state_on = false;          // start in OFF state
  state_switched_ms = now_ms();

  // Boot + Wi-Fi logs (JSON-style so they'll parse on the host side)
  Serial.println(F("{\"boot\":\"ok\"}"));
  Serial.println(F("{\"wifi\":\"connecting\"}"));

  // Try to join configured Wi-Fi, or fall back to AP mode
  if (wifi_init()) {
    Serial.print(F("{\"wifi\":\"ok\",\"ip\":\""));
    Serial.print(WiFi.localIP());
    Serial.println(F("\"}"));
  } else {
    // If wifi_init() fails, we expect the device to be hosting an AP such as "HomePower-Setup"
    Serial.println(F("{\"wifi\":\"failed\",\"hint\":\"connect to AP HomePower-Setup\"}"));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Arduino loop()
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  const uint32_t t_loop_start = now_ms();  // used later for frame pacing

  // ── 1) Update ON/OFF state machine
  update_on_off_state();
  const char* state_str = state_on ? "on" : "off";

  // Base targets for the current frame. In TEST_MODE we may override these
  // with appliance-specific values.
  float irms_target = state_on ? I_RMS_ON : I_RMS_OFF;
  float h2_amp      = state_on ? H2_ON    : H2_OFF;
  float phase_deg   = PHASE_DEG;

#if TEST_MODE
  // In TEST_MODE, when the "device" is ON we simulate a specific appliance.
  const ApplianceProfile& prof = PROFILES[profile_idx];
  if (state_on) {
    // Use the active profile's current, distortion, and phase
    irms_target = prof.I_RMS_ON;
    h2_amp      = prof.H2_ON;
    phase_deg   = prof.PHASE_DEG;
  } else {
    // When OFF, fall back to the generic idle behavior
    irms_target = I_RMS_OFF;
    h2_amp      = H2_OFF;
    phase_deg   = PHASE_DEG;
  }
#endif

  // ── 2) Acquire / synthesize one frame of V/I samples
#if TEST_MODE
  /**
   * TEST_MODE path:
   *  - We synthesize sinusoidal voltage and current with:
   *      • Specified Vrms and Irms.
   *      • Fundamental frequency F0.
   *      • Current phase offset vs voltage (phase_deg).
   *      • Injected 2nd harmonic amplitude (h2_amp) in the current.
   *
   * This mimics real breaker-panel waveforms without requiring hardware.
   */
  signals::vi_test_signals(FS, N, V_RMS_TARGET, irms_target, F0, phase_deg, h2_amp, v, i);
#else
  /**
   * Production path:
   *  - Replace this with real ADC acquisition from the AFE.
   *  - Typical flow:
   *      1) adc::fill_dual(v.data(), i.data(), N);  // grab N samples for both channels.
   *      2) dsp::remove_dc(v.data(), N);            // remove DC bias from V.
   *      3) dsp::remove_dc(i.data(), N);            // remove DC bias from I.
   *
   * This is where the hardware team and firmware meet.
   */
  // adc::fill_dual(v.data(), i.data(), N);
  // dsp::remove_dc(v.data(), N);
  // dsp::remove_dc(i.data(), N);
#endif

  // ── 3) Pre-processing for harmonic analysis (window the current)
  /**
   * We window the current before harmonic analysis to reduce spectral leakage.
   *  - i[] stays un-windowed and is used for RMS and power.
   *  - i_win[] is the windowed version used for Goertzel-based THD/harmonics.
   */
  for (uint32_t n = 0; n < N; ++n) {
    i_win[n] = i[n] * window[n];
  }

  // ── 4) Core scalar metrics (RMS, power, PF)
  const float Vrms = dsp::rms(v.data(), N);             // line voltage magnitude
  const float Irms = dsp::rms(i.data(), N);             // current magnitude
  const float P    = dsp::real_power(v.data(), i.data(), N); // true/active power [W]
  const float S    = dsp::apparent_power(Vrms, Irms);   // apparent power [VA]
  const float PF   = dsp::power_factor(P, S);           // overall power factor (P/S)

  // ── 5) Fundamental phasors and displacement PF
  /**
   * We compute the fundamental components (at F0) of V and I via a single-tone DFT.
   * This gives us:
   *  - v1_rms, i1_rms: RMS of the fundamental only.
   *  - v1_phase, i1_phase: phase angles of those fundamentals.
   *
   * From these we derive:
   *  - phi_deg: phase difference V - I in degrees.
   *  - S1: apparent power of fundamental only.
   *  - P1, Q1: fundamental active and reactive power.
   *  - PF_disp: displacement power factor (cos of phase angle), ignoring distortion.
   */
  float v1_rms = 0.0f, v1_phase = 0.0f;
  float i1_rms = 0.0f, i1_phase = 0.0f;
  dsp::dft_phasor(v.data(), N, FS, F0, v1_rms, v1_phase);
  dsp::dft_phasor(i.data(), N, FS, F0, i1_rms, i1_phase);

  const float phi     = v1_phase - i1_phase;
  const float phi_deg = phi * (180.0f / float(M_PI));
  const float S1      = v1_rms * i1_rms;
  const float P1      = S1 * cosf(phi);
  const float Q1      = S1 * sinf(phi);
  const float PF_disp = cosf(phi);

  // ── 6) Harmonic distortion features (current-focused)
  /**
   * THD_i:
   *  - Total Harmonic Distortion of the current, computed via Goertzel at
   *    a set of harmonic frequencies (2*F0, 3*F0, 4*F0, 5*F0).
   *
   * THD_v:
   *  - Currently a placeholder from config. If we later compute voltage
   *    harmonics onboard, this will be replaced with the real value.
   */
  const std::vector<float> harms = { 2.0f * F0, 3.0f * F0, 4.0f * F0, 5.0f * F0 };
  const float THD_i = dsp::thd_goertzel(i_win.data(), N, FS, F0, harms);
  const float THD_v = THDV_PLACEHOLDER;

  /**
   * harmonic_ratios_goertzel():
   *  - Returns a map<string,float> keyed by harmonic frequency in Hz
   *    (e.g., "120") with values as ratio vs fundamental.
   *  - We use this map both for JSON output and for derived metrics like
   *    odd vs even harmonic energy.
   */
  auto h_i = dsp::harmonic_ratios_goertzel(i_win.data(), N, FS, F0, CFG_HARM_KMAX);

  // Aggregate odd vs even harmonic content (based on harmonic index k ≈ f / F0)
  double odd_sum = 0.0, even_sum = 0.0;
  for (const auto& kv : h_i) {
    const int f_hz = atoi(kv.first.c_str());
    const int k    = int(lroundf(float(f_hz) / F0)); // harmonic index (2 → 2nd, 3 → 3rd, ...)
    if (k >= 2) {
      if ((k % 2) == 0) {
        even_sum += kv.second;
      } else {
        odd_sum  += kv.second;
      }
    }
  }

  // Shape factors: crest factor and form factor for the current waveform
  const float crest_i = dsp::crest_factor(i.data(), N);
  const float form_i  = dsp::form_factor(i.data(), N);

  // ── 7) Frame-to-frame deltas (dIrms and dP)
  /**
   * These deltas let us detect step changes in load behavior:
   *  - d_irms > 0 → current increased; likely a load turned ON.
   *  - d_irms < 0 → current decreased; likely a load turned OFF.
   *
   * We initialize prev_* on the first frame to avoid NaN propagation.
   */
  if (isnan(prev_irms)) prev_irms = Irms;
  if (isnan(prev_p))    prev_p    = P;
  const float d_irms = Irms - prev_irms;
  const float d_p    = P    - prev_p;
  prev_irms = Irms;
  prev_p    = P;

  // ── 8) Event detection logic
  /**
   * Simple event rule:
   *  - If |dIrms| > 1 A between frames, we declare an event.
   *  - "type" field is "on" if current increases, "off" if it decreases.
   *
   * This is intentionally simple and explainable. The ML layer can
   * later combine this with the full feature vector to classify loads.
   */
  latest_event.clear();
  if (fabsf(d_irms) > 1.0f) {
    latest_event["type"] = (d_irms > 0) ? "on" : "off";
    latest_event["t_ms"] = static_cast<uint64_t>(now_ms());  // relative timestamp in ms
  }

#if TEST_MODE
  /**
   * Appliance rotation (TEST_MODE only)
   *
   * We want each ON-cycle to correspond to a different appliance profile.
   * To do that, we:
   *  - Track the previous state_on.
   *  - When we detect an ON → OFF transition, we advance profile_idx.
   *  - This rotation is independent of the dIrms event threshold, so we
   *    always move through the profile list each ON/OFF cycle.
   */
  static bool prev_state_on_for_cycle = false;

  if (prev_state_on_for_cycle && !state_on) {
    // We just transitioned from ON -> OFF: move to the next appliance
    profile_idx = (profile_idx + 1) % NUM_PROFILES;
  }

  prev_state_on_for_cycle = state_on;
#endif

  // ── 9) Pack everything into FrameCore and then JSON
  /**
   * FrameCore:
   *  - Compact struct that holds all the scalar features for this frame.
   *  - The struct definition lives in telemetry.h / telemetry.cpp.
   *  - We pass it to pack_frame_json(), which turns it into a JSON object.
   */
  FrameCore core{
    Vrms, Irms, P, S, PF,
    v1_rms, i1_rms, phi_deg, P1, Q1, S1, PF_disp,
    THD_i, THD_v, zf(static_cast<float>(odd_sum)), zf(static_cast<float>(even_sum)),
    crest_i, form_i
  };

  // Use utils::now_ms() rather than millis() so this can be epoch-based if desired.
  const uint64_t t_ms = utils::now_ms();

  // Construct the JSON string for the telemetry frame.
  // Flags control whether we include core metrics, harmonic map, and event block.
  String json = pack_frame_json(
    t_ms,
    frame_id,
    FS,
    N,
    WINDOW_NAME,
    F0,
    core,
    h_i,
    state_str,
    zf(d_irms),
    zf(d_p),
    /* include_core  = */ true,
    /* include_harm  = */ true,
    /* include_event = */ true,
    CFG_FW_TAG,
    CFG_CAL_ID
  );

#if TEST_MODE
  /**
   * In TEST_MODE we embed a "labels" array in the JSON:
   *  - This gives ground-truth appliance names to whatever is ON.
   *  - The ML training pipeline consumes these labels for supervised learning.
   *
   * Implementation detail:
   *  - We parse the JSON we just built, inject "labels", and serialize again.
   *  - This is cheap enough for our frame rate and simplifies pack_frame_json().
   */
  StaticJsonDocument<256> doc;
  auto err = deserializeJson(doc, json);
  if (!err) {
    JsonArray labels = doc.createNestedArray("labels");
    if (state_on) {
      labels.add(PROFILES[profile_idx].name);
    }
    json = "";
    serializeJson(doc, json);
  }
#endif

  // ── 10) Output logs to Serial
  /**
   * We both:
   *  - Print the JSON for machine consumption (backend, logger, etc.).
   *  - Print a human-readable line with key metrics for quick debugging.
   */
  Serial.println(json);

  Serial.printf(
    "t=%llu ms  state=%s  Vrms=%.1f  Irms=%.3f  P=%.1f W  PF=%.3f  THD_i=%.3f  phi=%.1f°%s",
    (unsigned long long)t_ms,
    state_str,
    Vrms,
    Irms,
    P,
    PF,
    THD_i,
    phi_deg,
    (latest_event.isNull() ? "\n" : "  <-- EVENT\n")
  );

  // ── 11) Optional: Push to Firebase (every CFG_PUSH_EVERY_N frames)
#if CFG_PUSH_ENABLE
  /**
   * Firebase push policy:
   *  - To limit bandwidth and DB writes, we only push every Nth frame.
   *  - The host can still see full-rate frames over Serial.
   */
  if ((frame_id % CFG_PUSH_EVERY_N) == 0) {
    const bool ok = fb_push_frame(CFG_DEVICE_ID, CFG_FW_TAG, frame_id, json);
    if (!ok) {
      Serial.println(F("{\"push\":\"err\"}"));
    }
  }
#endif

  // ── 12) Frame pacing (enforce roughly constant FRAME_PERIOD)
  ++frame_id;

  const uint32_t elapsed_ms = static_cast<uint32_t>(millis() - t_loop_start);
  const uint32_t period_ms  = static_cast<uint32_t>(FRAME_PERIOD);

  if (elapsed_ms < period_ms) {
    const uint32_t wait_ms = period_ms - elapsed_ms;
    delay(wait_ms);  // this sets the frame rate / update period
  }
}
