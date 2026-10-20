#include "core/thumbnail.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ob {
namespace {

uint8_t clamp255(double v) {
  if (v <= 0.0) return 0;
  if (v >= 255.0) return 255;
  return static_cast<uint8_t>(std::lround(v));
}

}  // namespace

Thumbnail makeThumbnail(const Frame& frame, int targetWidth) {
  Thumbnail out;
  if (targetWidth < 2 || !frame.valid()) return out;

  const int sw = frame.format.width;
  const int sh = frame.format.height;
  // The long edge is targetWidth, so a portrait source gets a 90x160 preview, not an empty one.
  const double scale =
      std::min(1.0, static_cast<double>(targetWidth) / static_cast<double>(std::max(sw, sh)));
  const int tw = std::max(2, static_cast<int>(std::lround(sw * scale)));
  const int th = std::max(2, static_cast<int>(std::lround(sh * scale)));
  out.width = tw;
  out.height = th;
  out.bgra.resize(static_cast<size_t>(tw) * static_cast<size_t>(th) * 4u);

  const size_t stride = static_cast<size_t>(sw) * 2u;
  for (int y = 0; y < th; ++y) {
    const int y0 = y * sh / th;
    const int y1 = std::max(y0 + 1, (y + 1) * sh / th);
    for (int x = 0; x < tw; ++x) {
      const int x0 = x * sw / tw;
      const int x1 = std::max(x0 + 1, (x + 1) * sw / tw);

      // Box average. UYVY stores U and V once per two pixels; averaging them over the same box as
      // luma is correct at thumbnail scale (each source pixel contributes one chroma sample).
      uint64_t sumY = 0, sumU = 0, sumV = 0;
      uint64_t n = 0;
      for (int sy = y0; sy < y1; ++sy) {
        const uint8_t* row = frame.uyvy.data() + static_cast<size_t>(sy) * stride;
        for (int sx = x0; sx < x1; ++sx) {
          const int pair = sx & ~1;
          sumY += row[static_cast<size_t>(pair) * 2u + ((sx & 1) ? 3u : 1u)];
          sumU += row[static_cast<size_t>(pair) * 2u];
          sumV += row[static_cast<size_t>(pair) * 2u + 2u];
          ++n;
        }
      }
      if (n == 0) continue;

      // Limited-range BT.709, the colour space OMT and NDI carry.
      const double yy = static_cast<double>(sumY) / static_cast<double>(n) - 16.0;
      const double uu = static_cast<double>(sumU) / static_cast<double>(n) - 128.0;
      const double vv = static_cast<double>(sumV) / static_cast<double>(n) - 128.0;
      const double r = 1.164 * yy + 1.793 * vv;
      const double g = 1.164 * yy - 0.213 * uu - 0.533 * vv;
      const double b = 1.164 * yy + 2.112 * uu;

      const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(tw) +
                        static_cast<size_t>(x)) * 4u;
      out.bgra[i + 0] = clamp255(b);
      out.bgra[i + 1] = clamp255(g);
      out.bgra[i + 2] = clamp255(r);
      out.bgra[i + 3] = 255;
    }
  }
  return out;
}

}  // namespace ob
