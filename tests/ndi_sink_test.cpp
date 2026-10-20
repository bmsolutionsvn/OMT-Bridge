#ifdef OB_WITH_NDI
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/ndi_sink.h"

namespace {
class TestNdiSink : public ob::NdiSink {
 public:
  struct Sent { int w, h; bool alpha, premultiplied; uint8_t y0; };
  std::vector<Sent> sent;
 protected:
  void openLocked() override {}  // no network sender in a unit test
  void send(const ob::Frame& f, bool alpha) override {
    sent.push_back({f.format.width, f.format.height, alpha, f.premultiplied, f.uyvy[1]});
  }
};
}

static ob::Output ndiOutput(ob::AlphaMode alpha) {
  ob::Output o;
  o.id = "o1";
  o.target = ob::OutputTarget::Ndi;
  o.alpha = alpha;
  return o;
}

TEST_CASE("NDI sends a premultiplied frame as straight") {
  TestNdiSink sink;
  sink.configure(ndiOutput(ob::AlphaMode::Auto), "Bridge (A)");
  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{4, 2, 60, 1, true};
  f->premultiplied = true;
  f->uyvy.assign(f->lumaBytes() + f->alphaBytes(), 0);
  f->uyvy[1] = 126;  // Y0, canonical UYVY order
  f->uyvy[0] = 190;
  f->uyvy[2] = 70;
  f->uyvy[3] = 126;
  for (size_t i = 0; i < f->alphaBytes(); ++i) f->uyvy[f->lumaBytes() + i] = 128;
  sink.deliver(f);
  REQUIRE(sink.sent.size() == 1u);
  CHECK(sink.sent[0].alpha);
  CHECK_FALSE(sink.sent[0].premultiplied);
  CHECK(sink.sent[0].y0 >= 233);   // 16 + 110 / 0.502
  CHECK(sink.sent[0].y0 <= 235);
  CHECK(sink.note().empty());  // no M2 placeholder note on a working NDI sink
}

TEST_CASE("alpha: Off sends no alpha plane") {
  TestNdiSink sink;
  sink.configure(ndiOutput(ob::AlphaMode::Off), "Bridge (A)");
  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{4, 2, 60, 1, true};
  f->uyvy.assign(f->lumaBytes() + f->alphaBytes(), 0x80);
  sink.deliver(f);
  REQUIRE(sink.sent.size() == 1u);
  CHECK_FALSE(sink.sent[0].alpha);
}

TEST_CASE("a malformed frame is never sent") {
  TestNdiSink sink;
  sink.configure(ndiOutput(ob::AlphaMode::Auto), "Bridge (A)");
  const auto stats = sink.stats();  // the test seam opens no sender
  CHECK(stats.bytesSent == 0u);
  CHECK(stats.connections == 0);
  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{4, 2, 60, 1, true};
  f->uyvy.assign(7, 0);  // too short to be valid
  sink.deliver(f);
  CHECK(sink.sent.empty());
}
#endif