#include "engine/output_row.h"

#include <chrono>
#include <exception>

namespace ob {
namespace {
constexpr auto kPopTimeout = std::chrono::milliseconds(100);
}

OutputRow::OutputRow(std::string id, std::unique_ptr<Sink> sink, size_t capacity)
    : id_(std::move(id)), sink_(std::move(sink)), queue_(capacity) {}

OutputRow::~OutputRow() { stop(); }

void OutputRow::start() {
  if (thread_.joinable()) return;
  // A deliberate restart retries: clear a previous fault, or a row that now works would still be
  // reported Offline by the engine. `delivered_` is NOT reset — it is a cumulative stat.
  faulted_.store(false);
  live_.store(false);
  {
    std::lock_guard l(faultMu_);
    faultNote_.clear();
  }
  // A row can be started again after stop() ("Stop all outputs", then resume): stop() closed the
  // queue, so reopen it before the worker starts — otherwise its first pop returns nullopt, sees
  // `closed()`, and the worker exits immediately, delivering nothing.
  queue_.reopen();
  thread_ = std::thread([this] { run(); });
}

void OutputRow::stop() {
  queue_.close();
  if (thread_.joinable()) thread_.join();
  live_.store(false);
}

void OutputRow::offer(std::shared_ptr<const Frame> frame) { queue_.push(std::move(frame)); }

std::string OutputRow::note() const {
  {
    std::lock_guard l(faultMu_);
    if (!faultNote_.empty()) return faultNote_;
  }
  return sink_ ? sink_->note() : std::string{};
}

bool OutputRow::live() const {
  if (!live_.load()) return false;
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  return now - lastDeliverMs_.load() < 2000;
}

void OutputRow::configure(const Output& output, const std::string& advertisedName) {
  config_ = output;  // configuration-only copy; Output::operator== is config-only by design
  if (sink_) sink_->configure(config_, advertisedName);
}

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
      if (sink_) sink_->deliver(*f);
    } catch (const std::exception& e) {
      {
        std::lock_guard l(faultMu_);
        faultNote_ = e.what();
      }
      faulted_.store(true);
      live_.store(false);
      return;
    } catch (...) {
      {
        std::lock_guard l(faultMu_);
        faultNote_ = "sink failed";
      }
      faulted_.store(true);
      live_.store(false);
      return;
    }
    ++delivered_;
    lastDeliverMs_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
    live_.store(true);
  }
}

}  // namespace ob
