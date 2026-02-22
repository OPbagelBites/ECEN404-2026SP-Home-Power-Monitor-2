#include "telemetry.h"
#include <math.h>

#ifndef EVENT_DOC_BYTES
  #define EVENT_DOC_BYTES 160
#endif

extern StaticJsonDocument<EVENT_DOC_BYTES> latest_event;

static inline float r3(float x) { return roundf(x * 1000.0f) / 1000.0f; }
static inline float r2(float x) { return roundf(x *  100.0f) /  100.0f; }

void pack_frame_json(
  JsonDocument& doc,
  uint64_t t_ms,
  uint32_t frame_id,
  float fs,
  uint32_t N,
  const char* window_name,
  float f0_hz,
  const FrameCore& c,
  const float* harm_ratio,
  int kmax,
  const char* state,
  float d_irms,
  float d_p,
  bool fft_ok,
  bool sync_ok,
  bool adc_ok,
  const char* fw,
  const char* cal_id
) {
  doc.clear();

  doc["schema"]            = 1;
  doc["delta_convention"]  = "current_minus_previous";
  doc["h_i_units"]         = "relative_to_fundamental";

  doc["t"]        = t_ms;
  doc["frame_id"] = frame_id;
  doc["fs"]       = fs;
  doc["N"]        = N;
  doc["window"]   = window_name;
  doc["freq_hz"]  = r3(f0_hz);

  doc["rms_v"]    = r3(c.vrms);
  doc["rms_i"]    = r3(c.irms);
  doc["p"]        = r3(c.P);
  doc["s"]        = r3(c.S);
  doc["pf_true"]  = r3(c.PF_true);

  doc["v1_rms"]   = r3(c.v1_rms);
  doc["i1_rms"]   = r3(c.i1_rms);
  doc["phi_deg"]  = r2(c.phi_deg);
  doc["phi_convention"] = "phi = angle(V1) - angle(I1); positive = current lags";

  doc["p1"]       = r3(c.P1);
  doc["q1"]       = r3(c.Q1);
  doc["s1"]       = r3(c.S1);
  doc["pf_disp"]  = r3(c.PF_disp);

  doc["thd_i"]    = r3(c.thd_i);
  doc["thd_v"]    = r3(c.thd_v);

  // Harmonics object: same keys as before ("120","180",...)
  {
    JsonObject hobj = doc["h_i"].to<JsonObject>();
    if (harm_ratio && kmax >= 2 && f0_hz > 0.0f) {
      for (int k = 2; k <= kmax; ++k) {
        const int fk = (int)lroundf(f0_hz * float(k)); // e.g. 60*2=120
        char key[16];
        snprintf(key, sizeof(key), "%d", fk);
        hobj[key] = r3(harm_ratio[k]);
      }
    }
  }

  doc["odd_sum_i"]  = r3(c.odd_sum_i);
  doc["even_sum_i"] = r3(c.even_sum_i);

  doc["crest_i"] = r3(c.crest_i);
  doc["form_i"]  = r3(c.form_i);

  doc["state"]  = state;
  doc["d_irms"] = r3(d_irms);
  doc["d_p"]    = r3(d_p);

  // Events (only if populated)
  {
    JsonArray events = doc["events"].to<JsonArray>();
    JsonObject ev = latest_event.as<JsonObject>();
    if (!ev.isNull() && ev.containsKey("type") && ev.containsKey("t_ms")) {
      events.add(ev);
    }
  }

  doc["fft_ok"]  = fft_ok;
  doc["sync_ok"] = sync_ok;
  doc["adc_ok"]  = adc_ok;

  doc["fw"]     = fw;
  doc["cal_id"] = cal_id;
}