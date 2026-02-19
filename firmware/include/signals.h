#ifndef SIGNALS_H
#define SIGNALS_H

#include <Arduino.h>
#include <vector>

namespace signals {

// out[n] = amplitude * sin(2π f n / fs + phase_rad)
void sine(float f,
          float fs,
          size_t N,
          float amplitude,
          float phase_rad,
          std::vector<float>& out);

// Backwards-compatible: only H2 injected into current.
void vi_test_signals(float fs,
                     size_t N,
                     float vrms,
                     float irms,
                     float f0,
                     float phase_deg,
                     float h2_amp,
                     std::vector<float>& v,
                     std::vector<float>& i);

// New: inject H2–H5 into current.
// hK_amp are ratios relative to the fundamental peak (Ipk).
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
                     std::vector<float>& i);

} // namespace signals

#endif // SIGNALS_H
