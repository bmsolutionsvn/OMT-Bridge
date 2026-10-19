#include <doctest/doctest.h>

#include <memory>
#include <vector>

#include "engine/omt_sink.h"

namespace {
class TestSink : public ob::OmtSink {
 public:
  struct Sent { int w, h; bool alpha, premultiplied; int64_t ts; };
  std::vector<Sent> sent;
 protected:
  void openLocked() override {}  // no network sender in a unit test
  void send(const ob::VideoFormat& f, const uint8_t*, bool alpha, bool premultiplied,
            int64_t ts) override {
    sent.push_back({f.width, f.height, alpha, premultiplied, ts});
  }
};
}

static ob::Output output(ob::OutputTarget target, ob::HxCodec codec, ob::AlphaMode alpha) {
  ob::Output o;
  o.id = "o1";
  o.target = target;
  o.codec = codec;
  o.alpha = alpha;
  return o;
}

TEST_CASE("alpha follows the source under Auto, and is dropped under Off") {
  TestSink autoSink;
  autoSink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::Hevc, ob::AlphaMode::Auto), "B (A)");
  auto frame = std::make_shared<ob::Frame>();
  frame->format = ob::VideoFormat{4, 2, 60, 1, true};
  frame->uyvy.assign(frame->lumaBytes() + frame->alphaBytes(), 0x80);
  autoSink.deliver(frame);
  REQUIRE(autoSink.sent.size() == 1u);
  CHECK(autoSink.sent[0].alpha);

  TestSink offSink;
  offSink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::Hevc, ob::AlphaMode::Off), "B (A)");
  offSink.deliver(frame);
  REQUIRE(offSink.sent.size() == 1u);
  CHECK_FALSE(offSink.sent[0].alpha);
}

TEST_CASE("H.264 above 1080p drops alpha and says why") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::H264, ob::AlphaMode::Auto), "B (A)");
  auto frame = std::make_shared<ob::Frame>();
  frame->format = ob::VideoFormat{1920, 2160, 60, 1, true};
  frame->uyvy.assign(frame->lumaBytes() + frame->alphaBytes(), 0x80);
  sink.deliver(frame);
  REQUIRE(sink.sent.size() == 1u);
  CHECK_FALSE(sink.sent[0].alpha);
  CHECK(sink.note() == "alpha dropped — H.264 above 1080p, use HEVC");
}

TEST_CASE("an OMT-HX output of 1080p or less keeps H.264 alpha") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::H264, ob::AlphaMode::Auto), "B (A)");
  auto frame = std::make_shared<ob::Frame>();
  frame->format = ob::VideoFormat{1920, 1080, 60, 1, true};
  frame->uyvy.assign(frame->lumaBytes() + frame->alphaBytes(), 0x80);
  sink.deliver(frame);
  REQUIRE(sink.sent.size() == 1u);
  CHECK(sink.sent[0].alpha);
}
