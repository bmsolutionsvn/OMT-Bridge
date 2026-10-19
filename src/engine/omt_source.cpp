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

OmtSource::~OmtSource() { stopImpl(false); }  // no state callback from a destructor

bool OmtSource::running() const {
  std::lock_guard l(mu_);
  return rx_ != nullptr;  // true while a receiver is open, including the join window
}

void OmtSource::start() {
  std::lock_guard lifecycle(lifecycle_);
  {
    std::lock_guard l(mu_);
    if (video_.joinable() || audio_.joinable()) return;
  }
  std::unique_ptr<OmtReceiverHolder> rx;
  try {
    rx = std::make_unique<OmtReceiverHolder>(address_);
  } catch (const std::exception& e) {
    // Never let an open failure escape into the graph's demand path.
    if (onState_) onState_(SignalState::Offline, ErrorInfo{"omt-open", e.what()});
    return;
  }
  OmtReceiverHolder* holder = rx.get();
  {
    std::lock_guard l(mu_);
    stop_.store(false);
    rx_ = std::move(rx);
  }
  if (onState_) onState_(SignalState::Connecting, std::nullopt);

  std::thread v, a;
  try {
    v = std::thread([this, holder] { videoLoop(*holder); });
    a = std::thread([this, holder] { audioLoop(*holder); });
  } catch (const std::exception& e) {
    stop_.store(true);
    if (v.joinable()) v.join();
    {
      std::lock_guard l(mu_);
      rx_.reset();
    }
    if (onState_) onState_(SignalState::Offline, ErrorInfo{"omt-thread", e.what()});
    return;
  }
  {
    std::lock_guard l(mu_);
    video_ = std::move(v);
    audio_ = std::move(a);
  }
}

void OmtSource::stop() { stopImpl(true); }

void OmtSource::stopImpl(bool notify) {
  std::lock_guard lifecycle(lifecycle_);
  std::thread v, a;
  bool hadThreads = false;
  {
    std::lock_guard l(mu_);
    hadThreads = video_.joinable() || audio_.joinable();
    if (hadThreads) {
      stop_.store(true);
      v = std::move(video_);
      a = std::move(audio_);
    }
  }
  if (v.joinable()) v.join();
  if (a.joinable()) a.join();
  {
    std::lock_guard l(mu_);
    rx_.reset();  // closed only after both loops are gone
  }
  if (notify && onState_) onState_(SignalState::Ready, std::nullopt);
}

void OmtSource::videoLoop(OmtReceiverHolder& rx) {
  try {
    // Uyvy is the canonical frame: the UYVY plane followed by the alpha plane, exactly UYVA.
    bool live = false;
    auto last = std::chrono::steady_clock::now();
    while (!stop_.load()) {
      omthx::VideoFrame vf;
      if (rx.receiver.receiveVideo(vf, kReceiveTimeoutMs)) {
        auto f = std::make_shared<Frame>();
        f->format = VideoFormat{vf.format.width, vf.format.height, vf.format.fpsN,
                                vf.format.fpsD, vf.format.alpha};
        f->premultiplied = vf.premultiplied;
        f->timestamp = vf.timestamp;
        f->uyvy = std::move(vf.uyvy);
        if (!f->valid()) continue;  // malformed peer: never publish a frame the hub rejects
        last = std::chrono::steady_clock::now();  // liveness follows usable frames
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
  omthx::AudioFrame af;  // hoisted: receiveAudio reuses the planar vector's capacity
  try {
    while (!stop_.load()) {
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
