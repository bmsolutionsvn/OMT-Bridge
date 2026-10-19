#include <doctest/doctest.h>

#include "core/frame.h"
#include "omthx/omthx.h"

TEST_CASE("a frame reports its plane sizes") {
  ob::Frame f;
  f.format.width = 1920;
  f.format.height = 1080;
  f.format.alpha = false;
  CHECK(f.lumaBytes() == 1920u * 2u * 1080u);
  CHECK(f.alphaBytes() == 0u);

  f.format.alpha = true;
  CHECK(f.alphaBytes() == 1920u * 1080u);
}

TEST_CASE("alphaOffset points past the luma plane and is null when there is no alpha") {
  ob::Frame f;
  f.format = ob::VideoFormat{4, 2, 60, 1, true};
  CHECK(f.alphaOffset() == 4u * 2u * 2u);
  f.format.alpha = false;
  CHECK(f.alphaOffset() == ob::Frame::kNoAlpha);
}

TEST_CASE("valid() accepts an UYVY picture of the right size and rejects others") {
  ob::Frame f;
  f.format = ob::VideoFormat{4, 2, 60, 1, false};
  f.uyvy.assign(f.lumaBytes(), 0x10);
  CHECK(f.valid());

  f.uyvy.assign(f.lumaBytes() - 1, 0x10);
  CHECK_FALSE(f.valid());

  f.format = ob::VideoFormat{3, 2, 60, 1, false};
  f.uyvy.assign(f.lumaBytes(), 0x10);  // right size, odd width
  CHECK_FALSE(f.valid());
}

TEST_CASE("valid() accepts an alpha frame and rejects a degenerate height") {
  ob::Frame f;
  f.format = ob::VideoFormat{4, 2, 60, 1, true};
  f.uyvy.assign(f.lumaBytes() + f.alphaBytes(), 0x10);
  CHECK(f.valid());

  f.format = ob::VideoFormat{4, 1, 60, 1, false};
  f.uyvy.assign(f.lumaBytes(), 0x10);
  CHECK_FALSE(f.valid());
}

TEST_CASE("the linked omthx exports the UYVY API") {
  // The pinned OMT-HX (ae96aa1) contains sendVideoUyvy and PixelFormat::Coded. Spelling the
  // signature pins it, and the pointer-to-member forces the linker to resolve the symbol.
  static_assert(static_cast<int>(omthx::PixelFormat::Coded) >= 0);
  using SendUyvy = void (omthx::Sender::*)(const omthx::VideoFormat&, const uint8_t*, int,
                                           const uint8_t*, int, bool, int64_t);
  const SendUyvy fn = &omthx::Sender::sendVideoUyvy;
  CHECK(fn != nullptr);
}
