#include "engine/omt_source.h"

#include <chrono>
#include <exception>

#include <omthx/omthx.h>

namespace ob {

struct OmtReceiverHolder {
  explicit OmtReceiverHolder(const std::string& address)
      : receiver(address, omthx::PixelFormat::Uyvy) {}
  omthx::Receiver receiver;
};

namespace {
constexpr int kReceiveTimeoutMs = 100;
constexpr auto kOfflineAfter = std::chrono::seconds(2);
}

OmtSource::OmtSource(std::string address, FrameSink onFrame, StateSink onState)
    : address_(std::move(address)), onFrame_(std::move(onFrame)), onState_(std::move(onState)) {}

OmtSource::~OmtSource() { stop(); }

bool OmtSource::running() const {
  std::lock_guard l(mu_);
  return video_.joinable();
}

void OmtSource::start() {
  std::lock_guard l(mu_);
  // A stop() joins with mu_ released; until it finishes, the old loops still use the receiver.
  if (video_.joinable() || audio_.joinable() || stopping_) return;
  stop_.store(false);
  try {
    rx_ = std::make_unique<OmtReceiverHolder>(address_);
  } catch (const std::exception& e) {
    // Never let an open failure escape into the graph's demand path.
    rx_.reset();
    if (onState_) onState_(SignalState::Offline, ErrorInfo{"omt-open", e.what()});
    return;
  }
  if (onState_) onState_(SignalState::Connecting, std::nullopt);
  video_ = std::thread([this] { videoLoop(*rx_); });
  audio_ = std::thread([this] { audioLoop(*rx_); });
}

void OmtSource::stop() {
  std::thread v, a;
  {
    std::lock_guard l(mu_);
    if (!video_.joinable() && !audio_.joinable()) {
      rx_.reset();
      return;
    }
    stopping_ = true;
    stop_.store(true);
    v = std::move(video_);
    a = std::move(audio_);
  }
  if (v.joinable()) v.join();
  if (a.joinable()) a.join();
  {
    std::lock_guard l(mu_);
    rx_.reset();      // closed only after both loops are gone
    stopping_ = false;
  }
  if (onState_) onState_(SignalState::Ready, std::nullopt);
}

void OmtSource::videoLoop(OmtReceiverHolder& rx) {
  try {
    // Uyvy is the canonical frame: the UYVY plane followed by the alpha plane, exactly UYVA.
    bool live = false;
    auto last = std::chrono::steady_clock::now();
    while (!stop_.load()) {
      omthx::VideoFrame vf;
      if (rx.receiver.receiveVideo(vf, kReceiveTimeoutMs)) {
        last = std::chrono::steady_clock::now();
        auto f = std::make_shared<Frame>();
        f->format = VideoFormat{vf.format.width, vf.format.height, vf.format.fpsN,
                                vf.format.fpsD, vf.format.alpha};
        f->premultiplied = vf.premultiplied;
        f->timestamp = vf.timestamp;
        f->uyvy = std::move(vf.uyvy);
        if (!live) {
          live = true;
          if (onState_) onState_(SignalState::Live, std::nullopt);
        }
        if (onFrame_) onFrame_(std::move(f));
      } else if (live && std::chrono::steady_clock::now() - last > kOfflineAfter) {
        live = false;  // was live, no video
        if (onState_) onState_(SignalState::Offline, std::nullopt);
      }
    }
  } catch (const std::exception& e) {
    if (onState_) onState_(SignalState::Offline, ErrorInfo{"omt-receive", e.what()});
  }
}

void OmtSource::audioLoop(OmtReceiverHolder& rx) {
  try {
    while (!stop_.load()) {
      omthx::AudioFrame af;
      if (rx.receiver.receiveAudio(af, kReceiveTimeoutMs)) {
        // Audio passthrough is M3 (design 20 does not list it in M2's done-criteria). Draining
        // keeps the session's audio path moving instead of queuing behind an unread stream.
      }
    }
  } catch (const std::exception&) {
    // Audio failure must not take video down; the video path is independent.
  }
}

}  // namespace ob
