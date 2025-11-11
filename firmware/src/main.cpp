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
// Shared event scratch (telemetry.cpp reads this via `extern`).
// Increase a bit to avoid reallocation if fields grow.
StaticJsonDocument<160> latest_event;

// ─────────────────────────────────────────────────────────────────────────────
// Aliases from config
static constexpr float        FS           = CFG_FS_HZ;
static constexpr uint32_t     N            = CFG_N_SAMPLES;
static constexpr float        F0           = CFG_F0_HZ;
static constexpr const char*  WINDOW_NAME  = CFG_WINDOW_NAME;
static constexpr float        FRAME_PERIOD = CFG_FRAME_PERIOD_S * 1000.0f; // ms

// Device ON/OFF pattern targets
static constexpr float ON_DURATION_S  = CFG_ON_DURATION_S;
static constexpr float OFF_DURATION_S = CFG_OFF_DURATION_S;
static constexpr float I_RMS_OFF      = CFG_I_RMS_OFF_A;
static constexpr float I_RMS_ON       = CFG_I_RMS_ON_A;
static constexpr float PHASE_DEG      = CFG_PHASE_DEG;    // current lags voltage by +PHASE_DEG
static constexpr float H2_OFF         = CFG_H2_OFF;
static constexpr float H2_ON          = CFG_H2_ON;
static constexpr float V_RMS_TARGET   = CFG_V_RMS_TARGET;

// Optional placeholders / constants
static constexpr float THDV_PLACEHOLDER = CFG_THDV_PLACEHOLDER;  // mirrors your sim

// ─────────────────────────────────────────────────────────────────────────────
// Buffers
static std::vector<float> v(N), i(N), window(N), i_win(N);

// ─────────────────────────────────────────────────────────────────────────────
// State
static uint32_t frame_id            = 0;
static bool     state_on            = false;
static uint32_t state_switched_ms   = 0;
static float    prev_irms           = NAN;
static float    prev_p              = NAN;

// ─────────────────────────────────────────────────────────────────────────────
// Small helpers (internal linkage)
namespace {

inline uint32_t now_ms() {
  return millis();
}

// Rollover-safe elapsed computation
inline uint32_t elapsed_since(uint32_t t_start_ms) {
  return static_cast<uint32_t>(millis() - t_start_ms);
}

// Update ON/OFF state with rollover-safe timing
inline void update_on_off_state() {
  const uint32_t elapsed = elapsed_since(state_switched_ms);
  const uint32_t on_ms   = static_cast<uint32_t>(ON_DURATION_S  * 1000.0f);
  const uint32_t off_ms  = static_cast<uint32_t>(OFF_DURATION_S * 1000.0f);

  if (state_on) {
    if (elapsed >= on_ms) {
      state_on = false;
      state_switched_ms = now_ms();
    }
  } else {
    if (elapsed >= off_ms) {
      state_on = true;
      state_switched_ms = now_ms();
    }
  }
}

// Non-negative zeroing wrapper (keeps sign if nonzero, otherwise returns 0)
inline float zf(float x) { return static_cast<float>(utils::z(x)); }

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  // Some boards need a moment for USB CDC
  while (!Serial) { delay(10); }

  // Sanity checks (compile time where possible)
  static_assert(CFG_N_SAMPLES > 0, "CFG_N_SAMPLES must be > 0");
  static_assert(CFG_FS_HZ > 0,     "CFG_FS_HZ must be > 0");
  static_assert(CFG_F0_HZ > 0,     "CFG_F0_HZ must be > 0");

  // Window init (Hann by default; keep name for JSON)
  window.assign(N, 0.0f);
  dsp::hann(window);

  // Initial state
  state_on = false;
  state_switched_ms = now_ms();

  // Boot + Wi-Fi logs
  Serial.println(F("{\"boot\":\"ok\"}"));
  Serial.println(F("{\"wifi\":\"connecting\"}"));
  if (wifi_init()) {
    Serial.print(F("{\"wifi\":\"ok\",\"ip\":\""));
    Serial.print(WiFi.localIP());
    Serial.println(F("\"}"));
  } else {
    Serial.println(F("{\"wifi\":\"failed\",\"hint\":\"connect to AP HomePower-Setup\"}"));
  }
}

