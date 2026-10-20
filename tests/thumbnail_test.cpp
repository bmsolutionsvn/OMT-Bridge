#include <doctest/doctest.h>

#include "core/thumbnail.h"

namespace {

ob::Frame frameOf(int width, int height, uint8_t y, uint8_t u, uint8_t v) {
  ob::Frame f;
  f.format = ob::VideoFormat{width, height, 60, 1, false};
  f.uyvy.assign(f.lumaBytes(), 0);
  for (size_t i = 0; i < f.uyvy.size(); i += 4) {
    f.uyvy[i + 0] = u;
    f.uyvy[i + 1] = y;
    f.uyvy[i + 2] = v;
    f.uyvy[i + 3] = y;
  }
  return f;
}

}  // namespace

TEST_CASE("a thumbnail is a BGRA box downscale") {
  const auto white = ob::makeThumbnail(frameOf(4, 2, 235, 128, 128), 4);
  CHECK(white.width == 4);
  CHECK(white.height == 2);
  REQUIRE(white.bgra.size() == 4u * 2u * 4u);
  CHECK(white.bgra[0] >= 230);  // B
  CHECK(white.bgra[1] >= 230);  // G
  CHECK(white.bgra[2] >= 230);  // R
  CHECK(white.bgra[3] == 255);  // A
}

TEST_CASE("limited-range black stays black") {
  const auto black = ob::makeThumbnail(frameOf(4, 2, 16, 128, 128), 4);
  REQUIRE(black.bgra.size() == 4u * 2u * 4u);
  CHECK(black.bgra[0] <= 2);
  CHECK(black.bgra[1] <= 2);
  CHECK(black.bgra[2] <= 2);
}

TEST_CASE("the box average mixes both halves instead of sampling one") {
  auto f = frameOf(4, 2, 16, 128, 128);
  // The last pixel of each row is white; the right output pixel averages it with the black one.
  for (size_t row = 0; row < 2; ++row) {
    const size_t pair = 1;  // source pixels 2 and 3
    const size_t i = row * 8 + pair * 4;
    f.uyvy[i + 1] = 235;
    f.uyvy[i + 3] = 235;
  }
  const auto t = ob::makeThumbnail(f, 2);  // each output pixel covers a 2x1 box
  REQUIRE(t.width == 2);
  REQUIRE(t.height == 2);
  CHECK(t.bgra[2] <= 2);                  // left: black
  CHECK(t.bgra[(2 + 1) * 4 + 2] >= 100);  // right: half white is mid grey
}

TEST_CASE("a portrait source keeps its long edge at the target") {
  const auto t = ob::makeThumbnail(frameOf(4, 8, 235, 128, 128), 4);
  CHECK(t.width == 2);
  CHECK(t.height == 4);
}

TEST_CASE("an invalid frame produces an empty thumbnail") {
  ob::Frame f;
  f.format = ob::VideoFormat{4, 2, 60, 1, false};
  f.uyvy.assign(3, 0);
  CHECK(ob::makeThumbnail(f, 4).bgra.empty());
  CHECK(ob::makeThumbnail(frameOf(4, 2, 235, 128, 128), 0).bgra.empty());
}
