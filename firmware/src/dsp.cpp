/**
 * dsp.cpp
 *
 * This module implements all of the core signal-processing math used
 * by the Home Power Monitor firmware.
 *
 * These functions are intentionally lightweight and deterministic so
 * they can run each frame (hundreds of Hz) on the ESP32 with no dynamic
 * memory churn.
 *
 * Features implemented here:
 *   - Window generation (Hann)
 *   - RMS computation
 *   - Real (active) power
 *   - Crest factor and form factor
 *   - Single-tone DFT for fundamental phasor extraction
 *   - Goertzel algorithm for efficient harmonic magnitudes
 *   - THD via Goertzel
 *   - Harmonic ratio mapping
 *
 * IMPORTANT:
 *   - All functions operate on raw float pointers for speed.
 *   - All algorithms assume uniformly sampled signals.
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
/**
 * hann(w)
 *
 * Generates a Hann (a.k.a. Hanning) window:
 *   w[n] = 0.5 * (1 - cos( 2π n / (N-1) ))
 *
 * WHY we use it:
 *   - When doing frequency analysis (e.g., Goertzel harmonic extraction),
 *     finite-length sample windows cause spectral leakage.
 *   - The Hann window smoothly tapers to zero at both ends, reducing leakage.
 *
 * Used in main.cpp to create i_win[] for harmonic/THD computation.
 */
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
// RMS (Root Mean Square)
// ─────────────────────────────────────────────────────────────
/**
 * rms(x, N)
 *
 * Computes RMS magnitude:
 *      RMS = sqrt( (1/N) * Σ x[n]^2 )
 *
 * WHY it matters:
 *   - RMS is the “effective magnitude” of AC voltage/current.
 *   - Power calculations use Vrms and Irms.
 */
float rms(const float* x, size_t N) {
  if (!x || N == 0) return 0.0f;
  double acc = 0.0;
  for (size_t n = 0; n < N; ++n)
    acc += double(x[n]) * double(x[n]);
  return float(sqrt(acc / double(N)));
}

// ─────────────────────────────────────────────────────────────
// Real (Active) Power
// ─────────────────────────────────────────────────────────────
/**
 * real_power(v, i)
 *
 * Computes true active power:
 *       P = (1/N) * Σ (v[n] * i[n])
 *
 * This captures BOTH magnitude and phase relationship.
 *
 * Example:
 *   - Purely resistive load → v & i in phase → P is max.
 *   - Inductive load → i lags v → some power becomes reactive → P reduces.
 */
float real_power(const float* v, const float* i, size_t N) {
  if (!v || !i || N == 0) return 0.0f;
  double acc = 0.0;
  for (size_t n = 0; n < N; ++n)
    acc += double(v[n]) * double(i[n]);
  return float(acc / double(N));
}

// ─────────────────────────────────────────────────────────────
// Crest Factor
// ─────────────────────────────────────────────────────────────
/**
 * crest_factor(x)
 *
 * crest_factor = (peak amplitude) / (RMS)
 *
 * WHY important:
 *   - Tells us how "spiky" the waveform is.
 *   - Pure sine → CF ≈ 1.414.
 *   - High crest factor → sharp peaks, often from SMPS loads.
 */
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
// Form Factor
// ─────────────────────────────────────────────────────────────
/**
 * form_factor(x)
 *
 * form_factor = RMS / (mean rectified value)
 *
 * WHY important:
 *   - Measures waveform “shape.”
 *   - Pure sine → ≈ 1.11.
 *   - Nonlinear loads distort the shape, changing form factor.
 */
float form_factor(const float* x, size_t N) {
  if (!x || N == 0) return 0.0f;

  double acc_abs = 0.0;
  for (size_t n = 0; n < N; ++n)
    acc_abs += fabs(double(x[n]));

  const double avg_rect = acc_abs / double(N);
  const double r = rms(x, N);

  if (avg_rect < 1e-12) return 0.0f;
  return float(r / avg_rect);
}

