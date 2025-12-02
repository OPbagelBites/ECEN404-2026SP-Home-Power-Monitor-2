/**
 * signals.cpp
 *
 * This module generates synthetic waveforms for TEST_MODE.
 *
 * Why this file exists:
 *  - Before the AFE board + ADC are integrated, we still need to test the DSP,
 *    JSON telemetry, ML pipeline, Firebase upload, etc.
 *  - These functions generate clean, controlled AC voltage & current signals
 *    that mimic real loads (including RMS control, phase shift, and harmonics).
 *
 * Key functions:
 *   - sine(): general-purpose sine wave generator.
 *   - vi_test_signals(): generates realistic voltage/current pairs with:
 *         • Vrms and Irms control
 *         • fundamental frequency f0
 *         • phase shift between V and I
 *         • controllable 2nd harmonic distortion on current
 *
 * The output of vi_test_signals() feeds directly into main.cpp’s DSP pipeline
 * as if it were real ADC data.
 */

#include "signals.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace signals {

// ─────────────────────────────────────────────────────────────
// General-purpose sine generator
// ─────────────────────────────────────────────────────────────
/**
 * sine(f, fs, N, amplitude, phase_rad, out)
 *
 * Generates:
 *      out[n] = amplitude * sin( 2π f n / fs + phase )
 *
 * Inputs:
 *   f          = frequency of the sine wave
 *   fs         = sampling frequency
 *   N          = number of samples
 *   amplitude  = peak amplitude (NOT RMS)
 *   phase_rad  = initial phase of the sine wave in radians
 *
 * Why it exists:
 *   - This is a reusable building block for synthetic test signals.
 *   - vi_test_signals() could call it twice (V and I), but for efficiency we
 *     inline that logic inside vi_test_signals().
 */
void sine(float f, float fs, size_t N,
          float amplitude,
          float phase_rad,
          std::vector<float>& out)
{
  if (fs <= 0.0f || N == 0) {
    out.clear();
    return;
  }

  out.resize(N);

  const float w = 2.0f * float(M_PI) * f / fs;  // angular step per sample

  for (size_t n = 0; n < N; ++n) {
    out[n] = amplitude * sinf(w * float(n) + phase_rad);
  }
}

// ─────────────────────────────────────────────────────────────
// Synthetic V/I generator for TEST_MODE
// ─────────────────────────────────────────────────────────────
/**
 * vi_test_signals()
 *
 * Generates synthetic voltage and current waveforms that look like the signals
 * we will eventually acquire from the AFE + ADC.
 *
 * Features:
 *   - Vrms and Irms control
 *   - Fundamental frequency f0 (e.g., 60 Hz)
 *   - Adjustable phase shift (current lagging voltage)
 *   - Optional 2nd harmonic injection on the current
 *
 * Output:
 *   v[n] = Vpk * sin(w0*n)
 *   i[n] = Ipk * sin(w0*n - phase_rad)  [+ harmonic term]
 *
 * Why this is useful:
 *   - Lets us test the entire pipeline (DSP, event detection, harmonics, Firebase)
 *     without hardware.
 *   - Appliances in main.cpp’s TEST_MODE simply use different Irms, phase, and
 *     h2_amp parameters to mimic different devices.
 */
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
  // Basic input validation
  if (fs <= 0.0f || N == 0 || f0 <= 0.0f) {
    v.clear();
    i.clear();
    return;
  }

  v.resize(N);
  i.resize(N);

  // Convert RMS → peak
  // A sine wave’s peak = RMS * sqrt(2)
  const float Vpk = vrms * sqrtf(2.0f);
  const float Ipk = irms * sqrtf(2.0f);

  // Angular step of fundamental
  const float w0 = 2.0f * float(M_PI) * f0 / fs;

  // Convert phase lag from degrees → radians
  // Positive phase_deg means current lags voltage
  const float ph = phase_deg * float(M_PI) / 180.0f;

  /**
   * LOOP OVER SAMPLES:
   *   voltage[n] = Vpk * sin(w0*n)
   *   current[n] = Ipk * sin(w0*n - ph)
   *
   *   If h2_amp > 0, we inject:
   *      + h2_amp * Ipk * sin( 2*w0*n - ph )
   *
   *   This creates a realistic 2nd harmonic, common in:
   *      - rectifiers
   *      - motor loads
   *      - CFLs / SMPS behavior
   */
  for (size_t n = 0; n < N; ++n) {
    const float t = float(n);

    // Voltage is always reference phase = 0 in simulation
    v[n] = Vpk * sinf(w0 * t);

    // Current with phase shift
    i[n] = Ipk * sinf(w0 * t - ph);

    // Optional 2nd harmonic (scaled vs fundamental)
    if (h2_amp != 0.0f) {
      i[n] += h2_amp * Ipk * sinf(2.0f * w0 * t - ph);
    }
  }
}

} // namespace signals
