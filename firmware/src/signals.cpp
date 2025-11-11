#include "signals.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace signals {

void sine(float f, float fs, size_t N, float amplitude, float phase_rad, std::vector<float>& out) {
  if (fs <= 0.0f || N == 0) { out.clear(); return; }
  out.resize(N);
  const float w = 2.0f * float(M_PI) * f / fs;
  for (size_t n = 0; n < N; ++n) {
    out[n] = amplitude * sinf(w * float(n) + phase_rad);
  }
}

void vi_test_signals(float fs, size_t N, float vrms, float irms, float f0, float phase_deg, float h2_amp,
                     std::vector<float>& v, std::vector<float>& i) {
  if (fs <= 0.0f || N == 0 || f0 <= 0.0f) { v.clear(); i.clear(); return; }

  v.resize(N);
  i.resize(N);

  const float Vpk = vrms * sqrtf(2.0f);
  const float Ipk = irms * sqrtf(2.0f);
  const float w0  = 2.0f * float(M_PI) * f0 / fs;
  const float ph  = phase_deg * float(M_PI) / 180.0f; // +ph ⇒ current lags

  for (size_t n = 0; n < N; ++n) {
    const float t = float(n);
    v[n] = Vpk * sinf(w0 * t);                 // voltage ref phase 0
    i[n] = Ipk * sinf(w0 * t - ph);            // current lags by +phase_deg
    if (h2_amp != 0.0f) {
      i[n] += h2_amp * Ipk * sinf(2.0f * w0 * t - ph); // optional 2nd harmonic on current
    }
  }
}

} // namespace signals
