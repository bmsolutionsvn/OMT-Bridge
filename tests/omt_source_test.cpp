#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <omthx/omthx.h>

#include "engine/omt_source.h"

using namespace std::chrono_literals;

namespace {
constexpr int64_t kFrame = 166'667;  // one frame at 60 fps, in OMT units

// Sends a flat picture at 60 fps on its own thread until destroyed. A PC without the encoder the
// sender needs sets failed(): the tests then skip instead of failing on the machine, as the OMT-HX
// hardware tests do.
class Pump {
 public:
  Pump(omthx::Sender& sender, const omthx::VideoFormat& f)
      : thread_([this, &sender, f] {
          std::vector<uint8_t> uyvy(static_cast<size_t>(f.width) * f.height * (f.alpha ? 3 : 2), 0x80);
          const uint8_t* alpha = f.alpha ? uyvy.data() + static_cast<size_t>(f.width) * 2 * f.height : nullptr;
          auto next = std::chrono::steady_clock::now();
          for (int i = 0; !stop_.load(); ++i) {
            try {
              sender.sendVideoUyvy(f, uyvy.data(), f.width * 2, alpha, f.width, false,
                                   1'000'000 + i * kFrame);
            } catch (const std::exception&) {
              failed_.store(true);
              return;
            }
            next += 16ms;
            std::this_thread::sleep_until(next);
          }
        }) {}
  ~Pump() {
    stop_.store(true);
    thread_.join();
  }
  bool failed() const { return failed_.load(); }

 private:
  std::atomic<bool> stop_{false};
  std::atomic<bool> failed_{false};
  std::thread thread_;
};

bool waitFor(const std::function<bool()>& done, std::chrono::milliseconds limit) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!done() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
  return done();
}
}  // namespace

TEST_CASE("an OmtSource with only pass-through demand never decodes") {
  const omthx::VideoFormat f{640, 360, 60, 1, false};
  omthx::Sender sender("ob-test-source-idle", omthx::Mode::Full, omthx::Quality::Medium);
  std::atomic<int> coded{0};
  std::atomic<int> pictures{0};
  ob::OmtSource source(
      sender.address(),
      [&](std::shared_ptr<ob::Frame> fr) {
        if (fr->codedOnly()) ++coded;
        if (fr->valid()) ++pictures;
        return false;
      },
      [](ob::SignalState, std::optional<ob::ErrorInfo>) {},
      [](std::shared_ptr<ob::AudioFrame>) {},
      [](bool, bool) { return false; });
  source.start();
  Pump pump(sender, f);
  CHECK(waitFor([&] { return coded.load() >= 10; }, 3s));
  source.stop();
  CHECK(pictures.load() == 0);
  CHECK(source.framesDecoded() == 0);
}

TEST_CASE("an OmtSource decodes once a picture is wanted") {
  const omthx::VideoFormat f{640, 360, 60, 1, false};
  omthx::Sender sender("ob-test-source-demand", omthx::Mode::Full, omthx::Quality::Medium);
  std::atomic<bool> wanted{false};
  std::atomic<int> pictures{0};
  ob::OmtSource source(
      sender.address(),
      [&](std::shared_ptr<ob::Frame> fr) {
        if (fr->valid()) ++pictures;
        return false;
      },
      [](ob::SignalState, std::optional<ob::ErrorInfo>) {},
      [](std::shared_ptr<ob::AudioFrame>) {},
      [&](bool, bool) { return wanted.load(); });
  source.start();
  Pump pump(sender, f);
  CHECK(waitFor([&] { return sender.stats().framesSent >= 10; }, 3s));
  CHECK(source.framesDecoded() == 0);

  wanted.store(true);
  CHECK(waitFor([&] { return pictures.load() > 0; }, 3s));
  source.stop();
  CHECK(source.framesDecoded() > 0);
}

TEST_CASE("an HX OmtSource decodes from a keyframe once a picture is wanted") {
  const omthx::VideoFormat f{640, 360, 60, 1, false};
  omthx::Sender sender("ob-test-source-demand-hx", omthx::Mode::Hx, omthx::Quality::Medium,
                       omthx::Codec::Hevc);
  std::atomic<bool> wanted{false};
  std::atomic<int> pictures{0};
  ob::OmtSource source(
      sender.address(),
      [&](std::shared_ptr<ob::Frame> fr) {
        if (fr->valid()) ++pictures;
        return false;
      },
      [](ob::SignalState, std::optional<ob::ErrorInfo>) {},
      [](std::shared_ptr<ob::AudioFrame>) {},
      [&](bool hx, bool alpha) {
        CHECK(hx);
        CHECK_FALSE(alpha);
        return wanted.load();
      });
  source.start();
  Pump pump(sender, f);
  const bool encoding = waitFor([&] { return !sender.stats().encoder.empty(); }, 3s);
  if (pump.failed() || !encoding) {
    MESSAGE("SKIPPED, no hardware HEVC encoder on this PC");
    source.stop();
    return;
  }
  CHECK(source.framesDecoded() == 0);

  const uint64_t before = sender.stats().keyframes;
  wanted.store(true);
  CHECK(waitFor([&] { return pictures.load() > 0; }, 3s));
  source.stop();
  CHECK(source.framesDecoded() > 0);
  CHECK(sender.stats().keyframes > before);  // the decoder restarted at a keyframe
}

