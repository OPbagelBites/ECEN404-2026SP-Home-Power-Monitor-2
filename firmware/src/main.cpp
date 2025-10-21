#include <Arduino.h>
#include <vector>
#include <map>
#include "config.h"
#include "signals.h"
#include "dsp.h"
#include "telemetry.h"
#include "utils.h"

// ---------- Aliases from config ----------
static constexpr float     FS            = CFG_FS_HZ;
static constexpr uint32_t  N             = CFG_N_SAMPLES;
static constexpr float     F0            = CFG_F0_HZ;
static constexpr const char* WINDOW_NAME = CFG_WINDOW_NAME;
static constexpr float     FRAME_PERIOD  = CFG_FRAME_PERIOD_S * 1000.0f; // ms

// Device pattern (ON↔OFF)
static constexpr float ON_DURATION_S   = CFG_ON_DURATION_S;
static constexpr float OFF_DURATION_S  = CFG_OFF_DURATION_S;
static constexpr float I_RMS_OFF       = CFG_I_RMS_OFF_A;
static constexpr float I_RMS_ON        = CFG_I_RMS_ON_A;
static constexpr float PHASE_DEG       = CFG_PHASE_DEG;    // current lags voltage by +PHASE_DEG
static constexpr float H2_OFF          = CFG_H2_OFF;
static constexpr float H2_ON           = CFG_H2_ON;
static constexpr float V_RMS_TARGET    = CFG_V_RMS_TARGET;

// ---------- Buffers ----------
static std::vector<float> v(N), i(N), window(N), i_win(N);

// ---------- State ----------
static uint32_t frame_id = 0;
static bool state_on = false;
static uint32_t state_switched_ms = 0;
static float prev_irms = NAN;
static float prev_p    = NAN;

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  window.assign(N, 0.0f);
  dsp::hann(window);

  state_on = false;
  state_switched_ms = millis();
  Serial.println(F("{\"boot\":\"ok\"}"));
}

void loop() {
  const uint32_t t0 = millis();

  // Toggle ON/OFF by durations
  uint32_t elapsed = millis() - state_switched_ms;
  if (state_on && elapsed >= (uint32_t)(ON_DURATION_S * 1000.0f)) {
    state_on = false; state_switched_ms = millis();
  } else if (!state_on && elapsed >= (uint32_t)(OFF_DURATION_S * 1000.0f)) {
    state_on = true; state_switched_ms = millis();
  }

  // Targets for this state
  const float irms_target = state_on ? I_RMS_ON : I_RMS_OFF;
  const float h2_amp      = state_on ? H2_ON     : H2_OFF;
  const char* state_str   = state_on ? "on"      : "off";

#if TEST_MODE
  // Generate synthetic V/I like your sandbox
  signals::vi_test_signals(FS, N, V_RMS_TARGET, irms_target, F0, -PHASE_DEG /*- for lag*/, h2_amp, v, i);
#else
  // TODO: Replace with dual-channel ADC fill for v[] and i[] (with bias removal & scaling)
#endif

  // Windowed current for harmonic analysis
  for (size_t n = 0; n < N; ++n) i_win[n] = i[n] * window[n];

  // ---- Core metrics ----
  const float Vrms = dsp::rms(v.data(), N);
  const float Irms = dsp::rms(i.data(), N);
  const float P    = dsp::real_power(v.data(), i.data(), N);
  const float S    = dsp::apparent_power(Vrms, Irms);
  const float PF   = dsp::power_factor(P, S);

  // ---- Fundamental phasors (displacement PF) ----
  float v1_rms=0, v1_phase=0, i1_rms=0, i1_phase=0;
  dsp::dft_phasor(v.data(), N, FS, F0, v1_rms, v1_phase);
  dsp::dft_phasor(i.data(), N, FS, F0, i1_rms, i1_phase);
  const float phi = v1_phase - i1_phase;
  const float phi_deg = phi * (180.0f / float(M_PI));
  const float S1 = v1_rms * i1_rms;
  const float P1 = S1 * cosf(phi);
  const float Q1 = S1 * sinf(phi);
  const float PF_disp = cosf(phi);

  // ---- Distortion (current-focused) ----
  const std::vector<float> harms = { 2.0f*F0, 3.0f*F0, 4.0f*F0, 5.0f*F0 };
  const float THD_i = dsp::thd_goertzel(i_win.data(), N, FS, F0, harms);
  const float THD_v = CFG_THDV_PLACEHOLDER; // like your sim

  auto h_i = dsp::harmonic_ratios_goertzel(i_win.data(), N, FS, F0, CFG_HARM_KMAX);

  // odd/even sums (k=2 => even, k=3 => odd, etc.)
  double odd_sum = 0.0, even_sum = 0.0;
  for (const auto& kv : h_i) {
    int f_hz = atoi(kv.first.c_str());
    int k = int(lroundf(float(f_hz) / F0));
    if (k % 2 == 0) even_sum += kv.second; else odd_sum += kv.second;
  }

  const float crest_i = dsp::crest_factor(i.data(), N);
  const float form_i  = dsp::form_factor(i.data(), N);

  // ---- Deltas (for events downstream) ----
  if (isnan(prev_irms)) prev_irms = Irms;
  if (isnan(prev_p))    prev_p    = P;
  float d_irms = Irms - prev_irms;
  float d_p    = P    - prev_p;
  prev_irms = Irms;
  prev_p    = P;

  // ---- Build FrameCore + JSON ----
  FrameCore core{
    Vrms, Irms, P, S, PF,
    v1_rms, i1_rms, phi_deg, P1, Q1, S1, PF_disp,
    THD_i, THD_v, float(utils::z(float(odd_sum))), float(utils::z(float(even_sum))), crest_i, form_i
  };

  const uint64_t t_ms = utils::now_ms();
  String json = pack_frame_json(
    t_ms, frame_id, FS, N, WINDOW_NAME, F0, core, h_i, state_str,
    utils::z(d_irms), utils::z(d_p),
    true, true, true,
    CFG_FW_TAG, CFG_CAL_ID
  );
  Serial.println(json);

  frame_id++;

  // pacing
  const uint32_t elapsed_ms = millis() - t0;
  const uint32_t wait_ms = (elapsed_ms >= (uint32_t)FRAME_PERIOD) ? 0 : (uint32_t)FRAME_PERIOD - elapsed_ms;
  delay(wait_ms);
}
