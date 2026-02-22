#ifndef DSP_H
#define DSP_H

#include <Arduino.h>
#include <vector>
#include <map>

namespace dsp {

// Window
void hann(std::vector<float>& w);

// Core metrics
float rms(const float* x, size_t N);
float real_power(const float* v, const float* i, size_t N);
inline float apparent_power(float vrms, float irms) { return vrms * irms; }
inline float power_factor(float P, float S) { return (S > 1e-12f) ? (P / S) : 0.0f; }
float crest_factor(const float* x, size_t N);
float form_factor(const float* x, size_t N);

// Single-bin complex DFT (fundamental phasor)
void dft_phasor(const float* x, size_t N, float fs, float f0, float& out_rms, float& out_phase_rad);

// Goertzel for a specific frequency (magnitude only)
float goertzel_mag(const float* x, size_t N, float fs, float freq);

// THD using Goertzel (old API)
float thd_goertzel(const float* x_win, size_t N, float fs, float f0, const std::vector<float>& harmonics);

// THD using Goertzel (new: no heap allocation)
float thd_goertzel(const float* x_win, size_t N, float fs, float f0,
                   const float* harmonics_hz, size_t harmonics_len);

// Harmonic ratios (old API)
std::map<String, float> harmonic_ratios_goertzel(const float* x_win, size_t N, float fs, float f0, int kmax);

// Harmonic ratios (new: no heap allocation)
// out_ratio[k] = mag(k*f0)/mag(f0) for k=2..kmax. out_ratio[0..1] unused.
void harmonic_ratios_goertzel(const float* x_win, size_t N, float fs, float f0, int kmax, float* out_ratio);

} // namespace dsp

#endif // DSP_H