#pragma once
// Opaque-alpha detection (design 9.2). A source that marks frames as alpha while every pixel is
// opaque would double an OMT-HX coded picture for nothing, so alpha is dropped — but only after
// two seconds of continuous opacity. The hysteresis is required: changing alpha changes the HX
// coded height, so omthx reopens the encoder and forces a keyframe. Alpha must not flap.
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace ob {

class AlphaGate {
 public:
  // True when alpha should be dropped for this frame. Scans until the first value below 255.
  bool shouldDrop(bool hasAlpha, const uint8_t* alpha, size_t alphaBytes,
                  std::chrono::steady_clock::time_point now);
  void reset();

 private:
  std::chrono::steady_clock::time_point opaqueSince_{};
  bool sawOpaque_ = false;
};

}  // namespace ob
