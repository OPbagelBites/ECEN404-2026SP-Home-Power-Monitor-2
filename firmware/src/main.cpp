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

// ─────────────────────────────────────────────────────────────
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

#if !TEST_MODE
static SPIClass* adcSPI = &SPI;

#ifndef CFG_ADC_DEBUG_RAW
#define CFG_ADC_DEBUG_RAW 0
#endif

static inline uint16_t ads8344_read_ch(uint8_t ch) {
  const uint8_t ctrl = (uint8_t)(0xC0 | ((ch & 0x07) << 3));

  digitalWrite(CFG_ADC_CS_PIN, LOW);
  adcSPI->transfer(ctrl);
  const uint8_t b1 = adcSPI->transfer(0x00);
  const uint8_t b2 = adcSPI->transfer(0x00);
  const uint8_t b3 = adcSPI->transfer(0x00);
  digitalWrite(CFG_ADC_CS_PIN, HIGH);

#if CFG_ADC_DEBUG_RAW
  static uint32_t dbg_cnt = 0;
  if ((dbg_cnt++ % 512) == 0) {
    Serial.printf("[adc] ch=%u bytes=%02X %02X %02X\n", ch, b1, b2, b3);
  }
#endif

  // Default: last 16 bits. Adjust this line once if alignment differs.
  return (uint16_t)(((uint16_t)b2 << 8) | b3);
}

static inline float sample_ads8344_block(std::vector<float>& v_out, std::vector<float>& i_out) {
  const uint32_t dt_us = (uint32_t)lroundf(1e6f / FS_CFG);
  uint32_t t_next = micros() + dt_us;

  const uint32_t t_start = micros();

  adcSPI->beginTransaction(SPISettings(CFG_ADC_SPI_HZ, MSBFIRST, SPI_MODE0));

  for (uint32_t n = 0; n < N; ++n) {
    while ((int32_t)(micros() - t_next) < 0) {}
    t_next += dt_us;

    const uint16_t ci = ads8344_read_ch(CFG_ADC_CH_I);
    const uint16_t cv = ads8344_read_ch(CFG_ADC_CH_V);

    i_out[n] = (ci / 65535.0f) * CFG_ADC_VREF_V;
    v_out[n] = (cv / 65535.0f) * CFG_ADC_VREF_V;
  }

  adcSPI->endTransaction();

  const uint32_t t_end = micros();
  if (t_end > t_start) return (1e6f * (float)N) / (float)(t_end - t_start);
  return FS_CFG;
}
#endif

} // namespace

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  window.assign(N, 0.0f);
  dsp::hann(window);

  state_on = false;
  state_switched_ms = now_ms();

#if !TEST_MODE
  pinMode(CFG_ADC_CS_PIN, OUTPUT);
  digitalWrite(CFG_ADC_CS_PIN, HIGH);
  adcSPI->begin(/*SCLK*/ 18, /*MISO*/ 19, /*MOSI*/ 23, /*SS*/ CFG_ADC_CS_PIN);
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
  float phase_deg   = PHASE_DEG;

  float h2_amp = state_on ? H2_ON : H2_OFF;
  float h3_amp = 0.0f;
  float h4_amp = 0.0f;
  float h5_amp = 0.0f;

  float fs_eff = FS_CFG;

#if TEST_MODE
  const ApplianceProfile& prof = PROFILES[profile_idx];
  if (state_on) {
    irms_target = prof.I_RMS_ON;
    phase_deg   = prof.PHASE_DEG;
    h2_amp      = prof.H2_ON;
    h3_amp      = prof.H3_ON;
    h4_amp      = prof.H4_ON;
    h5_amp      = prof.H5_ON;
  } else {
    irms_target = I_RMS_OFF;
    phase_deg   = PHASE_DEG;
    h2_amp      = H2_OFF;
    h3_amp = h4_amp = h5_amp = 0.0f;
  }

  signals::vi_test_signals(FS_CFG, N, V_RMS_TARGET, irms_target, F0, phase_deg,
                           h2_amp, h3_amp, h4_amp, h5_amp, v, i);
#else
  fs_eff = sample_ads8344_block(v, i);
  dc_remove(v);
  dc_remove(i);
#endif

  for (uint32_t n = 0; n < N; ++n) i_win[n] = i[n] * window[n];

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

  // No-heap harmonics list for THD
  const float harms_hz[4] = { 2.0f*F0, 3.0f*F0, 4.0f*F0, 5.0f*F0 };
  const float THD_i = dsp::thd_goertzel(i_win.data(), N, fs_eff, F0, harms_hz, 4);
  const float THD_v = THDV_PLACEHOLDER;

  // No-heap harmonic ratios
  float h_ratio[CFG_HARM_KMAX + 1];
  dsp::harmonic_ratios_goertzel(i_win.data(), N, fs_eff, F0, CFG_HARM_KMAX, h_ratio);

  double odd_sum = 0.0, even_sum = 0.0;
  for (int k = 2; k <= CFG_HARM_KMAX; ++k) {
    ((k % 2) == 0) ? (even_sum += h_ratio[k]) : (odd_sum += h_ratio[k]);
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

  // One document, one serialize.
  // 4096 is usually enough for your schema + dbg + 32 samples.
  StaticJsonDocument<4096> doc;

  pack_frame_json(
    doc,
    t_ms, frame_id, fs_eff, N, WINDOW_NAME, F0,
    core,
    h_ratio, CFG_HARM_KMAX,
    state_str,
    zf(d_irms), zf(d_p),
    true, true, true,
    CFG_FW_TAG, CFG_CAL_ID
  );

  // Enrich fields (same as before)
  doc["mode"] = TEST_MODE ? "SIM" : "REAL";

  // These mirror your previous get_h("120") etc.
  doc["h2_i_norm"] = h_ratio[2];
  doc["h3_i_norm"] = (CFG_HARM_KMAX >= 3) ? h_ratio[3] : 0.0f;
  doc["h4_i_norm"] = (CFG_HARM_KMAX >= 4) ? h_ratio[4] : 0.0f;
  doc["h5_i_norm"] = (CFG_HARM_KMAX >= 5) ? h_ratio[5] : 0.0f;

#if TEST_MODE
  if (state_on) {
    doc["appliance_type"] = PROFILES[profile_idx].name;
    JsonArray labels = doc.createNestedArray("labels");
    labels.add(PROFILES[profile_idx].name);
  }
#endif

  float vmin = 1e9f, vmax = -1e9f;
  float imin = 1e9f, imax = -1e9f;
  for (uint32_t n = 0; n < N; ++n) {
    vmin = fminf(vmin, v[n]); vmax = fmaxf(vmax, v[n]);
    imin = fminf(imin, i[n]); imax = fmaxf(imax, i[n]);
  }

  JsonObject dbg = doc.createNestedObject("dbg");
#if TEST_MODE
  dbg["v_src"] = "SIM";
  dbg["i_src"] = "SIM";
#else
  dbg["v_src"] = "ADS8344_CH1";
  dbg["i_src"] = "ADS8344_CH0";
#endif
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

  // Serialize once, with reserved capacity to reduce reallocs.
  String json;
  json.reserve(4096);
  serializeJson(doc, json);

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