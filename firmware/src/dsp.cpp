#include "dsp.h"
#include <math.h>

namespace dsp {

void hann(std::vector<float>& w) {
  const size_t N = w.size();
  if (N < 2) { if (N == 1) w[0] = 1.0f; return; }
  const float M = float(N - 1);
  for (size_t n = 0; n < N; ++n) {
    w[n] = 0.5f * (1.0f - cosf(2.0f * float(M_PI) * float(n) / M));
  }
}

float rms(const float* x, size_t N) {
  double acc = 0.0;
  for (size_t n = 0; n < N; ++n) acc += double(x[n]) * double(x[n]);
  return (N ? float(sqrt(acc / double(N))) : 0.0f);
}

float real_power(const float* v, const float* i, size_t N) {
  double acc = 0.0;
  for (size_t n = 0; n < N; ++n) acc += double(v[n]) * double(i[n]);
  return (N ? float(acc / double(N)) : 0.0f);
}

float crest_factor(const float* x, size_t N) {
  if (!N) return 0.0f;
  float r = rms(x, N); if (r < 1e-12f) return 0.0f;
  float peak = 0.0f;
  for (size_t n = 0; n < N; ++n) peak = max(peak, fabsf(x[n]));
  return peak / r;
}

float form_factor(const float* x, size_t N) {
  if (!N) return 0.0f;
  double acc_abs = 0.0;
  for (size_t n = 0; n < N; ++n) acc_abs += fabs(double(x[n]));
  double avg_rect = acc_abs / double(N);
  double r = rms(x, N);
  if (avg_rect < 1e-12) return 0.0f;
  return float(double(r) / avg_rect);
}

void dft_phasor(const float* x, size_t N, float fs, float f0, float& out_rms, float& out_phase_rad) {
  // X = (2/N) * sum x[n] * e^{-j2π f0 n/fs}  (≈ peak for pure tone)
  double re = 0.0, im = 0.0;
  const double w = 2.0 * M_PI * double(f0) / double(fs);
  for (size_t n = 0; n < N; ++n) {
    double c = cos(double(n) * w);
    double s = sin(double(n) * w);
    double xn = double(x[n]);
    re += xn * c;
    im -= xn * s; // minus for e^{-jθ}
  }
  re *= (2.0 / double(N));
  im *= (2.0 / double(N));
  double mag_peak = sqrt(re * re + im * im);
  out_rms = float(mag_peak / sqrt(2.0));
  out_phase_rad = float(atan2(im, re));
}

float goertzel_mag(const float* x, size_t N, float fs, float freq) {
  if (freq <= 0.0f || N == 0 || fs <= 0.0f) return 0.0f;
  double w = 2.0 * M_PI * double(freq) / double(fs);
  double cw = cos(w);
  double coeff = 2.0 * cw;
  double s0 = 0.0, s1 = 0.0, s2 = 0.0;
  for (size_t n = 0; n < N; ++n) {
    s0 = double(x[n]) + coeff * s1 - s2;
    s2 = s1; s1 = s0;
  }
  double re = s1 - s2 * cw;
  double im = s2 * sin(w);
  double mag = sqrt(re * re + im * im);
  // Scale similar to DFT magnitude; normalize by N/2 to roughly match FFT peak → use consistent ratios
  return float((2.0 / double(N)) * mag);
}

float thd_goertzel(const float* x_win, size_t N, float fs, float f0, const std::vector<float>& harmonics) {
  float fund = goertzel_mag(x_win, N, fs, f0);
  if (fund < 1e-12f) return 0.0f;
  double sumsq = 0.0;
  for (float h : harmonics) {
    float mh = goertzel_mag(x_win, N, fs, h);
    sumsq += double(mh) * double(mh);
  }
  return float(sqrt(sumsq) / double(fund));
}

std::map<String, float> harmonic_ratios_goertzel(const float* x_win, size_t N, float fs, float f0, int kmax) {
  std::map<String, float> out;
  float fund = goertzel_mag(x_win, N, fs, f0);
  if (fund < 1e-12f) fund = 1e-12f;
  for (int k = 2; k <= kmax; ++k) {
    float fk = f0 * float(k);
    float mk = goertzel_mag(x_win, N, fs, fk);
    char key[16]; snprintf(key, sizeof(key), "%d", int(lround(fk)));
    out[String(key)] = mk / fund;
  }
  return out;
}

} // namespace dsp
