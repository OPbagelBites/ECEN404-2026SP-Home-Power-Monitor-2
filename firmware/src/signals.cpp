#include "signals.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace signals {

void sine(float f,
          float fs,
          size_t N,
          float amplitude,
          float phase_rad,
          std::vector<float>& out)
{
  if (fs <= 0.0f || N == 0 || f <= 0.0f) {
    out.clear();
    return;
  }

  out.resize(N);
  const float w = 2.0f * (float)M_PI * f / fs;

  for (size_t n = 0; n < N; ++n) {
    out[n] = amplitude * sinf(w * (float)n + phase_rad);
  }
}

static inline void vi_test_signals_core(float fs,
                                       size_t N,
                                       float vrms,
                                       float irms,
                                       float f0,
                                       float phase_deg,
                                       float h2_amp,
                                       float h3_amp,
                                       float h4_amp,
                                       float h5_amp,
                                       std::vector<float>& v,
                                       std::vector<float>& i)
{
  if (fs <= 0.0f || N == 0 || f0 <= 0.0f) {
    v.clear();
    i.clear();
    return;
  }

  v.resize(N);
  i.resize(N);

  const float Vpk = vrms * sqrtf(2.0f);
  const float Ipk = irms * sqrtf(2.0f);

  const float w0 = 2.0f * (float)M_PI * f0 / fs;
  const float ph = phase_deg * (float)M_PI / 180.0f;

  for (size_t n = 0; n < N; ++n) {
    const float t = (float)n;

    // Voltage: pure fundamental, reference phase 0
    const float v_f = sinf(w0 * t);
    v[n] = Vpk * v_f;

    // Current: fundamental + harmonics, same phase convention
    float i_val = Ipk * sinf(w0 * t - ph);

    if (h2_amp != 0.0f) i_val += (h2_amp * Ipk) * sinf(2.0f * w0 * t - ph);
    if (h3_amp != 0.0f) i_val += (h3_amp * Ipk) * sinf(3.0f * w0 * t - ph);
    if (h4_amp != 0.0f) i_val += (h4_amp * Ipk) * sinf(4.0f * w0 * t - ph);
    if (h5_amp != 0.0f) i_val += (h5_amp * Ipk) * sinf(5.0f * w0 * t - ph);

    i[n] = i_val;
  }
}

void vi_test_signals(float fs,
                     size_t N,
                     float vrms,
                     float irms,
                     float f0,
                     float phase_deg,
                     float h2_amp,
                     std::vector<float>& v,
                     std::vector<float>& i)
{
  vi_test_signals_core(fs, N, vrms, irms, f0, phase_deg,
                       h2_amp, 0.0f, 0.0f, 0.0f, v, i);
}

void vi_test_signals(float fs,
                     size_t N,
                     float vrms,
                     float irms,
                     float f0,
                     float phase_deg,
                     float h2_amp,
                     float h3_amp,
                     float h4_amp,
                     float h5_amp,
                     std::vector<float>& v,
                     std::vector<float>& i)
{
  vi_test_signals_core(fs, N, vrms, irms, f0, phase_deg,
                       h2_amp, h3_amp, h4_amp, h5_amp, v, i);
}

} // namespace signals
