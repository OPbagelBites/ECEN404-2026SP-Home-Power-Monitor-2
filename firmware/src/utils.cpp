#include "utils.h"

namespace utils {
uint64_t now_ms() {
  return static_cast<uint64_t>(millis());
}
}