#pragma once
#include "core/frame.h"

namespace ob {

// Premultiplied -> straight, in place, for a frame that already has alpha (design 9.1).
// Bytes are canonical UYVY: [U][Y0][V][Y1] per pair.
// Y = 16 + (Y' - 16) / a, C = 128 + (C' - 128) / a, clamped to 16..235 / 16..240;
// a == 0 becomes colour over black. Chroma uses the mean alpha of its two pixels.
// The alpha plane, `format` and `timestamp` are left untouched.
void toStraight(Frame& frame);

}  // namespace ob
