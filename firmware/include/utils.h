#pragma once
#include <Arduino.h>

namespace utils {
inline float z(float x, float eps = 1e-9f) { return (fabsf(x) < eps) ? 0.0f : x; }
uint64_t now_ms();         // epoch ms (best effort)
} // namespace utils
