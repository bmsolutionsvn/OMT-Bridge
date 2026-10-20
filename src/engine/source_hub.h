#pragma once
// One source (design 6.4). Publishes the newest decoded frame into a single slot; the
// adapter is meant to run only while the hub is running, which is exactly when at least one
// output references the source or its thumbnail is wanted (demand-driven).
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "core/alpha_gate.h"
#include "core/frame.h"

namespace ob {

class SourceHub {
 public:
  using RunningChanged = std::function<void(bool running)>;
  // Injected so the 2 s opaque-alpha window can be tested without sleeping.
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  explicit SourceHub(std::string id, Clock clock = [] { return std::chrono::steady_clock::now(); })
      : id_(std::move(id)), clock_(std::move(clock)) {}

  const std::string& id() const { return id_; }

  void addRef();
  void release();
  int refs() const;

  void setThumbnailWanted(bool wanted);
  bool thumbnailWanted() const;

  bool running() const;

  // Called once per running transition. The engine starts or stops the input adapter here.
  // Install this BEFORE the first demand change: the setter does not replay the current state,
  // so a callback installed after the hub is already running waits for the next transition.
  void setOnRunningChanged(RunningChanged cb);

  // Stores the newest frame in the single slot and applies opaque-alpha detection. Non-const in,
  // const stored: the hub may clear `format.alpha` before the frame becomes visible to consumers.
  // This does NOT fan out — carrying media to rows is BridgeGraph::publish(sourceId, frame). An
  // adapter must call the graph, never this, or outputs receive nothing while framesPublished()
  // climbs.
  // Precondition: the frame must satisfy Frame::valid() (uyvy.size() == lumaBytes() +
  // alphaBytes()). The decode boundary validates; this is the first code that reads alpha from
  // dimensions that ultimately come from the wire.
  // Ownership: the caller transfers the frame for this publish and must not reuse or mutate it
  // afterwards. storeFrame mutates the object in place (clears alpha, shrinks the plane), so reuse
  // would persist a cleared alpha and race the hub/rows.
  // alpha_ is written only by BridgeGraph::publish, which serialises stores under the graph
  // mutex; a direct two-thread caller would race the gate.
  void storeFrame(std::shared_ptr<Frame> frame);
  std::shared_ptr<const Frame> latest() const;

  uint64_t framesPublished() const;

 private:
  bool computeRunning() const;  // caller holds mu_
  void maybeTransition();       // fires the callback once per transition, outside the lock

  std::string id_;
  Clock clock_;
  mutable std::mutex mu_;
  int refs_ = 0;
  bool thumbnailWanted_ = false;
  bool running_ = false;
  RunningChanged onRunningChanged_;

  std::shared_ptr<const Frame> latest_;
  uint64_t published_ = 0;
  AlphaGate alpha_;
};

}  // namespace ob