void loop() {
  const uint32_t t_loop_start = now_ms();

  // ── 1) Update ON/OFF state machine
  update_on_off_state();
  const float irms_target = state_on ? I_RMS_ON : I_RMS_OFF;
  const float h2_amp      = state_on ? H2_ON    : H2_OFF;
  const char* state_str   = state_on ? "on"     : "off";

  // ── 2) Acquire / Synthesize signals
#if TEST_MODE
  // Synthetic V/I like your sandbox (includes harmonic 2 content on current)
  signals::vi_test_signals(FS, N, V_RMS_TARGET, irms_target, F0, PHASE_DEG, h2_amp, v, i);
#else
  // TODO: Replace with dual-channel ADC fill (remove bias, scale to volts/amps)
  // adc::fill_dual(v.data(), i.data(), N);  // example API
  // dsp::remove_dc(v.data(), N);
  // dsp::remove_dc(i.data(), N);
#endif

  // ── 3) Pre-process (window current for harmonic analysis)
  for (uint32_t n = 0; n < N; ++n) {
    i_win[n] = i[n] * window[n];
  }

  // ── 4) Core metrics
  const float Vrms = dsp::rms(v.data(), N);
  const float Irms = dsp::rms(i.data(), N);
  const float P    = dsp::real_power(v.data(), i.data(), N);
  const float S    = dsp::apparent_power(Vrms, Irms);
  const float PF   = dsp::power_factor(P, S);

  // ── 5) Fundamental phasors / displacement PF
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

  // ── 6) Distortion (current-focused)
  const std::vector<float> harms = { 2.0f * F0, 3.0f * F0, 4.0f * F0, 5.0f * F0 };
  const float THD_i = dsp::thd_goertzel(i_win.data(), N, FS, F0, harms);
  const float THD_v = THDV_PLACEHOLDER; // if you later add voltage harmonics, swap this

  // Harmonic ratio map (string key = Hz, value = ratio wrt fundamental)
  auto h_i = dsp::harmonic_ratios_goertzel(i_win.data(), N, FS, F0, CFG_HARM_KMAX);

  // Odd/even sums by harmonic index k = round(f_hz / F0)
  double odd_sum = 0.0, even_sum = 0.0;
  for (const auto& kv : h_i) {
    const int f_hz = atoi(kv.first.c_str());
    const int k    = int(lroundf(float(f_hz) / F0));
    if (k >= 2) {
      if ((k % 2) == 0) even_sum += kv.second;
      else              odd_sum  += kv.second;
    }
  }

  const float crest_i = dsp::crest_factor(i.data(), N);
  const float form_i  = dsp::form_factor(i.data(), N);

  // ── 7) Deltas (frame-to-frame)
  if (isnan(prev_irms)) prev_irms = Irms;
  if (isnan(prev_p))    prev_p    = P;
  const float d_irms = Irms - prev_irms;
  const float d_p    = P    - prev_p;
  prev_irms = Irms;
  prev_p    = P;

  // ── 8) Event detection (simple step on Irms)
  latest_event.clear();
  if (fabsf(d_irms) > 1.0f) {
    latest_event["type"] = (d_irms > 0) ? "on" : "off";
    latest_event["t_ms"] = static_cast<uint64_t>(now_ms());
  }

  // ── 9) Build FrameCore & JSON
  FrameCore core{
    Vrms, Irms, P, S, PF,
    v1_rms, i1_rms, phi_deg, P1, Q1, S1, PF_disp,
    THD_i, THD_v, zf(static_cast<float>(odd_sum)), zf(static_cast<float>(even_sum)), crest_i, form_i
  };

  const uint64_t t_ms = utils::now_ms(); // keep your existing util (e.g., epoch-based)
  String json = pack_frame_json(
    t_ms, frame_id, FS, N, WINDOW_NAME, F0, core, h_i, state_str,
    zf(d_irms), zf(d_p),
    /* include_core = */ true,
    /* include_harm = */ true,
    /* include_event= */ true,
    CFG_FW_TAG,
    CFG_CAL_ID
  );

  // ── 10) Output logs
  Serial.println(json);

  Serial.printf(
    "t=%llu ms  state=%s  Vrms=%.1f  Irms=%.3f  P=%.1f W  PF=%.3f  THD_i=%.3f  phi=%.1f°%s",
    static_cast<unsigned long long>(t_ms),
    state_str,
    Vrms, Irms, P, PF, THD_i, phi_deg,
    (latest_event.isNull() ? "\n" : "  <-- EVENT\n")
  );

  // ── 11) Optional: Push to Firebase
#if CFG_PUSH_ENABLE
  if ((frame_id % CFG_PUSH_EVERY_N) == 0) {
    const bool ok = fb_push_frame(CFG_DEVICE_ID, CFG_FW_TAG, frame_id, json);
    if (!ok) {
      Serial.println(F("{\"push\":\"err\"}"));
    }
  }
#endif

  // ── 12) Frame pacing
  ++frame_id;
  const uint32_t elapsed_ms = static_cast<uint32_t>(millis() - t_loop_start);
  const uint32_t period_ms  = static_cast<uint32_t>(FRAME_PERIOD);
  if (elapsed_ms < period_ms) {
    const uint32_t wait_ms = period_ms - elapsed_ms;
    delay(wait_ms);
  }
}
