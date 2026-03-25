#include "config.h"  // MUST be first

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <vector>
#include <math.h>

#if !TEST_MODE
  #include <SPI.h>
#endif

#include "signals.h"
#include "dsp.h"
#include "telemetry.h"
#include "utils.h"
#include "firebase.h"

StaticJsonDocument<160> latest_event;

// --- Constants ---
static constexpr float         FS_CFG          = CFG_FS_HZ;
static constexpr uint32_t      N               = CFG_N_SAMPLES;
static constexpr float         F0              = CFG_F0_HZ;
static constexpr const char* WINDOW_NAME     = CFG_WINDOW_NAME;
static constexpr float         FRAME_PERIOD_MS = CFG_FRAME_PERIOD_S * 1000.0f;

static constexpr float ON_DURATION_S    = CFG_ON_DURATION_S;
static constexpr float OFF_DURATION_S   = CFG_OFF_DURATION_S;
static constexpr float I_RMS_OFF        = CFG_I_RMS_OFF_A;
static constexpr float I_RMS_ON         = CFG_I_RMS_ON_A;
static constexpr float PHASE_DEG        = CFG_PHASE_DEG;
static constexpr float H2_OFF           = CFG_H2_OFF;
static constexpr float H2_ON            = CFG_H2_ON;
static constexpr float V_RMS_TARGET     = CFG_V_RMS_TARGET;
static constexpr float THDV_PLACEHOLDER = CFG_THDV_PLACEHOLDER;

#if TEST_MODE
struct ApplianceProfile {
  const char* name;
  float I_RMS_ON;
  float H2_ON;
  float H3_ON;
  float H4_ON;
  float H5_ON;
  float PHASE_DEG;
};

static const ApplianceProfile PROFILES[] = {
  {"Air Conditioner",           1.192706f, 0.00217665f, 0.04316344f, 0.00082146f, 0.01452099f, 35.7835f},
  {"Compact Fluorescent Lamp",  0.162197f, 0.01215796f, 0.80368712f, 0.00947182f, 0.44727753f, 64.2708f},
  {"Fan",                       0.372436f, 0.00331123f, 0.03764177f, 0.00172074f, 0.01769968f, 27.3296f},
  {"Fridge",                    3.089953f, 0.00298826f, 0.05050919f, 0.00097678f, 0.01596840f, 40.1619f},
  {"Hairdryer",                 4.223328f, 0.00262886f, 0.02072254f, 0.00095582f, 0.01376311f, 21.3599f},
  {"Heater",                    8.847182f, 0.00095121f, 0.01696448f, 0.00054136f, 0.01279767f, 20.7661f},
  {"Incandescent Light Bulb",   0.300547f, 0.00654241f, 0.01582962f, 0.00435971f, 0.01456974f, 24.6898f},
  {"Laptop",                    0.343313f, 0.00714021f, 0.90758221f, 0.00604128f, 0.68551296f, 59.4576f},
  {"Microwave",                 8.486173f, 0.04939847f, 0.32763411f, 0.01877993f, 0.13661158f, 48.5796f},
  {"Vacuum",                    7.662025f, 0.00518261f, 0.17515441f, 0.00139704f, 0.01429103f, 32.7705f},
  {"Washing Machine",           4.573205f, 0.00230555f, 0.03246858f, 0.00068493f, 0.01206051f, 54.5545f},
};

static constexpr int NUM_PROFILES = sizeof(PROFILES) / sizeof(PROFILES[0]);
static int profile_idx = 0;
#endif

static std::vector<float> v(N), i(N), window(N), i_win(N);

static uint32_t frame_id          = 0;
static bool     state_on          = false;
static uint32_t state_switched_ms = 0;
static float    prev_irms         = NAN;
static float    prev_p            = NAN;

#if !TEST_MODE
static bool g_adc_ok = false;
static uint16_t g_last_raw_i = 0, g_last_raw_v = 0;
static uint8_t  g_last_b_i[4] = {0,0,0,0}, g_last_b_v[4] = {0,0,0,0};
static uint16_t g_raw_i_min = 65535, g_raw_i_max = 0, g_raw_v_min = 65535, g_raw_v_max = 0;
static uint32_t g_raw_i_zero_count = 0, g_raw_i_full_count = 0, g_raw_v_zero_count = 0, g_raw_v_full_count = 0;
static float g_vrms_adc_pre_scale = 0.0f, g_irms_adc_pre_scale = 0.0f;
#endif

