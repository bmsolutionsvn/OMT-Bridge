#pragma once
// The engine's one frame type: 8-bit 4:2:2 YUV, UYVY, or UYVA when alpha is present.
// Design 6.3. Luma and chroma are limited range (16-235); the alpha plane is full range.
// The layout matches OMT's and NDI's UYVA exactly: the UYVY plane, then the alpha plane.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ob {

struct VideoFormat {
  int width = 0;
  int height = 0;
  int fpsN = 60;
  int fpsD = 1;
  bool alpha = false;
  bool operator==(const VideoFormat&) const = default;
};

struct Frame {
  VideoFormat format;
  bool premultiplied = false;  // colour already multiplied by alpha (design 9.1); false = straight
  int64_t timestamp = 0;      // OMT units: 10,000,000 per second
  std::vector<uint8_t> uyvy;  // luma plane, then the alpha plane when format.alpha

  static constexpr size_t kNoAlpha = static_cast<size_t>(-1);

  size_t lumaBytes() const {
    return static_cast<size_t>(format.width) * 2u * static_cast<size_t>(format.height);
  }
  size_t alphaBytes() const {
    return format.alpha ? static_cast<size_t>(format.width) * static_cast<size_t>(format.height) : 0u;
  }
  // Only meaningful when format.alpha; kNoAlpha otherwise, so never index with it unguarded.
  size_t alphaOffset() const { return format.alpha ? lumaBytes() : kNoAlpha; }

  // A UYVY picture needs an even width, both dimensions at least 2, and exactly the right bytes.
  bool valid() const {
    if (format.width < 2 || format.height < 2 || format.width % 2 != 0) return false;
    return uyvy.size() == lumaBytes() + alphaBytes();
  }
};

}  // namespace ob
