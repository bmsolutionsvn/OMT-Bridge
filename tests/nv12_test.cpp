#include <doctest/doctest.h>

#include <vector>

#include "core/nv12.h"

TEST_CASE("full-range NV12 maps to limited range by the table") {
  // 4x2: Y 0->16 and 255->235; C 0->16, 128->128, 255->240.
  const std::vector<uint8_t> full = {0, 255, 0, 255, 0, 255, 0, 255, 0, 128, 255, 64};
  std::vector<uint8_t> out;
  ob::nv12FullToLimited(full, 4, 2, out);
  REQUIRE(out.size() == 12u);
  const std::vector<uint8_t> want = {16, 235, 16, 235, 16, 235, 16, 235, 16, 128, 240, 72};
  CHECK(out == want);
}

TEST_CASE("a 4x2 picture converts plane by plane") {
  std::vector<uint8_t> full(12);
  for (size_t i = 0; i < 8; ++i) full[i] = static_cast<uint8_t>(i * 32);
  full[8] = 0;
  full[9] = 64;
  full[10] = 128;
  full[11] = 192;
  std::vector<uint8_t> out;
  ob::nv12FullToLimited(full, 4, 2, out);
  REQUIRE(out.size() == full.size());
  for (size_t i = 0; i < 8; ++i) {
    CHECK(out[i] == static_cast<uint8_t>(16 + (static_cast<int>(i * 32) * 219 + 127) / 255));
  }
  CHECK(out[8] == 16);
  CHECK(out[11] == static_cast<uint8_t>(128 + (64 * 224 + 127) / 255));
}