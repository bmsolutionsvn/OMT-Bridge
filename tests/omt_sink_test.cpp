#include <doctest/doctest.h>

#include <memory>
#include <span>
#include <vector>

#include "engine/omt_sink.h"

namespace {
class TestSink : public ob::OmtSink {
 public:
  struct Sent { int w, h; bool alpha, premultiplied; int64_t ts; };
  std::vector<Sent> sent;
  struct AudioSent { int rate, channels, floats, ts; };
  std::vector<AudioSent> audio;
  int opens = 0;
 protected:
  void openLocked() override { ++opens; }  // no network sender in a unit test
  void send(const ob::VideoFormat& f, const uint8_t*, bool alpha, bool premultiplied,
            int64_t ts) override {
    sent.push_back({f.width, f.height, alpha, premultiplied, ts});
  }
  void sendAudio(std::span<const float> planar, int rate, int channels, int64_t ts) override {
    audio.push_back({rate, channels, static_cast<int>(planar.size()), static_cast<int>(ts)});
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

TEST_CASE("alpha: Off drops alpha and says so") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::Hevc, ob::AlphaMode::Off), "B (A)");
  auto frame = std::make_shared<ob::Frame>();
  frame->format = ob::VideoFormat{4, 2, 60, 1, true};
  frame->uyvy.assign(frame->lumaBytes() + frame->alphaBytes(), 0x80);
  sink.deliver(frame);
  REQUIRE(sink.sent.size() == 1u);
  CHECK_FALSE(sink.sent[0].alpha);
  CHECK(sink.note() == "alpha dropped");
}

TEST_CASE("the H.264 label clears once the frame fits") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::H264, ob::AlphaMode::Auto), "B (A)");
  auto big = std::make_shared<ob::Frame>();
  big->format = ob::VideoFormat{1920, 2160, 60, 1, true};
  big->uyvy.assign(big->lumaBytes() + big->alphaBytes(), 0x80);
  sink.deliver(big);
  REQUIRE(sink.note() == "alpha dropped — H.264 above 1080p, use HEVC");

  auto small = std::make_shared<ob::Frame>();
  small->format = ob::VideoFormat{1920, 1080, 60, 1, true};
  small->uyvy.assign(small->lumaBytes() + small->alphaBytes(), 0x80);
  sink.deliver(small);
  REQUIRE(sink.sent.size() == 2u);
  CHECK(sink.sent[1].alpha);
  CHECK(sink.note().empty());
}

TEST_CASE("a Full OMT output ignores the H.264 resolution rule") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::Omt, ob::HxCodec::H264, ob::AlphaMode::Auto), "B (A)");
  auto frame = std::make_shared<ob::Frame>();
  frame->format = ob::VideoFormat{1920, 2160, 60, 1, true};
  frame->uyvy.assign(frame->lumaBytes() + frame->alphaBytes(), 0x80);
  sink.deliver(frame);
  REQUIRE(sink.sent.size() == 1u);
  CHECK(sink.sent[0].alpha);
}

TEST_CASE("premultiplied passes through untouched") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::Hevc, ob::AlphaMode::Auto), "B (A)");
  auto frame = std::make_shared<ob::Frame>();
  frame->format = ob::VideoFormat{4, 2, 60, 1, true};
  frame->premultiplied = true;
  frame->uyvy.assign(frame->lumaBytes() + frame->alphaBytes(), 0x80);
  sink.deliver(frame);
  REQUIRE(sink.sent.size() == 1u);
  CHECK(sink.sent[0].premultiplied);

  auto straight = std::make_shared<ob::Frame>();
  straight->format = ob::VideoFormat{4, 2, 60, 1, true};
  straight->premultiplied = false;
  straight->uyvy.assign(straight->lumaBytes() + straight->alphaBytes(), 0x80);
  sink.deliver(straight);
  REQUIRE(sink.sent.size() == 2u);
  CHECK_FALSE(sink.sent[1].premultiplied);
}

TEST_CASE("a malformed frame is never sent") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::Hevc, ob::AlphaMode::Auto), "B (A)");
  auto frame = std::make_shared<ob::Frame>();
  frame->format = ob::VideoFormat{4, 2, 60, 1, true};
  frame->uyvy.assign(7, 0x80);  // too short to be valid
  sink.deliver(frame);
  CHECK(sink.sent.empty());
}

TEST_CASE("an NDI output keeps its M3 note even with alpha: Off") {
  ob::OmtSink sink;  // configure returns before constructing a sender for NDI
  sink.configure(output(ob::OutputTarget::Ndi, ob::HxCodec::Hevc, ob::AlphaMode::Off), "B (A)");
  auto frame = std::make_shared<ob::Frame>();
  frame->format = ob::VideoFormat{4, 2, 60, 1, true};
  frame->uyvy.assign(frame->lumaBytes() + frame->alphaBytes(), 0x80);
  sink.deliver(frame);
  CHECK(sink.note() == "NDI output needs the NDI adapter (M3)");
}

TEST_CASE("audio keeps the source's rate, channels and total float count") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::Hevc, ob::AlphaMode::Auto), "B (A)");

  auto a = std::make_shared<ob::AudioFrame>();
  a->format = {44100, 6};
  a->samples = 5;
  a->planar.assign(30, 0.25f);
  a->timestamp = 99;
  sink.deliverAudio(a);

  REQUIRE(sink.audio.size() == 1u);
  CHECK(sink.audio[0].rate == 44100);
  CHECK(sink.audio[0].channels == 6);
  CHECK(sink.audio[0].floats == 30);
  CHECK(sink.audio[0].ts == 99);

  auto bad = std::make_shared<ob::AudioFrame>();
  bad->format = {48000, 2};
  bad->samples = 4;
  bad->planar.assign(7, 0.0f);  // one short: invalid
  sink.deliverAudio(bad);
  CHECK(sink.audio.size() == 1u);
}
