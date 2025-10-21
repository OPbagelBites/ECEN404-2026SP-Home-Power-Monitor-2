#include "telemetry.h"
#include <ArduinoJson.h>

String pack_frame_json(
  uint64_t t_ms, uint32_t frame_id, float fs, uint32_t N, const char* window_name, float f0_hz,
  const FrameCore& c,
  const std::map<String, float>& h_i,
  const char* state,
  float d_irms, float d_p,
  bool fft_ok, bool sync_ok, bool adc_ok,
  const char* fw, const char* cal_id
) {
  StaticJsonDocument<1024> doc;

  doc["schema"] = 1;
  doc["delta_convention"] = "current_minus_previous";
  doc["h_i_units"] = "relative_to_fundamental";

  doc["t"] = t_ms;
  doc["frame_id"] = frame_id;
  doc["fs"] = fs;
  doc["N"] = N;
  doc["window"] = window_name;
  doc["freq_hz"] = roundf(f0_hz * 1000.0f) / 1000.0f;

  doc["rms_v"]   = roundf(c.vrms    * 1000.0f) / 1000.0f;
  doc["rms_i"]   = roundf(c.irms    * 1000.0f) / 1000.0f;
  doc["p"]       = roundf(c.P       * 1000.0f) / 1000.0f;
  doc["s"]       = roundf(c.S       * 1000.0f) / 1000.0f;
  doc["pf_true"] = roundf(c.PF_true * 1000.0f) / 1000.0f;

  doc["v1_rms"]  = roundf(c.v1_rms  * 1000.0f) / 1000.0f;
  doc["i1_rms"]  = roundf(c.i1_rms  * 1000.0f) / 1000.0f;
  doc["phi_deg"] = roundf(c.phi_deg * 100.0f)  / 100.0f;
  doc["phi_convention"] = "phi = angle(V1) - angle(I1); positive = current lags";
  doc["p1"] = roundf(c.P1 * 1000.0f) / 1000.0f;
  doc["q1"] = roundf(c.Q1 * 1000.0f) / 1000.0f;
  doc["s1"] = roundf(c.S1 * 1000.0f) / 1000.0f;
  doc["pf_disp"] = roundf(c.PF_disp * 1000.0f) / 1000.0f;

  doc["thd_i"] = roundf(c.thd_i * 1000.0f) / 1000.0f;
  doc["thd_v"] = roundf(c.thd_v * 1000.0f) / 1000.0f;

  // h_i map
  {
    JsonObject hobj = doc.createNestedObject("h_i");
    for (auto& kv : h_i) {
      hobj[kv.first] = roundf(kv.second * 1000.0f) / 1000.0f;
    }
  }

  doc["odd_sum_i"]  = roundf(c.odd_sum_i  * 1000.0f) / 1000.0f;
  doc["even_sum_i"] = roundf(c.even_sum_i * 1000.0f) / 1000.0f;
  doc["crest_i"]    = roundf(c.crest_i    * 1000.0f) / 1000.0f;
  doc["form_i"]     = roundf(c.form_i     * 1000.0f) / 1000.0f;

  doc["state"]  = state;
  doc["d_irms"] = roundf(d_irms * 1000.0f) / 1000.0f;
  doc["d_p"]    = roundf(d_p    * 1000.0f) / 1000.0f;

  doc["events"] = JsonArray(); // placeholder (empty)

  doc["fft_ok"]  = fft_ok;
  doc["sync_ok"] = sync_ok;
  doc["adc_ok"]  = adc_ok;

  doc["fw"]     = fw;
  doc["cal_id"] = cal_id;

  String out;
  serializeJson(doc, out);
  return out;
}