// ─────────────────────────────────────────────────────────────
// Fundamental Phasor via single-bin DFT
// ─────────────────────────────────────────────────────────────
/**
 * dft_phasor(x, N, fs, f0)
 *
 * Computes the FREQUENCY-SPECIFIC DFT at the fundamental frequency f0.
 *
 * Outputs:
 *   - out_rms: RMS magnitude of the fundamental frequency component
 *   - out_phase_rad: phase angle of that component (radians)
 *
 * Implementation:
 *   re = Σ x[n] cos(2π f0 n / fs)
 *   im = Σ x[n] sin(2π f0 n / fs)
 *
 * WHY it matters:
 *   - Displacement power factor requires knowing the phase between V and I.
 *   - This isolates ONLY the fundamental, ignoring harmonic distortion.
 */
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
    im -= xn * s;  // note: minus sign = e^{-jθ}
  }

  // Convert peak magnitude to RMS
  re *= (2.0 / double(N));
  im *= (2.0 / double(N));
  const double mag_peak = sqrt(re * re + im * im);

  out_rms       = float(mag_peak / sqrt(2.0));
  out_phase_rad = float(atan2(im, re));
}

// ─────────────────────────────────────────────────────────────
// Goertzel Algorithm (efficient single-bin spectrum magnitude)
// ─────────────────────────────────────────────────────────────
/**
 * goertzel_mag(x, N, fs, freq)
 *
 * Computes the magnitude of the signal at a SINGLE target frequency.
 *
 * WHY Goertzel:
 *   - FFT computes ALL bins (O(N log N)).
 *   - Goertzel computes ONE bin (O(N)).
 *   - Perfect for embedded harmonic analysis where we only care about
 *     specific frequencies (2nd, 3rd, 4th, 5th harmonic).
 *
 * Output scaling:
 *   - Returns a magnitude scaled similarly to FFT peak amplitude.
 */
float goertzel_mag(const float* x, size_t N, float fs, float freq) {
  if (!x || N == 0 || fs <= 0.0f || freq <= 0.0f) return 0.0f;

  const double w = 2.0 * M_PI * double(freq) / double(fs);
  const double cw = cos(w);
  const double coeff = 2.0 * cw;

  double s0 = 0.0, s1 = 0.0, s2 = 0.0;

  // Standard Goertzel recursion
  for (size_t n = 0; n < N; ++n) {
    s0 = double(x[n]) + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }

  const double re = s1 - s2 * cw;
  const double im = s2 * sin(w);

  const double mag = sqrt(re * re + im * im);

  // Scale similar to DFT magnitude
  return float((2.0 / double(N)) * mag);
}

// ─────────────────────────────────────────────────────────────
// THD via Goertzel
// ─────────────────────────────────────────────────────────────
/**
 * thd_goertzel(x_win, N, fs, f0, harmonics)
 *
 * Computes Total Harmonic Distortion (THD):
 *
 *      THD = sqrt( Σ harmonic_mag^2 ) / fundamental_mag
 *
 * where harmonic freqs come from the vector `harmonics`, e.g.:
 *   { 2*f0, 3*f0, 4*f0, 5*f0 }
 *
 * REQUIREMENT:
 *   - Input must already be windowed (e.g., Hann) to reduce leakage.
 */
float thd_goertzel(const float* x_win, size_t N, float fs, float f0,
                   const std::vector<float>& harmonics)
{
  if (!x_win || N == 0 || fs <= 0.0f || f0 <= 0.0f) return 0.0f;

  float fund = goertzel_mag(x_win, N, fs, f0);
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
// Harmonic ratios map (for ML and JSON output)
// ─────────────────────────────────────────────────────────────
/**
 * harmonic_ratios_goertzel(x_win, N, fs, f0, kmax)
 *
 * Computes harmonic magnitudes relative to the fundamental:
 *
 *   ratio_k = mag(f_k) / mag(f0)
 *
 * Produces:
 *   std::map<String, float>
 *     key = "<harmonic frequency in Hz>"
 *     value = ratio
 *
 * Example output keys for 60 Hz:
 *   "120", "180", "240", ...
 *
 * Used in telemetry JSON to provide ML with clean harmonic fingerprints.
 */
std::map<String, float> harmonic_ratios_goertzel(const float* x_win, size_t N,
                                                 float fs, float f0, int kmax)
{
  std::map<String, float> out;

  if (!x_win || N == 0 || fs <= 0.0f || f0 <= 0.0f || kmax < 2)
    return out;

  float fund = goertzel_mag(x_win, N, fs, f0);
  if (fund < 1e-12f) fund = 1e-12f;  // avoid divide-by-zero

  for (int k = 2; k <= kmax; ++k) {
    const float fk = f0 * float(k);
    const float mk = goertzel_mag(x_win, N, fs, fk);

    // Keys are string versions of the harmonic frequency in Hz
    char key[16];
    snprintf(key, sizeof(key), "%d", int(lround(fk)));

    out[String(key)] = mk / fund;
  }

  return out;
}

} // namespace dsp
