#pragma once
#include <Arduino.h>
#include <math.h>

namespace utils {
  inline float z(float x, float eps = 1e-9f) { return (fabsf(x) < eps) ? 0.0f : x; }
  uint64_t now_ms();   // ms since boot (monotonic-ish)
}