#pragma once
// One output (design 6.5). Reads its source's newest frame and hands it to a sink. Its own
// drop-oldest queue means a slow or failed sink can never block another row or its SourceHub.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "core/bounded_queue.h"
#include "core/bridge_model.h"
#include "core/frame.h"

namespace ob {

class Sink {
 public:
  virtual ~Sink() = default;
  virtual void deliver(const std::shared_ptr<const Frame>& frame) = 0;
  virtual void configure(const Output& output, const std::string& advertisedName) {}
  virtual std::string note() const { return {}; }
};
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

  // Applies the output's configuration to the row and its sink. Idempotent. Called by the graph
  // under its mutex; the worker thread never reads config_ (the sink owns its own state).
  void configure(const Output& output, const std::string& advertisedName);

  const std::string& id() const { return id_; }
  const std::string& sourceId() const { return config_.sourceId; }
  std::string note() const { return sink_ ? sink_->note() : std::string{}; }

  uint64_t delivered() const { return delivered_.load(); }  // atomic: incremented on the worker thread
  uint64_t dropped() const { return queue_.dropped(); }
  // True once the sink threw. The worker then stops delivering (it does not spin retrying); the
  // engine reads this to mark this output Offline, so one failed sink affects only its own row.
  bool faulted() const { return faulted_.load(); }

 private:
  void run();

  std::string id_;
  Output config_;
  std::unique_ptr<Sink> sink_;
  BoundedQueue<std::shared_ptr<const Frame>> queue_;
  std::thread thread_;
  // Read by the test (and later the state snapshot) on a different thread from run(), which
  // does the increment: it must be atomic, not a plain uint64_t.
  std::atomic<uint64_t> delivered_ = 0;
  std::atomic<bool> faulted_ = false;
};

}  // namespace ob
