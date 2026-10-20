#include "core/alpha_convert.h"

#include <algorithm>
#include <cmath>

namespace ob {
namespace {

uint8_t unmultiply(int value, int neutral, int lo, int hi, float a) {
  const int scaled = static_cast<int>(std::lround(static_cast<float>(neutral) + (static_cast<float>(value - neutral) / a)));
  return static_cast<uint8_t>(std::clamp(scaled, lo, hi));
}

}  // namespace

void toStraight(Frame& frame) {
  if (!frame.format.alpha || !frame.valid()) return;

  const size_t width = static_cast<size_t>(frame.format.width);
  uint8_t* alphaPlane = frame.uyvy.data() + frame.alphaOffset();
  for (int y = 0; y < frame.format.height; ++y) {
    uint8_t* row = frame.uyvy.data() + static_cast<size_t>(y) * width * 2;
    const uint8_t* alpha = alphaPlane + static_cast<size_t>(y) * width;
    for (size_t x = 0; x < width; x += 2) {
      const float a0 = static_cast<float>(alpha[x]) / 255.0f;
      const float a1 = static_cast<float>(alpha[x + 1]) / 255.0f;
      const float mean = (a0 + a1) / 2.0f;
      // Canonical UYVY: [U][Y0][V][Y1] per pair.
      row[x * 2] = mean > 0.0f ? unmultiply(row[x * 2], 128, 16, 240, mean) : 128;
      row[x * 2 + 1] = a0 > 0.0f ? unmultiply(row[x * 2 + 1], 16, 16, 235, a0) : 16;
      row[x * 2 + 2] = mean > 0.0f ? unmultiply(row[x * 2 + 2], 128, 16, 240, mean) : 128;
      row[x * 2 + 3] = a1 > 0.0f ? unmultiply(row[x * 2 + 3], 16, 16, 235, a1) : 16;
    }
  }
  frame.premultiplied = false;
}

}  // namespace ob
