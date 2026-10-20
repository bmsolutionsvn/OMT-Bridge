#pragma once
// A source preview for the UI (design 11): the decoded UYVY frame box-downscaled to a small BGRA
// picture. This is the only RGB conversion in the engine, on a thumbnail-sized image.
#include <cstdint>
#include <vector>

#include "core/frame.h"

namespace ob {

struct Thumbnail {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> bgra;  // width * height * 4, BGRA byte order
};

// targetWidth is the long edge; the height keeps the source's aspect ratio (minimum 2).
// Returns an empty thumbnail for an invalid frame or a nonsensical width.
Thumbnail makeThumbnail(const Frame& frame, int targetWidth = 160);

}  // namespace ob