namespace {

inline uint32_t now_ms() { return millis(); }
inline uint32_t elapsed_since(uint32_t t0) { return (uint32_t)(millis() - t0); }
inline float zf(float x) { return utils::z(x); }

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

inline void patch_adc_dropouts(float* buffer, uint32_t size, float bias) {
  float last_valid_sample = bias; 
  for (uint32_t n = 0; n < size; ++n) {
    if (buffer[n] == 0.0f) buffer[n] = last_valid_sample;
    else last_valid_sample = buffer[n];
  }
}

#if !TEST_MODE
static SPIClass* adcSPI = &SPI;

static inline uint16_t ads8344_read_command(uint8_t command, uint8_t out_bytes[4]) {
  adcSPI->beginTransaction(SPISettings(CFG_ADC_SPI_HZ, MSBFIRST, CFG_ADC_SPI_MODE));
  digitalWrite(CFG_ADC_CS_PIN, LOW);
  delayMicroseconds(2); 
  adcSPI->transfer(command);
  delayMicroseconds(12); 
  out_bytes[0] = adcSPI->transfer(0x00);
  out_bytes[1] = adcSPI->transfer(0x00);
  out_bytes[2] = adcSPI->transfer(0x00);
  digitalWrite(CFG_ADC_CS_PIN, HIGH);
  adcSPI->endTransaction();

  return (uint16_t)((((uint16_t)(out_bytes[0] & 0x7F)) << 9) | 
                    (((uint16_t)out_bytes[1]) << 1) | 
                    (((uint16_t)out_bytes[2]) >> 7));
}

static inline float raw_to_volts(uint16_t raw) {
  return ((float)raw / 65535.0f) * CFG_ADC_VREF_V;
}

static inline bool adc_samples_look_valid(const std::vector<float>& v_out, const std::vector<float>& i_out, float& vmin, float& vmax, float& imin, float& imax) {
  vmin = 1e9f; vmax = -1e9f; imin = 1e9f; imax = -1e9f;
  for (uint32_t n = 0; n < N; ++n) {
    vmin = fminf(vmin, v_out[n]); vmax = fmaxf(vmax, v_out[n]);
    imin = fminf(imin, i_out[n]); imax = fmaxf(imax, i_out[n]);
  }
  return ((vmax - vmin) > 0.0005f) || ((imax - imin) > 0.0005f);
}

static inline float sample_ads8344_block(std::vector<float>& v_out, std::vector<float>& i_out) {
  const uint32_t dt_us = (uint32_t)lroundf(1e6f / FS_CFG);
  uint32_t t_next = micros() + dt_us;
  const uint32_t t_start = micros();

  g_raw_i_min = 65535; g_raw_i_max = 0; g_raw_v_min = 65535; g_raw_v_max = 0;
  g_raw_i_zero_count = 0; g_raw_i_full_count = 0; g_raw_v_zero_count = 0; g_raw_v_full_count = 0;

  for (uint32_t n = 0; n < N; ++n) {
    while ((int32_t)(micros() - t_next) < 0) {}
    t_next += dt_us;

    uint8_t bi[4], bv[4];
    uint16_t ci, cv;

    if (CFG_ADC_DEBUG_SINGLE_CH) {
      cv = ci = ads8344_read_command(CFG_ADC_DEBUG_FORCE_CMD, bv);
      for (int k = 0; k < 4; ++k) bi[k] = bv[k];
    } else {
      ci = ads8344_read_command(CFG_ADC_CMD_CH_I, bi);
      cv = ads8344_read_command(CFG_ADC_CMD_CH_V, bv);
    }

    g_last_raw_i = ci; g_last_raw_v = cv;
    for (int k = 0; k < 4; ++k) { g_last_b_i[k] = bi[k]; g_last_b_v[k] = bv[k]; }
    if (ci < g_raw_i_min) g_raw_i_min = ci; if (ci > g_raw_i_max) g_raw_i_max = ci;
    if (cv < g_raw_v_min) g_raw_v_min = cv; if (cv > g_raw_v_max) g_raw_v_max = cv;
    if (ci == 0) ++g_raw_i_zero_count; if (ci == 65535) ++g_raw_i_full_count;
    if (cv == 0) ++g_raw_v_zero_count; if (cv == 65535) ++g_raw_v_full_count;

    i_out[n] = raw_to_volts(ci); v_out[n] = raw_to_volts(cv);
  }
  const uint32_t t_end = micros();
  return (t_end > t_start) ? (1e6f * (float)N) / (float)(t_end - t_start) : FS_CFG;
}
#endif

} // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  window.assign(N, 0.0f);
  dsp::hann(window);
  state_on = false;
  state_switched_ms = now_ms();

