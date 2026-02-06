// main.cpp (dual-channel VP/VN, minimal + stable)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <vector>
#include <map>
#include <math.h>

#include "config.h"
#include "signals.h"
#include "dsp.h"
#include "telemetry.h"
#include "utils.h"
#include "firebase.h"

StaticJsonDocument<160> latest_event;

// Config aliases
static constexpr float        FS_CFG          = CFG_FS_HZ;
static constexpr uint32_t     N               = CFG_N_SAMPLES;
static constexpr float        F0              = CFG_F0_HZ;
static constexpr const char*  WINDOW_NAME     = CFG_WINDOW_NAME;
static constexpr float        FRAME_PERIOD_MS = CFG_FRAME_PERIOD_S * 1000.0f;

static constexpr float ON_DURATION_S   = CFG_ON_DURATION_S;
static constexpr float OFF_DURATION_S  = CFG_OFF_DURATION_S;

static constexpr float I_RMS_OFF       = CFG_I_RMS_OFF_A;
static constexpr float I_RMS_ON        = CFG_I_RMS_ON_A;
static constexpr float PHASE_DEG       = CFG_PHASE_DEG;
static constexpr float H2_OFF          = CFG_H2_OFF;
static constexpr float H2_ON           = CFG_H2_ON;
static constexpr float V_RMS_TARGET    = CFG_V_RMS_TARGET;

static constexpr float THDV_PLACEHOLDER = CFG_THDV_PLACEHOLDER;

// ESP32 ADC pins
static constexpr int V_PIN = 36; // VP = GPIO36 = ADC1_CH0
static constexpr int I_PIN = 39; // VN = GPIO39 = ADC1_CH3

#if TEST_MODE
struct ApplianceProfile { const char* name; float I_RMS_ON; float H2_ON; float PHASE_DEG; };

static const ApplianceProfile PROFILES[] = {
  {"Air Conditioner",          10.0f, 0.635f, 50.6f},
  {"Compact Fluorescent Lamp",  0.30f, 0.082f, 26.4f},
  {"Incandescent Light Bulb",   0.60f, 0.096f, 22.3f},
  {"Fan",                       1.00f, 0.121f, 20.0f},
  {"Fridge",                    3.00f, 0.800f, 44.7f},
  {"Hairdryer",                12.00f, 0.119f, 16.7f},
  {"Heater",                   12.00f, 0.130f, 15.4f},
  {"Laptop",                    0.50f, 0.033f, 25.8f},
  {"Microwave",                 9.00f, 0.324f, 37.1f},
  {"Vacuum",                   10.00f, 0.143f, 35.6f},
  {"Washing Machine",           8.00f, 0.800f, 87.3f},
};
static constexpr int NUM_PROFILES = sizeof(PROFILES) / sizeof(PROFILES[0]);
static int profile_idx = 0;
#endif

// Buffers
static std::vector<float> v(N), i(N), window(N), i_win(N);

// State
static uint32_t frame_id          = 0;
static bool     state_on          = false;
static uint32_t state_switched_ms = 0;
static float    prev_irms         = NAN;
static float    prev_p            = NAN;

namespace {
inline uint32_t now_ms() { return millis(); }
inline uint32_t elapsed_since(uint32_t t0) { return (uint32_t)(millis() - t0); }
inline float zf(float x) { return (float)utils::z(x); }

inline void update_on_off_state() {
  const uint32_t elapsed = elapsed_since(state_switched_ms);
  const uint32_t on_ms  = (uint32_t)lroundf(ON_DURATION_S  * 1000.0f);
  const uint32_t off_ms = (uint32_t)lroundf(OFF_DURATION_S * 1000.0f);

  if (state_on) {
    if (elapsed >= on_ms) { state_on = false; state_switched_ms = now_ms(); }
  } else {
    if (elapsed >= off_ms) { state_on = true; state_switched_ms = now_ms(); }
  }
}

inline void dc_remove(std::vector<float>& x) {
  double m = 0.0;
  for (uint32_t n = 0; n < N; ++n) m += x[n];
  m /= (double)N;
  const float mf = (float)m;
  for (uint32_t n = 0; n < N; ++n) x[n] -= mf;
}
} // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  window.assign(N, 0.0f);
  dsp::hann(window);

  state_on = false;
  state_switched_ms = now_ms();

