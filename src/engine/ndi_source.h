#pragma once
// One NDI input (design 8). One worker owns the find/recv instances; stop() joins it.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/audio.h"
#include "core/frame.h"
#include "core/signal_state.h"

namespace ob {

class NdiSource {
 public:
  using FrameSink = std::function<void(std::shared_ptr<Frame>)>;
  // Called from the receive thread: must not block or re-enter NdiSource.
  using AudioSink = std::function<void(std::shared_ptr<AudioFrame>)>;
  // Called from start/stop and from the receive thread. It must not block, must not re-enter
  // NdiSource, and must synchronise its own storage.
  using StateSink = std::function<void(SignalState, std::optional<ErrorInfo>)>;

  NdiSource(std::string name, FrameSink onFrame, AudioSink onAudio, StateSink onState,
            bool lowBandwidth = false);
  ~NdiSource();
  NdiSource(const NdiSource&) = delete;
  NdiSource& operator=(const NdiSource&) = delete;

  void start();  // idempotent; Connecting, then Offline if the NDI runtime cannot start
  void stop();   // idempotent; joins the worker, destroys find/recv; Ready
  // true while the worker exists; it may be true after the worker has exited on an error until stop().
  bool running() const;

  // Stored atomically; the receive thread applies it with NDIlib_recv_set_tally.
  void setTally(bool program, bool preview);

 private:
  void stopImpl(bool notify);
  void run();

  std::string name_;
  // NDI's preview mode: lower bandwidth and resolution, for a source that only feeds a thumbnail
  // (design 11). The engine reopens the receiver at full bandwidth when an output attaches.
  bool lowBandwidth_ = false;
  FrameSink onFrame_;
  AudioSink onAudio_;
  StateSink onState_;
  std::atomic<bool> stop_{false};
  mutable std::mutex mu_;  // fields only; never join while holding it
  std::mutex lifecycle_;   // serialises whole start()/stop() bodies
  std::thread thread_;     // guarded by mu_
  std::atomic<uint32_t> tally_{0};  // bit 0 program, bit 1 preview
  std::atomic<bool> tallyDirty_{false};
};

}  // namespace ob
