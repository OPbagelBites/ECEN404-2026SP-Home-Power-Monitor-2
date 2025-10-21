#pragma once
#include <Arduino.h>
#include <vector>

namespace signals {

// Basic sine
void sine(float f, float fs, size_t N, float amplitude, float phase_rad, std::vector<float>& out);

// Test V/I pair (sim mode) – mirrors your Python vi_test_signals()
// Voltage phase = 0; current lags by +phase_deg (pass negative degrees if you want lag)
void vi_test_signals(float fs, size_t N, float vrms, float irms, float f0, float phase_deg, float h2_amp,
                     std::vector<float>& v, std::vector<float>& i);

} // namespace signals
