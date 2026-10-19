#include "engine/output_row.h"

#include <chrono>
#include <exception>

namespace ob {
namespace {
constexpr auto kPopTimeout = std::chrono::milliseconds(100);
}

OutputRow::OutputRow(std::string id, std::string sourceId, Deliver deliver, size_t capacity)
    : id_(std::move(id)), sourceId_(std::move(sourceId)), deliver_(std::move(deliver)),
      queue_(capacity) {}

OutputRow::~OutputRow() { stop(); }

void OutputRow::start() {
  if (thread_.joinable()) return;
  // A row can be started again after stop() ("Stop all outputs", then resume): stop() closed the
  // queue, so reopen it before the worker starts — otherwise its first pop returns nullopt, sees
  // `closed()`, and the worker exits immediately, delivering nothing.
  queue_.reopen();
  thread_ = std::thread([this] { run(); });
}

void OutputRow::stop() {
  queue_.close();
  if (thread_.joinable()) thread_.join();
}

void OutputRow::offer(std::shared_ptr<const Frame> frame) { queue_.push(std::move(frame)); }

void OutputRow::run() {
  for (;;) {
    auto f = queue_.pop(kPopTimeout);
    if (!f.has_value()) {
      if (queue_.closed()) return;  // closed by stop(); a timeout just keeps waiting
      continue;
    }
    // A sink that throws must not take the process down — an exception escaping a thread body
    // calls std::terminate — and must not spin retrying either. Mark the row faulted and stop
    // delivering; the engine turns that into Offline for this output alone, so a failed sink
    // never affects another row or its SourceHub (the header's promise).
    try {
      deliver_(*f);
    } catch (...) {
      faulted_.store(true);
      return;
    }
    ++delivered_;
  }
}

}  // namespace ob
