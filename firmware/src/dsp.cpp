/**
 * dsp.cpp
 *
 * Core signal-processing math for Home Power Monitor firmware.
 *
 * Features:
 *   - Hann window generation
 *   - RMS computation
 *   - Real (active) power
 *   - Crest factor and form factor
 *   - Single-tone DFT for fundamental phasor extraction
 *   - Goertzel algorithm for harmonic magnitudes
 *   - THD via Goertzel (vector and no-heap overload)
 *   - Harmonic ratio mapping (std::map API + no-heap array API)
 *
 * Notes:
 *   - All scalar computations use double accumulators for stability.
 *   - "No-heap" APIs are provided for long-run stability on ESP32.
 */

#include "dsp.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace dsp {

// ─────────────────────────────────────────────────────────────
// Hann window
// ─────────────────────────────────────────────────────────────
void hann(std::vector<float>& w) {
  const size_t N = w.size();
  if (N == 0) return;
  if (N == 1) { w[0] = 1.0f; return; }

  const float M = float(N - 1);
  for (size_t n = 0; n < N; ++n) {
    w[n] = 0.5f * (1.0f - cosf(2.0f * float(M_PI) * float(n) / M));
  }
}

// ─────────────────────────────────────────────────────────────
// RMS
// ─────────────────────────────────────────────────────────────
float rms(const float* x, size_t N) {
  if (!x || N == 0) return 0.0f;
  double acc = 0.0;
  for (size_t n = 0; n < N; ++n) {
    const double xn = double(x[n]);
    acc += xn * xn;
  }
  return float(sqrt(acc / double(N)));
}

// ─────────────────────────────────────────────────────────────
// Real (active) power
// ─────────────────────────────────────────────────────────────
float real_power(const float* v, const float* i, size_t N) {
  if (!v || !i || N == 0) return 0.0f;
  double acc = 0.0;
  for (size_t n = 0; n < N; ++n) {
    acc += double(v[n]) * double(i[n]);
  }
  return float(acc / double(N));
}

// ─────────────────────────────────────────────────────────────
// Crest factor
// ─────────────────────────────────────────────────────────────
float crest_factor(const float* x, size_t N) {
  if (!x || N == 0) return 0.0f;

  const float r = rms(x, N);
  if (r < 1e-12f) return 0.0f;

  float peak = 0.0f;
  for (size_t n = 0; n < N; ++n) {
    const float a = fabsf(x[n]);
    if (a > peak) peak = a;
  }
  return peak / r;
}

// ─────────────────────────────────────────────────────────────
// Form factor
// ─────────────────────────────────────────────────────────────
float form_factor(const float* x, size_t N) {
  if (!x || N == 0) return 0.0f;

  double acc_abs = 0.0;
  for (size_t n = 0; n < N; ++n) {
    acc_abs += fabs(double(x[n]));
  }

  const double avg_rect = acc_abs / double(N);
  if (avg_rect < 1e-12) return 0.0f;

  const double r = double(rms(x, N));
  return float(r / avg_rect);
}

// ─────────────────────────────────────────────────────────────
// Fundamental phasor via single-bin DFT
// ─────────────────────────────────────────────────────────────
void dft_phasor(const float* x, size_t N, float fs, float f0,
                float& out_rms, float& out_phase_rad)
{
  if (!x || N == 0 || fs <= 0.0f || f0 <= 0.0f) {
    out_rms = 0.0f;
    out_phase_rad = 0.0f;
    return;
  }

  double re = 0.0, im = 0.0;
  const double w = 2.0 * M_PI * double(f0) / double(fs);

  for (size_t n = 0; n < N; ++n) {
    const double c = cos(double(n) * w);
    const double s = sin(double(n) * w);
    const double xn = double(x[n]);
    re += xn * c;
    im -= xn * s;  // e^{-jθ}
  }

  // Scale to peak amplitude (DFT-like) then convert to RMS for a sinusoid
  re *= (2.0 / double(N));
  im *= (2.0 / double(N));
  const double mag_peak = sqrt(re * re + im * im);

  out_rms       = float(mag_peak / sqrt(2.0));
  out_phase_rad = float(atan2(im, re));
}

