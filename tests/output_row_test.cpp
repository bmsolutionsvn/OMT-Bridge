#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "engine/output_row.h"

using namespace std::chrono_literals;

static std::shared_ptr<const ob::Frame> frame(int64_t ts) {
  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{4, 2, 60, 1, false};
  f->uyvy.assign(f->lumaBytes(), 0x10);
  f->timestamp = ts;
  return f;
}

TEST_CASE("a row delivers what it is offered, in order") {
  std::mutex m;
  std::vector<int64_t> got;
  ob::OutputRow row("o1", "s1", [&](const std::shared_ptr<const ob::Frame>& f) {
    std::lock_guard l(m);
    got.push_back(f->timestamp);
  });
  row.start();
  for (int64_t i = 0; i < 3; ++i) row.offer(frame(i));

  // stop() closes the queue and clears anything not yet delivered, so wait for delivery first.
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (row.delivered() < 3 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();

  CHECK(got == std::vector<int64_t>{0, 1, 2});
}

TEST_CASE("a slow sink drops the oldest frames and never blocks the producer") {
  std::atomic<bool> gate{false};
  ob::OutputRow row("o1", "s1",
                    [&](const std::shared_ptr<const ob::Frame>&) {
                      while (!gate.load()) std::this_thread::sleep_for(1ms);
                    }, /*capacity=*/2u);
  row.start();

  // The worker takes one and blocks; two more fill the queue; the next two drop the oldest.
  for (int64_t i = 0; i < 5; ++i) row.offer(frame(i));
  gate.store(true);
  row.stop();

  CHECK(row.dropped() >= 2u);
}

TEST_CASE("stopping a row unblocks its worker") {
  ob::OutputRow row("o1", "s1", [](const std::shared_ptr<const ob::Frame>&) {});
  row.start();
  row.stop();  // must not hang
  CHECK(true);
}

TEST_CASE("a sink that throws stops the row instead of terminating the process") {
  ob::OutputRow row("o1", "s1", [](const std::shared_ptr<const ob::Frame>&) {
    throw std::runtime_error("sink broke");
  });
  row.start();
  row.offer(frame(0));

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!row.faulted() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();  // must not hang on a worker that already returned

  CHECK(row.faulted());
  CHECK(row.delivered() == 0u);  // the frame was consumed by the throw, not delivered
}

TEST_CASE("a row can be started again after a stop") {
  ob::OutputRow row("o1", "s1", [](const std::shared_ptr<const ob::Frame>&) {});
  row.start();
  row.offer(frame(0));
  auto deadline = std::chrono::steady_clock::now() + 2s;
  while (row.delivered() < 1 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();
  CHECK(row.delivered() == 1u);

  // Restart: start() must reopen the closed queue, or the worker exits at once and delivers
  // nothing (this is the "Stop all outputs, then Start" cycle the UI offers).
  row.start();
  row.offer(frame(1));
  deadline = std::chrono::steady_clock::now() + 2s;
  while (row.delivered() < 2 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();
  CHECK(row.delivered() == 2u);
}