#if !TEST_MODE
  analogReadResolution(12);                 // 0..4095
  analogSetPinAttenuation(V_PIN, ADC_11db); // widest-ish range
  analogSetPinAttenuation(I_PIN, ADC_11db);
#endif

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

  update_on_off_state();
  const char* state_str = state_on ? "on" : "off";

  float irms_target = state_on ? I_RMS_ON : I_RMS_OFF;
  float h2_amp      = state_on ? H2_ON    : H2_OFF;
  float phase_deg   = PHASE_DEG;

  float fs_eff = FS_CFG; // measured in REAL mode, used for DFT/Goertzel

#if TEST_MODE
  const ApplianceProfile& prof = PROFILES[profile_idx];
  if (state_on) {
    irms_target = prof.I_RMS_ON;
    h2_amp      = prof.H2_ON;
    phase_deg   = prof.PHASE_DEG;
  } else {
    irms_target = I_RMS_OFF;
    h2_amp      = H2_OFF;
    phase_deg   = PHASE_DEG;
  }
  signals::vi_test_signals(FS_CFG, N, V_RMS_TARGET, irms_target, F0, phase_deg, h2_amp, v, i);

#else
  // Sample VP/VN with timing loop. Convert raw->volts same as your earlier code.
  const uint32_t dt_us = (uint32_t)lroundf(1e6f / FS_CFG);
  uint32_t t_next = micros() + dt_us;

  const uint32_t t_start = micros();

  for (uint32_t n = 0; n < N; ++n) {
    while ((int32_t)(micros() - t_next) < 0) {}
    t_next += dt_us;

    const int rv = analogRead(V_PIN);
    const int ri = analogRead(I_PIN);

    v[n] = (rv / 4095.0f) * 3.3f;
    i[n] = (ri / 4095.0f) * 3.3f;
  }

  const uint32_t t_end = micros();
  if (t_end > t_start) {
    fs_eff = (1e6f * (float)N) / (float)(t_end - t_start);
  }

  // Remove the ~1.6V bias from both channels
  dc_remove(v);
  dc_remove(i);
#endif

  // Window current for harmonics
  for (uint32_t n = 0; n < N; ++n) i_win[n] = i[n] * window[n];

  // Metrics
  const float Vrms = dsp::rms(v.data(), N);
  const float Irms = dsp::rms(i.data(), N);
  const float P    = dsp::real_power(v.data(), i.data(), N);
  const float S    = dsp::apparent_power(Vrms, Irms);
  const float PF   = dsp::power_factor(P, S);

  float v1_rms = 0.0f, v1_phase = 0.0f;
  float i1_rms = 0.0f, i1_phase = 0.0f;
  dsp::dft_phasor(v.data(), N, fs_eff, F0, v1_rms, v1_phase);
  dsp::dft_phasor(i.data(), N, fs_eff, F0, i1_rms, i1_phase);

  const float phi     = v1_phase - i1_phase;
  const float phi_deg = phi * (180.0f / (float)M_PI);
  const float S1      = v1_rms * i1_rms;
  const float P1      = S1 * cosf(phi);
  const float Q1      = S1 * sinf(phi);
  const float PF_disp = cosf(phi);

  const std::vector<float> harms = { 2.0f*F0, 3.0f*F0, 4.0f*F0, 5.0f*F0 };
  const float THD_i = dsp::thd_goertzel(i_win.data(), N, fs_eff, F0, harms);
  const float THD_v = THDV_PLACEHOLDER;

  auto h_i = dsp::harmonic_ratios_goertzel(i_win.data(), N, fs_eff, F0, CFG_HARM_KMAX);

  double odd_sum = 0.0, even_sum = 0.0;
  for (const auto& kv : h_i) {
    const int f_hz = atoi(kv.first.c_str());
    const int k    = (int)lroundf((float)f_hz / F0);
    if (k >= 2) ((k % 2) == 0) ? (even_sum += kv.second) : (odd_sum += kv.second);
  }

  const float crest_i = dsp::crest_factor(i.data(), N);
  const float form_i  = dsp::form_factor(i.data(), N);

  if (isnan(prev_irms)) prev_irms = Irms;
  if (isnan(prev_p))    prev_p    = P;
  const float d_irms = Irms - prev_irms;
  const float d_p    = P    - prev_p;
  prev_irms = Irms;
  prev_p    = P;

  latest_event.clear();
  if (fabsf(d_irms) > 1.0f) {
    latest_event["type"] = (d_irms > 0) ? "on" : "off";
    latest_event["t_ms"] = (uint64_t)now_ms();
  }

