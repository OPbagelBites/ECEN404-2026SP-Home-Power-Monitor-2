#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <Arduino.h>
#include <map>

struct FrameCore {
  float vrms, irms, P, S, PF_true;
  float v1_rms, i1_rms, phi_deg, P1, Q1, S1, PF_disp;
  float thd_i, thd_v, odd_sum_i, even_sum_i, crest_i, form_i;
};

String pack_frame_json(
  uint64_t t_ms, uint32_t frame_id, float fs, uint32_t N, const char* window_name, float f0_hz,
  const FrameCore& core,
  const std::map<String, float>& h_i,
  const char* state,
  float d_irms, float d_p,
  bool fft_ok = true, bool sync_ok = true, bool adc_ok = true,
  const char* fw = "fw-esp-0.1.0",
  const char* cal_id = "cal-default"
);

#endif // TELEMETRY_H
