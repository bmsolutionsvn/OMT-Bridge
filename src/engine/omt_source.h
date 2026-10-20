#pragma once
// One OMT or OMT-HX input (design 8). Runs only while its SourceHub is demanded: start() opens an
// omthx::Receiver, stop() closes it. ONE receiver serves both loops — the library states that
// receiveVideo and receiveAudio may run on two threads, one call type per thread.
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

struct OmtReceiverHolder;  // hides omthx from this header

class OmtSource {
 public:
  using FrameSink = std::function<void(std::shared_ptr<Frame>)>;
  // Called from the audio thread. It must not block or re-enter OmtSource.
  using AudioSink = std::function<void(std::shared_ptr<AudioFrame>)>;
  // Called from the start/stop caller and from the video/audio threads. It must not block, must
  // not re-enter OmtSource, and must synchronise its own storage.
  using StateSink = std::function<void(SignalState, std::optional<ErrorInfo>)>;

  OmtSource(std::string address, FrameSink onFrame, StateSink onState, AudioSink onAudio);
  ~OmtSource();
  OmtSource(const OmtSource&) = delete;
  OmtSource& operator=(const OmtSource&) = delete;

  void start();  // idempotent; opens the receiver; Connecting (Offline if it cannot open)
  void stop();   // idempotent; joins both threads, closes the receiver; Ready
  bool running() const;
  // Stored atomically; the video thread applies it with Receiver::setTally (design 12.3).
  void setTally(bool program, bool preview);

 private:
  void stopImpl(bool notify);
  void videoLoop(OmtReceiverHolder& rx);
  void audioLoop(OmtReceiverHolder& rx);

  std::string address_;
  FrameSink onFrame_;
  StateSink onState_;
  AudioSink onAudio_;
  std::atomic<bool> stop_{false};
  std::atomic<uint32_t> tally_{0};  // bit 0 program, bit 1 preview
  std::atomic<bool> tallyDirty_{false};
  mutable std::mutex mu_;   // fields only; never join while holding it
  std::mutex lifecycle_;    // serialises whole start()/stop() bodies
  std::unique_ptr<OmtReceiverHolder> rx_;  // guarded by mu_; alive only while running
  std::thread video_, audio_;
};

}  // namespace ob
