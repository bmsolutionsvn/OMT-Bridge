#include <doctest/doctest.h>

#include "core/alpha_convert.h"

// Canonical UYVY: each pair is [U][Y0][V][Y1].
static ob::Frame make(int y, int u, int v, int a) {
  ob::Frame f;
  f.format = ob::VideoFormat{2, 2, 60, 1, true};
  f.premultiplied = true;
  f.uyvy.assign(f.lumaBytes() + f.alphaBytes(), 0);
  for (size_t row = 0; row < 2; ++row) {
    uint8_t* p = f.uyvy.data() + row * 4;
    p[0] = static_cast<uint8_t>(u);
    p[1] = static_cast<uint8_t>(y);
    p[2] = static_cast<uint8_t>(v);
    p[3] = static_cast<uint8_t>(y);
  }
  for (size_t i = 0; i < f.alphaBytes(); ++i) f.uyvy[f.lumaBytes() + i] = static_cast<uint8_t>(a);
  return f;
}

TEST_CASE("toStraight leaves opaque and empty alpha unchanged") {
  auto opaque = make(100, 120, 130, 255);
  ob::toStraight(opaque);
  CHECK(opaque.uyvy[0] == 120);   // U unchanged
  CHECK(opaque.uyvy[1] == 100);   // Y unchanged
  CHECK_FALSE(opaque.premultiplied);

  auto black = make(200, 200, 200, 0);
  ob::toStraight(black);
  CHECK(black.uyvy[1] == 16);     // colour over black
  CHECK(black.uyvy[0] == 128);
}

TEST_CASE("toStraight divides by alpha and clamps") {
  auto mid = make(126, 190, 70, 128);  // Y = 16 + 110 / 0.502 ~= 235
  ob::toStraight(mid);
  CHECK(mid.uyvy[1] >= 233);
  CHECK(mid.uyvy[1] <= 235);
  CHECK(mid.uyvy[0] == 240);           // 251.5 clamps to the chroma legal maximum

  auto low = make(20, 130, 130, 10);   // Y = 16 + 4 / 0.039 ~= 118
  ob::toStraight(low);
  CHECK(low.uyvy[1] > 100);
  CHECK(low.uyvy[1] < 140);
}

TEST_CASE("toStraight uses each pair's mean alpha and keeps the alpha plane") {
  ob::Frame f;
  f.format = ob::VideoFormat{4, 2, 60, 1, true};
  f.timestamp = 1234;
  f.premultiplied = true;
  f.uyvy.assign(f.lumaBytes() + f.alphaBytes(), 0);

  // Row 0: pair A alphas 255,128; pair B alphas 0,0.
  uint8_t* r0 = f.uyvy.data();
  r0[0] = 200; r0[1] = 126; r0[2] = 70; r0[3] = 126;
  r0[4] = 200; r0[5] = 100; r0[6] = 70; r0[7] = 100;
  // Row 1: pair A alphas 0,255; pair B alphas 254,1.
  uint8_t* r1 = f.uyvy.data() + 8;
  r1[0] = 180; r1[1] = 100; r1[2] = 160; r1[3] = 100;
  r1[4] = 60;  r1[5] = 200; r1[6] = 250; r1[7] = 200;

  const uint8_t alphas[8] = {255, 128, 0, 0, 0, 255, 254, 1};
  for (int i = 0; i < 8; ++i) f.uyvy[f.lumaBytes() + i] = alphas[i];

  ob::toStraight(f);

  CHECK(f.uyvy[0] == 224);   // U by the pair mean
  CHECK(f.uyvy[1] == 126);   // a = 255
  CHECK(f.uyvy[2] == 51);    // V by the pair mean
  CHECK(f.uyvy[3] == 235);   // a = 128
  CHECK(f.uyvy[4] == 128);   // both alphas 0
  CHECK(f.uyvy[5] == 16);
  CHECK(f.uyvy[6] == 128);
  CHECK(f.uyvy[7] == 16);

  CHECK(f.uyvy[8] == 232);   // U, a = 0 and 255 -> mean 0.5
  CHECK(f.uyvy[9] == 16);    // a = 0
  CHECK(f.uyvy[10] == 192);
  CHECK(f.uyvy[11] == 100);  // a = 255
  CHECK(f.uyvy[12] == 16);   // clamps low
  CHECK(f.uyvy[13] == 201);  // a = 254
  CHECK(f.uyvy[14] == 240);  // clamps high
  CHECK(f.uyvy[15] == 235);  // a = 1

  for (int i = 0; i < 8; ++i) CHECK(f.uyvy[f.lumaBytes() + i] == alphas[i]);
  CHECK(f.timestamp == 1234);
  CHECK_FALSE(f.premultiplied);
}

TEST_CASE("toStraight leaves a frame without alpha, or an invalid frame, untouched") {
  auto noAlpha = make(100, 120, 130, 255);
  noAlpha.format.alpha = false;
  noAlpha.uyvy.resize(noAlpha.lumaBytes());  // valid, but there is no alpha plane
  ob::toStraight(noAlpha);
  CHECK(noAlpha.uyvy[1] == 100);
  CHECK(noAlpha.uyvy[0] == 120);
  CHECK(noAlpha.premultiplied);

  auto shortFrame = make(100, 120, 130, 255);
  shortFrame.uyvy.pop_back();
  ob::toStraight(shortFrame);
  CHECK(shortFrame.uyvy[1] == 100);
  CHECK(shortFrame.premultiplied);
}
