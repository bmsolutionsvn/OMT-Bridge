#pragma once
// The engine's one frame type. The picture lane carries either 8-bit 4:2:2 YUV, UYVY (or UYVA when alpha
// is present), or NV12: full-range BT.709 with no alpha, as OMT-HX decodes HX.
// UYVY luma and chroma are limited range (16-235); the alpha plane is full range.
// The UYVY layout matches OMT's and NDI's UYVA exactly: the UYVY plane, then the alpha plane.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace omthx {
struct CodedFrame;  // OMT-HX's coded frame, kept opaque here so core/ stays free of the omthx headers
}

namespace ob {

struct VideoFormat {
  int width = 0;
  int height = 0;
  int fpsN = 60;
  int fpsD = 1;
  bool alpha = false;
  bool operator==(const VideoFormat&) const = default;
};

enum class Layout { Uyvy, Nv12 };

struct Frame {
  VideoFormat format;
  Layout layout = Layout::Uyvy;
  bool premultiplied = false;  // colour already multiplied by alpha (design 9.1); false = straight
  int64_t timestamp = 0;      // OMT units: 10,000,000 per second
  std::vector<uint8_t> uyvy;  // luma plane, then the alpha plane when format.alpha
  // NV12 (layout Nv12): Y for width * height bytes, then interleaved UV for width * height / 2. Full range.
  std::vector<uint8_t> nv12;
  // The source's coded frame, when this one came in as coded data (design 3.1); null otherwise. A
  // same-family output sends it on with Sender::sendCoded, with no picture in between.
  std::shared_ptr<const omthx::CodedFrame> coded;

  // A frame may travel coded-only, with no picture: picture consumers must ignore it, not index it.
  bool codedOnly() const { return coded && uyvy.empty() && nv12.empty(); }

  static constexpr size_t kNoAlpha = static_cast<size_t>(-1);

  size_t lumaBytes() const {
    return static_cast<size_t>(format.width) * 2u * static_cast<size_t>(format.height);
  }
  size_t alphaBytes() const {
    return format.alpha ? static_cast<size_t>(format.width) * static_cast<size_t>(format.height) : 0u;
  }
  // Only meaningful when format.alpha; kNoAlpha otherwise, so never index with it unguarded.
  size_t alphaOffset() const { return format.alpha ? lumaBytes() : kNoAlpha; }

  // An NV12 picture needs an even width, both dimensions at least 2, no alpha, and exactly the right
  // bytes. A UYVY picture needs an even width, both dimensions at least 2, and exactly the right bytes.
  bool valid() const {
    if (format.width < 2 || format.height < 2 || format.width % 2 != 0) return false;
    if (layout == Layout::Nv12) {
      if (format.alpha) return false;
      return nv12.size() == static_cast<size_t>(format.width) * static_cast<size_t>(format.height) * 3u / 2u;
    }
    return uyvy.size() == lumaBytes() + alphaBytes();
  }

  // The frame's picture in limited-range NV12, made once and shared by every sink that needs it; null for
  // a UYVY frame. Safe from several threads at once, and copies of a frame share the one conversion.
  std::shared_ptr<const std::vector<uint8_t>> limitedNv12() const;

 private:
  struct LimitedNv12 {
    std::once_flag once;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
  };
  std::shared_ptr<LimitedNv12> limited_ = std::make_shared<LimitedNv12>();
};

}  // namespace ob
