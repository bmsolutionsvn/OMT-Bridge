#include <doctest/doctest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/audio.h"
#include "core/frame.h"
#include "core/signal_state.h"
#include "engine/ndi_runtime.h"
#include "engine/ndi_sink.h"
#include "engine/ndi_source.h"

#ifdef OB_WITH_NDI

namespace {

std::shared_ptr<ob::Frame> makePattern(int w, int h) {
  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{w, h, 60, 1, true};
  f->uyvy.assign(f->lumaBytes() + f->alphaBytes(), 0x80);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; x += 2) {
      const size_t o = static_cast<size_t>(y) * w * 2 + static_cast<size_t>(x) * 2;
      f->uyvy[o] = 128;  // U
      f->uyvy[o + 1] = static_cast<uint8_t>(40 + (y % 100));  // Y0
      f->uyvy[o + 2] = 128;  // V
      f->uyvy[o + 3] = static_cast<uint8_t>(40 + (y % 100));  // Y1
    }
    // Three alpha bands: opaque, transparent, half. A wrong plane offset or row pitch shifts these.
    const uint8_t a = y < h / 3 ? 255 : (y < 2 * h / 3 ? 0 : 128);
    for (int x = 0; x < w; ++x)
      f->uyvy[f->lumaBytes() + static_cast<size_t>(y) * w + static_cast<size_t>(x)] = a;
  }
  return f;
}

}  // namespace

TEST_CASE("an NDI sink and source round-trip video, alpha and audio") {
  REQUIRE(ob::ensureNdi());

  // The advertised name is matched by substring, so the machine prefix does not matter.
  const std::string advertised = "OMT Bridge M3 Test (Out)";

  ob::NdiSink sink;
  ob::Output o;
  o.id = "o1";
  o.target = ob::OutputTarget::Ndi;
  o.alpha = ob::AlphaMode::Auto;
  sink.configure(o, advertised);

  std::mutex m;
  std::condition_variable cv;
  std::shared_ptr<const ob::Frame> video;
  std::shared_ptr<const ob::AudioFrame> audio;

  ob::NdiSource src(
      advertised,
      [&](std::shared_ptr<ob::Frame> f) {
        std::lock_guard l(m);
        video = std::move(f);
        cv.notify_all();
      },
      [&](std::shared_ptr<ob::AudioFrame> a) {
        std::lock_guard l(m);
        audio = std::move(a);
        cv.notify_all();
      },
      [](ob::SignalState, std::optional<ob::ErrorInfo>) {});

  // Let the sender advertise before discovery starts.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  src.start();

  auto frame = makePattern(64, 36);
  auto packet = std::make_shared<ob::AudioFrame>();
  packet->format = {44100, 2};
  packet->samples = 441;  // 10 ms at 44.1 kHz
  packet->planar.resize(882);
  for (int i = 0; i < 441; ++i) {
    packet->planar[static_cast<size_t>(i)] = static_cast<float>(i) / 441.0f;
    packet->planar[441 + static_cast<size_t>(i)] = -static_cast<float>(i) / 441.0f;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    sink.deliver(frame);
    sink.deliverAudio(packet);
    {
      std::lock_guard l(m);
      if (video && audio) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  src.stop();

  std::lock_guard l(m);
  REQUIRE(video.get() != nullptr);
  CHECK(video->format.width == 64);
  CHECK(video->format.height == 36);
  CHECK(video->format.alpha);
  REQUIRE(video->valid());

  // SpeedHQ is lossy at edges, so sample the middle of each band.
  CHECK(video->uyvy[video->lumaBytes() + 1 * 64 + 32] >= 240);
  CHECK(video->uyvy[video->lumaBytes() + 18 * 64 + 32] <= 15);
  const int mid = video->uyvy[video->lumaBytes() + 30 * 64 + 32];
  CHECK(mid >= 110);
  CHECK(mid <= 146);

  REQUIRE(audio.get() != nullptr);
  CHECK(audio->format.sampleRate == 44100);
  CHECK(audio->format.channels == 2);
}

TEST_CASE("a premultiplied frame reaches NDI as straight colour") {
  REQUIRE(ob::ensureNdi());
  const std::string advertised = "OMT Bridge M3 Test (Straight)";

  ob::NdiSink sink;
  ob::Output o;
  o.id = "o1";
  o.target = ob::OutputTarget::Ndi;
  o.alpha = ob::AlphaMode::Auto;
  sink.configure(o, advertised);

  std::mutex m;
  std::shared_ptr<const ob::Frame> video;
  ob::NdiSource src(
      advertised,
      [&](std::shared_ptr<ob::Frame> f) {
        std::lock_guard l(m);
        video = std::move(f);
      },
      [](std::shared_ptr<ob::AudioFrame>) {},
      [](ob::SignalState, std::optional<ob::ErrorInfo>) {});

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  src.start();

  // Premultiplied Y=126, U=190, V=70 at alpha 128 -> straight Y~235, U clamps to 240, V~51.
  auto frame = std::make_shared<ob::Frame>();
  frame->format = ob::VideoFormat{64, 36, 60, 1, true};
  frame->premultiplied = true;
  frame->uyvy.assign(frame->lumaBytes() + frame->alphaBytes(), 128);
  for (size_t i = 0; i + 3 < frame->lumaBytes(); i += 4) {
    frame->uyvy[i] = 190;      // U
    frame->uyvy[i + 1] = 126;  // Y0
    frame->uyvy[i + 2] = 70;   // V
    frame->uyvy[i + 3] = 126;  // Y1
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    sink.deliver(frame);
    {
      std::lock_guard l(m);
      if (video) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  src.stop();

  std::lock_guard l(m);
  REQUIRE(video.get() != nullptr);
  REQUIRE(video->format.alpha);
  const size_t mid = static_cast<size_t>(video->format.width) * 2 * (video->format.height / 2) + 32 * 2;
  CHECK(video->uyvy[mid + 1] >= 225);  // Y was doubled by the un-premultiply
  const int u = video->uyvy[mid];
  CHECK(u >= 230);                     // clamped near 240
  const int v = video->uyvy[mid + 2];
  CHECK(v >= 10);   // (70 - 128) / 0.502 + 128 ~= 16, clamped to the legal minimum
  CHECK(v <= 30);
}
#endif  // OB_WITH_NDI