TEST_CASE("an HX OmtSource publishes NV12 pictures") {
  const omthx::VideoFormat f{640, 360, 60, 1, false};
  omthx::Sender sender("ob-test-source-nv12", omthx::Mode::Hx, omthx::Quality::Medium,
                       omthx::Codec::Hevc);
  std::atomic<int> nv12{0}, others{0}, invalid{0};
  ob::OmtSource source(
      sender.address(),
      [&](std::shared_ptr<ob::Frame> fr) {
        if (!fr->codedOnly()) {
          if (!fr->valid()) ++invalid;
          else if (fr->layout == ob::Layout::Nv12) ++nv12;
          else ++others;
        }
        return false;
      },
      [](ob::SignalState, std::optional<ob::ErrorInfo>) {},
      [](std::shared_ptr<ob::AudioFrame>) {},
      [](bool, bool) { return true; });
  source.start();
  Pump pump(sender, f);
  const bool encoding = waitFor([&] { return !sender.stats().encoder.empty(); }, 3s);
  const bool pictures = waitFor([&] { return nv12.load() >= 10; }, 5s);
  source.stop();
  if (pump.failed() || !encoding) {
    MESSAGE("SKIPPED, no hardware HEVC encoder on this PC");
    return;
  }
  CHECK(pictures);
  CHECK(invalid.load() == 0);
  CHECK(others.load() == 0);  // the picture lane never unpacks to UYVY
  CHECK(source.framesDecoded() > 0);
}

TEST_CASE("an HX source that turns alpha on switches to Uyvy at a keyframe") {
  const omthx::VideoFormat plain{640, 360, 60, 1, false};
  const omthx::VideoFormat keyed{640, 360, 60, 1, true};
  omthx::Sender sender("ob-test-source-alpha-switch", omthx::Mode::Hx, omthx::Quality::Medium,
                       omthx::Codec::Hevc);
  std::atomic<int> nv12{0}, uyvy{0}, invalid{0};
  std::atomic<bool> sawAlpha{false};
  ob::OmtSource source(
      sender.address(),
      [&](std::shared_ptr<ob::Frame> fr) {
        if (!fr->codedOnly()) {
          if (!fr->valid()) {
            ++invalid;
          } else if (fr->layout == ob::Layout::Nv12) {
            ++nv12;
          } else {
            ++uyvy;
            if (fr->format.alpha) sawAlpha.store(true);
          }
        }
        return false;
      },
      [](ob::SignalState, std::optional<ob::ErrorInfo>) {},
      [](std::shared_ptr<ob::AudioFrame>) {},
      [](bool, bool) { return true; });
  source.start();
  bool skipped = false;
  {
    Pump pump(sender, plain);
    const bool encoding = waitFor([&] { return !sender.stats().encoder.empty(); }, 3s);
    const bool got = waitFor([&] { return nv12.load() >= 10; }, 5s);
    skipped = pump.failed() || !encoding || !got;
  }
  if (!skipped) {
    Pump pump(sender, keyed);
    const bool switched = waitFor([&] { return sawAlpha.load() && uyvy.load() >= 10; }, 5s);
    skipped = pump.failed() || !switched;
  }
  source.stop();
  if (skipped) {
    MESSAGE("SKIPPED, no hardware HEVC encoder on this PC");
    return;
  }
  CHECK(invalid.load() == 0);  // no picture with the wrong layout while the switch happens
  CHECK(nv12.load() >= 10);
  CHECK(uyvy.load() >= 10);
  CHECK(sawAlpha.load());
}

TEST_CASE("a keyframe request from downstream reaches the source") {
  const omthx::VideoFormat f{640, 360, 60, 1, false};
  omthx::Sender sender("ob-test-source-keyframe", omthx::Mode::Hx, omthx::Quality::Medium,
                       omthx::Codec::Hevc);
  std::atomic<bool> asked{false};
  ob::OmtSource source(
      sender.address(),
      [&](std::shared_ptr<ob::Frame>) { return !asked.exchange(true); },  // true once
      [](ob::SignalState, std::optional<ob::ErrorInfo>) {},
      [](std::shared_ptr<ob::AudioFrame>) {},
      [](bool, bool) { return false; });
  source.start();
  Pump pump(sender, f);
  const bool encoding = waitFor([&] { return !sender.stats().encoder.empty(); }, 3s);
  if (pump.failed() || !encoding) {
    MESSAGE("SKIPPED, no hardware HEVC encoder on this PC");
    source.stop();
    return;
  }
  const uint64_t before = sender.stats().keyframes;
  CHECK(waitFor([&] { return sender.stats().keyframes > before; }, 3s));
  CHECK(asked.load());
  source.stop();
}