// ─────────────────────────────────────────────────────────────
// Goertzel magnitude
// ─────────────────────────────────────────────────────────────
float goertzel_mag(const float* x, size_t N, float fs, float freq) {
  if (!x || N == 0 || fs <= 0.0f || freq <= 0.0f) return 0.0f;

  const double w  = 2.0 * M_PI * double(freq) / double(fs);
  const double cw = cos(w);
  const double sw = sin(w);
  const double coeff = 2.0 * cw;

  double s0 = 0.0, s1 = 0.0, s2 = 0.0;

  for (size_t n = 0; n < N; ++n) {
    s0 = double(x[n]) + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }

  const double re = s1 - s2 * cw;
  const double im = s2 * sw;

  const double mag = sqrt(re * re + im * im);

  // Scale similar to a single-bin DFT peak amplitude
  return float((2.0 / double(N)) * mag);
}

// ─────────────────────────────────────────────────────────────
// THD via Goertzel (vector API - existing)
// ─────────────────────────────────────────────────────────────
float thd_goertzel(const float* x_win, size_t N, float fs, float f0,
                   const std::vector<float>& harmonics)
{
  if (!x_win || N == 0 || fs <= 0.0f || f0 <= 0.0f) return 0.0f;

  const float fund = goertzel_mag(x_win, N, fs, f0);
  if (fund < 1e-12f) return 0.0f;

  double sumsq = 0.0;
  for (float h : harmonics) {
    if (h <= 0.0f) continue;
    const float mh = goertzel_mag(x_win, N, fs, h);
    sumsq += double(mh) * double(mh);
  }

  return float(sqrt(sumsq) / double(fund));
}

// ─────────────────────────────────────────────────────────────
// THD via Goertzel (no-heap overload)
// ─────────────────────────────────────────────────────────────
float thd_goertzel(const float* x_win, size_t N, float fs, float f0,
                   const float* harmonics_hz, size_t harmonics_len)
{
  if (!x_win || N == 0 || fs <= 0.0f || f0 <= 0.0f) return 0.0f;

  const float fund = goertzel_mag(x_win, N, fs, f0);
  if (fund < 1e-12f) return 0.0f;

  double sumsq = 0.0;
  for (size_t idx = 0; idx < harmonics_len; ++idx) {
    const float h = harmonics_hz[idx];
    if (h <= 0.0f) continue;
    const float mh = goertzel_mag(x_win, N, fs, h);
    sumsq += double(mh) * double(mh);
  }

  return float(sqrt(sumsq) / double(fund));
}

// ─────────────────────────────────────────────────────────────
// Harmonic ratios (std::map API - existing)
// ─────────────────────────────────────────────────────────────
std::map<String, float> harmonic_ratios_goertzel(const float* x_win, size_t N,
                                                 float fs, float f0, int kmax)
{
  std::map<String, float> out;

  if (!x_win || N == 0 || fs <= 0.0f || f0 <= 0.0f || kmax < 2)
    return out;

  float fund = goertzel_mag(x_win, N, fs, f0);
  if (fund < 1e-12f) fund = 1e-12f;

  for (int k = 2; k <= kmax; ++k) {
    const float fk = f0 * float(k);
    const float mk = goertzel_mag(x_win, N, fs, fk);

    char key[16];
    snprintf(key, sizeof(key), "%d", int(lround(fk)));

    out[String(key)] = mk / fund;
  }

  return out;
}

// ─────────────────────────────────────────────────────────────
// Harmonic ratios (no-heap array API)
//   out_ratio[k] = mag(k*f0)/mag(f0) for k=2..kmax
//   out_ratio[0..1] unused (set to 0)
// ─────────────────────────────────────────────────────────────
void harmonic_ratios_goertzel(const float* x_win, size_t N,
                              float fs, float f0, int kmax,
                              float* out_ratio)
{
  if (!out_ratio) return;

  // Clear output
  for (int k = 0; k <= kmax; ++k) out_ratio[k] = 0.0f;

  if (!x_win || N == 0 || fs <= 0.0f || f0 <= 0.0f || kmax < 2) return;

  float fund = goertzel_mag(x_win, N, fs, f0);
  if (fund < 1e-12f) fund = 1e-12f;

  for (int k = 2; k <= kmax; ++k) {
    const float fk = f0 * float(k);
    const float mk = goertzel_mag(x_win, N, fs, fk);
    out_ratio[k] = mk / fund;
  }
}

} // namespace dsp