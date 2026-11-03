#include <doctest/doctest.h>

#include <memory>
#include <thread>

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

TEST_CASE("a frame with coded data but no picture is not valid") {
  auto coded = std::make_shared<omthx::CodedFrame>();
  coded->format = omthx::VideoFormat{4, 2, 60, 1, false};
  coded->data = {1, 2, 3};
  ob::Frame f;
  f.format = ob::VideoFormat{4, 2, 60, 1, false};
  f.coded = coded;
  CHECK(f.codedOnly());
  CHECK_FALSE(f.valid());  // no UYVY picture to hand a picture consumer

  f.uyvy.assign(f.lumaBytes(), 0x10);
  CHECK_FALSE(f.codedOnly());
  CHECK(f.valid());
}

TEST_CASE("an NV12 frame is valid with the right bytes and no alpha") {
  ob::Frame f;
  f.layout = ob::Layout::Nv12;
  f.format = ob::VideoFormat{4, 2, 60, 1, false};
  f.nv12.assign(4u * 2u * 3u / 2u, 0x80);
  CHECK(f.valid());

  f.nv12.assign(4u * 2u * 3u / 2u - 1u, 0x80);
  CHECK_FALSE(f.valid());

  f.nv12.assign(3u * 2u * 3u / 2u, 0x80);
  f.format = ob::VideoFormat{3, 2, 60, 1, false};  // odd width
  CHECK_FALSE(f.valid());

  f.format = ob::VideoFormat{4, 1, 60, 1, false};  // height 1
  f.nv12.assign(4u * 1u * 3u / 2u, 0x80);
  CHECK_FALSE(f.valid());

  f.format = ob::VideoFormat{4, 3, 60, 1, false};  // odd height: chroma rows come in pairs
  f.nv12.assign(4u * 3u * 3u / 2u, 0x80);
  CHECK_FALSE(f.valid());

  f.format = ob::VideoFormat{4, 2, 60, 1, true};  // NV12 carries no alpha
  f.nv12.assign(4u * 2u * 3u / 2u, 0x80);
  CHECK_FALSE(f.valid());
}

TEST_CASE("limitedNv12 makes the limited picture once, also from two threads") {
  ob::Frame f;
  f.layout = ob::Layout::Nv12;
  f.format = ob::VideoFormat{4, 2, 60, 1, false};
  f.nv12.assign(4u * 2u * 3u / 2u, 128);
  const auto first = f.limitedNv12();
  REQUIRE(first.get() != nullptr);
  const bool once = f.limitedNv12() == first;
  CHECK(once);
  CHECK((*first)[0] == 16 + static_cast<uint8_t>((128 * 219 + 127) / 255));

  std::shared_ptr<const std::vector<uint8_t>> a, b;
  std::thread ta([&] { a = f.limitedNv12(); });
  std::thread tb([&] { b = f.limitedNv12(); });
  ta.join();
  tb.join();
  const bool shared = a == first;
  const bool both = a == b;
  CHECK(shared);
  CHECK(both);

  ob::Frame uyvy;
  uyvy.format = ob::VideoFormat{4, 2, 60, 1, false};
  const bool none = uyvy.limitedNv12() == nullptr;
  CHECK(none);
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
