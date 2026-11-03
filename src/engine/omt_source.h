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
  // True: a downstream receiver wants a keyframe (a pass-through output whose receiver joined or
  // lost a frame), so the video loop asks the source. Called on the video thread.
  using FrameSink = std::function<bool(std::shared_ptr<Frame>)>;
  // Called from the audio thread. It must not block or re-enter OmtSource.
  using AudioSink = std::function<void(std::shared_ptr<AudioFrame>)>;
  // Called from the video thread, for each coded frame, before anything is decoded: (source is HX,
  // source has alpha). Null means a picture is always wanted.
  using PictureWanted = std::function<bool(bool hx, bool alpha)>;
  // Called from the start/stop caller and from the video/audio threads. It must not block, must
  // not re-enter OmtSource, and must synchronise its own storage.
  using StateSink = std::function<void(SignalState, std::optional<ErrorInfo>)>;

  OmtSource(std::string address, FrameSink onFrame, StateSink onState, AudioSink onAudio,
            PictureWanted pictureWanted = {}, bool preview = false);
  ~OmtSource();
  OmtSource(const OmtSource&) = delete;
  OmtSource& operator=(const OmtSource&) = delete;

  void start();  // idempotent; opens the receiver; Connecting (Offline if it cannot open)
  void stop();   // idempotent; joins both threads, closes the receiver; Ready
  bool running() const;
  // Stored atomically; the video thread applies it with Receiver::setTally (design 12.3).
  void setTally(bool program, bool preview);
  // Pictures this source has decoded; 0 while only pass-through outputs read it.
  uint64_t framesDecoded() const { return framesDecoded_.load(); }

 private:
  void stopImpl(bool notify);
  void videoLoop(OmtReceiverHolder& rx);
  void audioLoop(OmtReceiverHolder& rx);

  std::string address_;
  // A small picture for a source with no outputs (design 11): OMT's VMX preview, or OMT-HX's
  // second ~640-wide stream. The engine reopens the receiver at full size when an output attaches.
  bool preview_ = false;
  FrameSink onFrame_;
  StateSink onState_;
  AudioSink onAudio_;
  PictureWanted pictureWanted_;
  std::atomic<bool> stop_{false};
  std::atomic<uint32_t> tally_{0};  // bit 0 program, bit 1 preview
  std::atomic<bool> tallyDirty_{false};
  std::atomic<uint64_t> framesDecoded_{0};
  mutable std::mutex mu_;   // fields only; never join while holding it
  std::mutex lifecycle_;    // serialises whole start()/stop() bodies
  std::unique_ptr<OmtReceiverHolder> rx_;  // guarded by mu_; alive only while running
  std::thread video_, audio_;
};

}  // namespace ob
