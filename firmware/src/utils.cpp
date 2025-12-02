#include "utils.h"
#include <time.h>

//Adds time feature to the program

namespace utils {
uint64_t now_ms() {
  // Prefer millis() relative if no RTC; if you have SNTP later, swap to epoch.
  // For JSON compatibility with your sandbox, we’ll return millis since boot added to a fake base if needed.
  // Here: just return millis() as a 64-bit millisecond counter.
  return static_cast<uint64_t>(millis());
}
} // namespace utils