#if !TEST_MODE
  pinMode(CFG_ADC_CS_PIN, OUTPUT);
  digitalWrite(CFG_ADC_CS_PIN, HIGH);
  adcSPI->begin(18, 19, 23, CFG_ADC_CS_PIN);
  uint8_t dump[4];
  ads8344_read_command(CFG_ADC_CMD_CH_I, dump);
  ads8344_read_command(CFG_ADC_CMD_CH_V, dump);
#endif

  if (wifi_init()) {
    Serial.print(F("{\"wifi\":\"ok\",\"ip\":\""));
    Serial.print(WiFi.localIP());
    Serial.println(F("\"}"));
  }
}

void loop() {
  const uint32_t t_loop_start = now_ms();
  update_on_off_state();
  const char* state_str = state_on ? "on" : "off";

  // --- Variable Declarations (Common Scope) ---
  float Vrms = 0.0f, Irms = 0.0f, P = 0.0f, S = 0.0f, PF = 0.0f;
  float v1_rms = 0.0f, i1_rms = 0.0f, phi_deg = 0.0f;
  float P1 = 0.0f, Q1 = 0.0f, S1 = 0.0f, PF_disp = 0.0f;
  float THD_i = 0.0f, THD_v = THDV_PLACEHOLDER;
  float h_ratio[CFG_HARM_KMAX + 1] = {0};
  double odd_sum = 0.0, even_sum = 0.0;
  float crest_i = 0.0f, form_i = 0.0f;
  float fs_eff = FS_CFG;

#if TEST_MODE
  const ApplianceProfile& prof = PROFILES[profile_idx];
  float cur_irms = state_on ? prof.I_RMS_ON : I_RMS_OFF;
  signals::vi_test_signals(FS_CFG, N, V_RMS_TARGET, cur_irms, F0, prof.PHASE_DEG, prof.H2_ON, prof.H3_ON, prof.H4_ON, prof.H5_ON, v, i);

  Vrms = dsp::rms(v.data(), N);
  Irms = dsp::rms(i.data(), N);
  P = dsp::real_power(v.data(), i.data(), N);
  S = Vrms * Irms;
  PF = (S > 0.01f) ? (P / S) : 1.0f;

  float v1_p = 0.0f, i1_p = 0.0f;
  dsp::dft_phasor(v.data(), N, fs_eff, F0, v1_rms, v1_p);
  dsp::dft_phasor(i.data(), N, fs_eff, F0, i1_rms, i1_p);
  phi_deg = (v1_p - i1_p) * (180.0f / (float)M_PI);
  S1 = v1_rms * i1_rms; P1 = S1 * cosf(v1_p - i1_p); Q1 = S1 * sinf(v1_p - i1_p); PF_disp = cosf(v1_p - i1_p);

  for (uint32_t n = 0; n < N; ++n) i_win[n] = i[n] * window[n];
  const float harms_hz[4] = { 2.0f*F0, 3.0f*F0, 4.0f*F0, 5.0f*F0 };
  THD_i = dsp::thd_goertzel(i_win.data(), N, fs_eff, F0, harms_hz, 4);
  dsp::harmonic_ratios_goertzel(i_win.data(), N, fs_eff, F0, CFG_HARM_KMAX, h_ratio);
  for (int k = 2; k <= CFG_HARM_KMAX; ++k) { ((k % 2) == 0) ? (even_sum += h_ratio[k]) : (odd_sum += h_ratio[k]); }
  crest_i = dsp::crest_factor(i.data(), N);
  form_i = dsp::form_factor(i.data(), N);

#else
  fs_eff = sample_ads8344_block(v, i);
  patch_adc_dropouts(v.data(), N, 1.65f);
  patch_adc_dropouts(i.data(), N, 1.65f);

  if (!CFG_ADC_DEBUG_SKIP_DC_REM) { dc_remove(v); dc_remove(i); }
  
  g_vrms_adc_pre_scale = dsp::rms(v.data(), N);
  g_irms_adc_pre_scale = dsp::rms(i.data(), N);

  for (uint32_t n = 0; n < N; ++n) { v[n] *= CFG_V_SCALE; i[n] *= CFG_I_SCALE; }

  Vrms = dsp::rms(v.data(), N);
  Irms = dsp::rms(i.data(), N);
  P = dsp::real_power(v.data(), i.data(), N);
  S = Vrms * Irms;

  if (Irms < 0.015f) { P = 0.0f; S = 0.0f; Irms = 0.0f; } // Deadband logic
  PF = (S > 0.01f) ? (P / S) : 0.0f;
#endif

  // --- Common Final Steps ---
  if (isnan(prev_irms)) prev_irms = Irms;
  if (isnan(prev_p))    prev_p    = P;
  float d_irms = Irms - prev_irms;
  float d_p = P - prev_p;
  prev_irms = Irms; prev_p = P;

  latest_event.clear();
  if (fabsf(d_irms) > 1.0f) {
    latest_event["type"] = (d_irms > 0) ? "on" : "off";
    latest_event["t_ms"] = (uint64_t)now_ms();
  }

#if TEST_MODE
  static bool prev_state_on_cycle = false;
  if (prev_state_on_cycle && !state_on) profile_idx = (profile_idx + 1) % NUM_PROFILES;
  prev_state_on_cycle = state_on;
#endif

  FrameCore core{ Vrms, Irms, P, S, PF, v1_rms, i1_rms, phi_deg, P1, Q1, S1, PF_disp, THD_i, THD_v, zf((float)odd_sum), zf((float)even_sum), crest_i, form_i };

  StaticJsonDocument<4096> doc;
  const uint64_t t_ms = utils::now_ms();

#if !TEST_MODE
  float dvmin, dvmax, dimin, dimax;
  g_adc_ok = adc_samples_look_valid(v, i, dvmin, dvmax, dimin, dimax);
#endif

  pack_frame_json(doc, t_ms, frame_id, fs_eff, N, WINDOW_NAME, F0, core, h_ratio, CFG_HARM_KMAX, state_str, zf(d_irms), zf(d_p), 
#if TEST_MODE
  true, true, true, 
#else
  g_adc_ok, true, true,
#endif
  CFG_FW_TAG, CFG_CAL_ID);

  JsonObject dbg = doc.createNestedObject("dbg");
  dbg["fs_eff"] = fs_eff;
#if !TEST_MODE
  dbg["vrms_adc_pre_scale"] = g_vrms_adc_pre_scale;
  dbg["irms_adc_pre_scale"] = g_irms_adc_pre_scale;
  dbg["raw_i_last"] = g_last_raw_i;
  dbg["raw_v_last"] = g_last_raw_v;
#endif

  JsonArray vs = dbg.createNestedArray("v_samp");
  JsonArray is = dbg.createNestedArray("i_samp");
  for (uint32_t k = 0; k < 32 && k < N; ++k) { vs.add(v[k]); is.add(i[k]); }

  String json;
  serializeJson(doc, json);
  Serial.println(json);

#if CFG_PUSH_ENABLE
  if ((frame_id % CFG_PUSH_EVERY_N) == 0) fb_push_frame(CFG_DEVICE_ID, CFG_FW_TAG, frame_id, json);
#endif

  ++frame_id;
  uint32_t elapsed_ms = elapsed_since(t_loop_start);
  if (elapsed_ms < FRAME_PERIOD_MS) delay(FRAME_PERIOD_MS - elapsed_ms);
}