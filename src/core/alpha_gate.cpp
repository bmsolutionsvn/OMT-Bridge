#include "core/alpha_gate.h"

namespace ob {
namespace {
constexpr auto kOpaqueWindow = std::chrono::seconds(2);
}

bool AlphaGate::shouldDrop(bool hasAlpha, const uint8_t* alpha, size_t alphaBytes,
                           std::chrono::steady_clock::time_point now) {
  if (!hasAlpha || alpha == nullptr || alphaBytes == 0) {
    reset();
    return false;
  }
  for (size_t i = 0; i < alphaBytes; ++i) {
    if (alpha[i] != 255) {
      reset();  // any transparency keeps alpha on and restarts the clock
      return false;
    }
  }
  if (!sawOpaque_) {
    sawOpaque_ = true;
    opaqueSince_ = now;
    return false;
  }
  return now - opaqueSince_ >= kOpaqueWindow;
}

void AlphaGate::reset() {
  sawOpaque_ = false;
  opaqueSince_ = {};
}

}  // namespace ob
