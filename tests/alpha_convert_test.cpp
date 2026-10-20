#include <doctest/doctest.h>
#include "core/alpha_convert.h"

static ob::Frame make(int y, int u, int v, int a) {
  ob::Frame f;
  f.format = ob::VideoFormat{2, 2, 60, 1, true};
  f.premultiplied = true;
  f.uyvy.assign(f.lumaBytes() + f.alphaBytes(), 0);
  // 2x2: UYVY pairs: Y0 U Y1 V per row.
  f.uyvy[0] = static_cast<uint8_t>(y);
  f.uyvy[1] = static_cast<uint8_t>(u);
  f.uyvy[2] = static_cast<uint8_t>(y);
  f.uyvy[3] = static_cast<uint8_t>(v);
  f.uyvy[4] = static_cast<uint8_t>(y);
  f.uyvy[5] = static_cast<uint8_t>(u);
  f.uyvy[6] = static_cast<uint8_t>(y);
  f.uyvy[7] = static_cast<uint8_t>(v);
  for (size_t i = 0; i < f.alphaBytes(); ++i) f.uyvy[f.lumaBytes() + i] = static_cast<uint8_t>(a);
  return f;
}

TEST_CASE("toStraight leaves opaque and empty alpha unchanged") {
  auto opaque = make(100, 120, 130, 255);
  ob::toStraight(opaque);
  CHECK(opaque.uyvy[0] == 100);
  CHECK(opaque.uyvy[1] == 120);
  CHECK_FALSE(opaque.premultiplied);

  auto black = make(200, 200, 200, 0);
  ob::toStraight(black);
  CHECK(black.uyvy[0] == 16);   // colour over black
  CHECK(black.uyvy[1] == 128);
}

TEST_CASE("toStraight divides by alpha and clamps") {
  auto mid = make(126, 190, 70, 128);  // Y = 16 + 110 / 0.502 ~= 235
  ob::toStraight(mid);
  CHECK(mid.uyvy[0] >= 233);
  CHECK(mid.uyvy[0] <= 235);
  CHECK(mid.uyvy[1] == 240);           // 251.5 clamps to the chroma legal maximum

  auto low = make(20, 130, 130, 10);   // Y = 16 + 4 / 0.039 ~= 118
  ob::toStraight(low);
  CHECK(low.uyvy[0] > 100);
  CHECK(low.uyvy[0] < 140);
}
