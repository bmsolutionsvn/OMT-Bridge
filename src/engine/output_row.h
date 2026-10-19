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
#include "core/frame.h"

namespace ob {

class OutputRow {
 public:
  using Deliver = std::function<void(const std::shared_ptr<const Frame>&)>;

  OutputRow(std::string id, std::string sourceId, Deliver deliver, size_t capacity = 4);
  ~OutputRow();
  OutputRow(const OutputRow&) = delete;
  OutputRow& operator=(const OutputRow&) = delete;

  // Lifecycle and setSourceId have a single writer: BridgeGraph serialises every row call under
  // its own mutex. Two threads calling start() at once could assign over a live std::thread.
  void start();
  void stop();

  void offer(std::shared_ptr<const Frame> frame);

  const std::string& id() const { return id_; }
  const std::string& sourceId() const { return sourceId_; }
  void setSourceId(std::string id) { sourceId_ = std::move(id); }

  uint64_t delivered() const { return delivered_.load(); }  // atomic: incremented on the worker thread
  uint64_t dropped() const { return queue_.dropped(); }
  // True once the sink threw. The worker then stops delivering (it does not spin retrying); the
  // engine reads this to mark this output Offline, so one failed sink affects only its own row.
  bool faulted() const { return faulted_.load(); }

 private:
  void run();

  std::string id_;
  std::string sourceId_;
  Deliver deliver_;
  BoundedQueue<std::shared_ptr<const Frame>> queue_;
  std::thread thread_;
  // Read by the test (and later the state snapshot) on a different thread from run(), which
  // does the increment: it must be atomic, not a plain uint64_t.
  std::atomic<uint64_t> delivered_ = 0;
  std::atomic<bool> faulted_ = false;
};

}  // namespace ob
