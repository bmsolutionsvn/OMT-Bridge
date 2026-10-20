#pragma once
// One NDI input (design 8). One worker owns the find/recv instances; stop() joins it.
#include <atomic>
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
  using StateSink = std::function<void(SignalState, std::optional<ErrorInfo>)>;

  NdiSource(std::string name, FrameSink onFrame, AudioSink onAudio, StateSink onState);
  ~NdiSource();
  NdiSource(const NdiSource&) = delete;
  NdiSource& operator=(const NdiSource&) = delete;

  void start();  // idempotent; Connecting, then Offline if the NDI runtime cannot start
  void stop();   // idempotent; joins the worker, destroys find/recv; Ready
  bool running() const;

  // Stored atomically; the receive thread applies it with NDIlib_recv_set_tally.
  void setTally(bool program, bool preview);

 private:
  void stopImpl(bool notify);
  void run();

  std::string name_;
  FrameSink onFrame_;
  AudioSink onAudio_;
  StateSink onState_;
  std::atomic<bool> stop_{false};
  mutable std::mutex mu_;  // fields only; never join while holding it
  std::mutex lifecycle_;   // serialises whole start()/stop() bodies
  std::thread thread_;     // guarded by mu_
  std::atomic<bool> tallyProgram_{false};
  std::atomic<bool> tallyPreview_{false};
  std::atomic<bool> tallyDirty_{false};
};

}  // namespace ob