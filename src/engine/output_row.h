#pragma once
// One output (design 6.5). Reads its source's newest frame and hands it to a sink. Its own
// drop-oldest queue means a slow or failed sink can never block another row or its SourceHub.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/audio.h"
#include "core/bounded_queue.h"
#include "core/bridge_model.h"
#include "core/frame.h"

namespace ob {

// A sink owns one output's delivery. The graph constructs it through SinkFactory and configures
// it; the row's worker calls deliver().
//
// configure() and deliver() may run on different threads (a re-apply while the row is running);
// a sink must synchronise its own state — the graph mutex does not protect it.
//
// A sink must never call back into BridgeGraph or SourceHub: construction and configure() run
// while the graph mutex is held, so a callback would invert the lock order.
//
// deliver() and deliverAudio() run on two worker threads and may run concurrently, so a sink
// must synchronise any state the two paths share. A default deliverAudio() drops audio.
//
// Construction and configure() run under the graph mutex: keep them cheap and do not block.
// A factory or configure() may reopen a real adapter and may therefore block (an OMT sender
// teardown/setup); while it does, commands wait. It must still never call back into the graph.
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void deliver(const std::shared_ptr<const Frame>& frame) = 0;
  virtual void deliverAudio(const std::shared_ptr<const AudioFrame>&) {}
  virtual void configure(const Output&, const std::string&) {}
  virtual std::string note() const { return {}; }
};
// May return nullptr; a row with no sink is legal.
using SinkFactory = std::function<std::unique_ptr<Sink>(const Output&, const std::string&)>;

class OutputRow {
 public:
  OutputRow(std::string id, std::unique_ptr<Sink> sink, size_t capacity = 4);
  ~OutputRow();
  OutputRow(const OutputRow&) = delete;
  OutputRow& operator=(const OutputRow&) = delete;

  // Lifecycle has a single writer: BridgeGraph serialises every row call under its own mutex.
  // Two threads calling start() at once could assign over a live std::thread.
  void start();
  void stop();

  void offer(std::shared_ptr<const Frame> frame);
  // Audio travels on its own queue and worker, so a stalled or failing sink's audio path can
  // never delay or fault video (design 10).
  void offerAudio(std::shared_ptr<const AudioFrame> frame);

  // Applies the output's configuration to the row and its sink. Idempotent. Called by the graph
  // under its mutex; the worker thread never reads config_ (the sink owns its own state).
  void configure(const Output& output, const std::string& advertisedName);

  const std::string& id() const { return id_; }
  const std::string& sourceId() const { return config_.sourceId; }
  // Read only by the graph, under its mutex, so an apply() can tell a configuration edit from a
  // re-apply of the same model.
  const Output& config() const { return config_; }
  // The fault message when the sink threw, else the sink's own note.
  std::string note() const;

  uint64_t delivered() const { return delivered_.load(); }  // atomic: incremented on the worker thread
  // Audio packets handed to the sink, cumulative like delivered(). A throwing audio sink does not
  // count, and does not fault the row.
  uint64_t audioDelivered() const { return audioDelivered_.load(); }
  uint64_t dropped() const { return queue_.dropped(); }
  // True while the running worker has delivered a frame recently. Per-run and decaying: start()/
  // stop()/a fault reset it, and a stall (no frame for 2 s) reports not-live without a stop.
  bool live() const;
  // True once the sink threw. The worker then stops delivering (it does not spin retrying); the
  // engine reads this to mark this output Offline, so one failed sink affects only its own row.
  bool faulted() const { return faulted_.load(); }

 private:
  void run();
  void audioRun();

  std::string id_;
  Output config_;
  std::unique_ptr<Sink> sink_;
  BoundedQueue<std::shared_ptr<const Frame>> queue_;
  BoundedQueue<std::shared_ptr<const AudioFrame>> audioQueue_;
  std::thread thread_;
  std::thread audioThread_;
  // Read by the test (and later the state snapshot) on a different thread from run(), which
  // does the increment: it must be atomic, not a plain uint64_t.
  std::atomic<uint64_t> delivered_ = 0;
  std::atomic<uint64_t> audioDelivered_ = 0;
  std::atomic<int64_t> lastDeliverMs_{0};
  std::atomic<bool> live_ = false;
  std::atomic<bool> faulted_ = false;
  // The worker's fault message; guarded because note() reads it from another thread.
  mutable std::mutex faultMu_;
  std::string faultNote_;
};

}  // namespace ob