#if TEST_MODE
  static bool prev_state_on_for_cycle = false;
  if (prev_state_on_for_cycle && !state_on) profile_idx = (profile_idx + 1) % NUM_PROFILES;
  prev_state_on_for_cycle = state_on;
#endif

  FrameCore core{
    Vrms, Irms, P, S, PF,
    v1_rms, i1_rms, phi_deg, P1, Q1, S1, PF_disp,
    THD_i, THD_v, zf((float)odd_sum), zf((float)even_sum),
    crest_i, form_i
  };

  const uint64_t t_ms = utils::now_ms();

  String json = pack_frame_json(
    t_ms, frame_id, fs_eff, N, WINDOW_NAME, F0,
    core, h_i, state_str, zf(d_irms), zf(d_p),
    true, true, true,
    CFG_FW_TAG, CFG_CAL_ID
  );

  // Debug injection (and labels in TEST_MODE)
  {
    StaticJsonDocument<2304> doc;
    if (deserializeJson(doc, json) == DeserializationError::Ok) {
#if TEST_MODE
      JsonArray labels = doc.createNestedArray("labels");
      if (state_on) labels.add(PROFILES[profile_idx].name);
#endif

      float vmin = 1e9f, vmax = -1e9f;
      float imin = 1e9f, imax = -1e9f;
      for (uint32_t n = 0; n < N; ++n) {
        vmin = fminf(vmin, v[n]); vmax = fmaxf(vmax, v[n]);
        imin = fminf(imin, i[n]); imax = fmaxf(imax, i[n]);
      }

      JsonObject dbg = doc.createNestedObject("dbg");
      dbg["v_pin"]  = "VP";
      dbg["i_pin"]  = "VN";
      dbg["fs_eff"] = fs_eff;
      dbg["v_min"]  = vmin;
      dbg["v_max"]  = vmax;
      dbg["i_min"]  = imin;
      dbg["i_max"]  = imax;

      JsonArray vs = dbg.createNestedArray("v_samp");
      JsonArray is = dbg.createNestedArray("i_samp");
      for (uint32_t k = 0; k < 32 && k < N; ++k) {
        vs.add(v[k]);
        is.add(i[k]);
      }

      json = "";
      serializeJson(doc, json);
    }
  }

  Serial.println(json);

  Serial.printf(
    "t=%llu ms state=%s fs=%.1f Vrms=%.3f Irms=%.3f P=%.3f PF=%.3f v1=%.3f i1=%.3f phi=%.2f THD_i=%.3f\n",
    (unsigned long long)t_ms,
    state_str,
    fs_eff,
    Vrms, Irms, P, PF,
    v1_rms, i1_rms, phi_deg,
    THD_i
  );

#if CFG_PUSH_ENABLE
  if ((frame_id % CFG_PUSH_EVERY_N) == 0) {
    const bool ok = fb_push_frame(CFG_DEVICE_ID, CFG_FW_TAG, frame_id, json);
    if (!ok) Serial.println(F("{\"push\":\"err\"}"));
  }
#endif

  ++frame_id;

  const uint32_t elapsed_ms = (uint32_t)(millis() - t_loop_start);
  const uint32_t period_ms  = (uint32_t)FRAME_PERIOD_MS;
  if (elapsed_ms < period_ms) delay(period_ms - elapsed_ms);
}
