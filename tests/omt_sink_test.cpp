#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <omthx/omthx.h>

#include "engine/omt_sink.h"
#include "engine/omt_source.h"

using namespace std::chrono_literals;

namespace {
class TestSink : public ob::OmtSink {
 public:
  struct Sent { int w, h; bool alpha, premultiplied; int64_t ts; };
  std::vector<Sent> sent;
  struct AudioSent { int rate, channels, floats, ts; };
  std::vector<AudioSent> audio;
  struct CodedSent { int w, h; bool alpha, keyframe; int64_t ts; };
  std::vector<CodedSent> coded;
  int opens = 0;
 protected:
  void openLocked() override { ++opens; }  // no network sender in a unit test
  void send(const ob::VideoFormat& f, const uint8_t*, bool alpha, bool premultiplied,
            int64_t ts) override {
    sent.push_back({f.width, f.height, alpha, premultiplied, ts});
  }
  void sendCoded(const omthx::CodedFrame& f) override {
    coded.push_back({f.format.width, f.format.height, f.format.alpha, f.keyframe, f.timestamp});
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
  const auto stats = sink.stats();  // no sender: zero counters, not a crash
  CHECK(stats.bytesSent == 0u);
  CHECK(stats.connections == 0);
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

static std::shared_ptr<ob::Frame> codedFrame(omthx::Mode mode, bool alpha, bool picture = false) {
  auto c = std::make_shared<omthx::CodedFrame>();
  c->mode = mode;
  c->format = omthx::VideoFormat{4, 2, 60, 1, alpha};
  c->data.assign(16, 0x40);
  c->timestamp = 42;
  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{4, 2, 60, 1, alpha};
  f->timestamp = 42;
  if (picture) f->uyvy.assign(f->lumaBytes() + f->alphaBytes(), 0x80);
  f->coded = c;
  return f;
}

TEST_CASE("a coded HX frame passes through an OMT-HX output") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::Hevc, ob::AlphaMode::Auto), "B (A)");
  sink.takeKeyframeRequest();  // the request a new output makes

  sink.deliver(codedFrame(omthx::Mode::Hx, false));
  REQUIRE(sink.coded.size() == 1u);
  CHECK(sink.coded[0].w == 4);
  CHECK(sink.coded[0].ts == 42);
  CHECK(sink.sent.empty());
  CHECK(sink.note() == "passed through");
}

TEST_CASE("the same coded frame to another family is re-encoded from its picture") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::Omt, ob::HxCodec::Hevc, ob::AlphaMode::Auto), "B (A)");
  sink.deliver(codedFrame(omthx::Mode::Hx, false, true));
  CHECK(sink.coded.empty());
  REQUIRE(sink.sent.size() == 1u);
}

TEST_CASE("a coded HX frame with alpha does not pass through an output that drops alpha") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::Hevc, ob::AlphaMode::Off), "B (A)");
  sink.deliver(codedFrame(omthx::Mode::Hx, true, true));
  CHECK(sink.coded.empty());
  REQUIRE(sink.sent.size() == 1u);
  CHECK_FALSE(sink.sent[0].alpha);
}

TEST_CASE("a coded-only frame to another family is dropped, not indexed") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::Omt, ob::HxCodec::Hevc, ob::AlphaMode::Auto), "B (A)");
  CHECK_NOTHROW(sink.deliver(codedFrame(omthx::Mode::Hx, false)));  // no picture to re-encode
  CHECK(sink.coded.empty());
  CHECK(sink.sent.empty());
}

TEST_CASE("a new output asks for a keyframe once") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::Hevc, ob::AlphaMode::Auto), "B (A)");
  CHECK(sink.takeKeyframeRequest());
  CHECK_FALSE(sink.takeKeyframeRequest());
}

namespace {
bool waitFor(const std::function<bool()>& done, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!done() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return done();
}
}  // namespace

TEST_CASE("an HX source passes through an OMT-HX output with no decode and no encode") {
  const omthx::VideoFormat f{640, 360, 60, 1, false};
  omthx::Sender source("ob-test-pass-in", omthx::Mode::Hx, omthx::Quality::Medium,
                       omthx::Codec::Hevc);

  struct ProbeSink : ob::OmtSink {  // the real sink, with its sender's address for the receiver
    std::string address() const { return senderAddress(); }
  };
  ProbeSink out;
  out.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::Hevc, ob::AlphaMode::Auto),
                "ob-test-pass-out");

  // The source hands its coded frames to the output, as the graph would; the output's keyframe
  // requests go back to the source the same way.
  ob::OmtSource in(
      source.address(),
      [&](std::shared_ptr<ob::Frame> fr) {
        out.deliver(fr);
        return out.takeKeyframeRequest();
      },
      [](ob::SignalState, std::optional<ob::ErrorInfo>) {},
      [](std::shared_ptr<ob::AudioFrame>) {},
      [](bool, bool) { return false; });

  std::atomic<bool> stop{false};
  std::thread pump([&] {
    std::vector<uint8_t> uyvy(static_cast<size_t>(f.width) * f.height * 2, 0x80);
    auto next = std::chrono::steady_clock::now();
    for (int i = 0; !stop.load(); ++i) {
      try {
        source.sendVideoUyvy(f, uyvy.data(), f.width * 2, nullptr, f.width, false,
                             1'000'000 + static_cast<int64_t>(i) * 166'667);
      } catch (const std::exception&) {
        return;  // no hardware HEVC encoder
      }
      next += std::chrono::milliseconds(16);
      std::this_thread::sleep_until(next);
    }
  });
  in.start();

  const bool encoding = waitFor([&] { return !source.stats().encoder.empty(); }, 3s);
  const std::string address = out.address();
  if (!encoding || address.empty()) {
    if (!encoding) MESSAGE("SKIPPED, no hardware HEVC encoder on this PC");
    else MESSAGE("SKIPPED, the output's sender did not open");
    in.stop();
    stop.store(true);
    pump.join();
    return;
  }

  // A receiver joins after the stream is running: its keyframe request must travel back.
  omthx::Receiver downstream(address, omthx::PixelFormat::Uyvy);
  omthx::VideoFrame picture;
  bool got = false;
  for (const auto deadline = std::chrono::steady_clock::now() + 3s;
       !got && std::chrono::steady_clock::now() < deadline;)
    got = downstream.receiveVideo(picture, 100);
  in.stop();
  stop.store(true);
  pump.join();

  CHECK(got);
  CHECK(picture.format.width == 640);
  CHECK(picture.format.height == 360);
  CHECK(in.framesDecoded() == 0);              // the source never decoded
  CHECK(downstream.stats().encoder == source.stats().encoder);  // the source's own encoder, not a new one
}

TEST_CASE("an output that changes lane reopens its sender and asks for a keyframe") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::Hevc, ob::AlphaMode::Off), "B (A)");
  sink.takeKeyframeRequest();  // the request a new output makes
  const int opens = sink.opens;

  sink.deliver(codedFrame(omthx::Mode::Hx, false, true));  // passes through: the coded lane
  CHECK(sink.coded.size() == 1u);
  CHECK(sink.opens == opens);  // same lane, same sender

  // The source's alpha appears, and this output drops it: from now on it re-encodes pictures.
  // A sender must not mix coded frames and pictures, so it is reopened and a keyframe asked for.
  sink.deliver(codedFrame(omthx::Mode::Hx, true, true));
  CHECK(sink.sent.size() == 1u);
  CHECK(sink.opens == opens + 1);
  CHECK(sink.takeKeyframeRequest());
}
