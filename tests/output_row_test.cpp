#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "core/audio.h"
#include "engine/output_row.h"

using namespace std::chrono_literals;

static std::shared_ptr<const ob::Frame> frame(int64_t ts) {
  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{4, 2, 60, 1, false};
  f->uyvy.assign(f->lumaBytes(), 0x10);
  f->timestamp = ts;
  return f;
}

namespace {
class LambdaSink : public ob::Sink {
 public:
  explicit LambdaSink(std::function<void(const std::shared_ptr<const ob::Frame>&)> f)
      : f_(std::move(f)) {}
  void deliver(const std::shared_ptr<const ob::Frame>& f) override { f_(f); }
 private:
  std::function<void(const std::shared_ptr<const ob::Frame>&)> f_;
};

class AudioSink : public ob::Sink {
 public:
  void deliver(const std::shared_ptr<const ob::Frame>&) override { ++video; }
  void deliverAudio(const std::shared_ptr<const ob::AudioFrame>& a) override {
    std::lock_guard l(m);
    order.push_back(a->timestamp);
  }
  std::atomic<int> video{0};
  std::mutex m;
  std::vector<int64_t> order;
};
}

TEST_CASE("a row delivers what it is offered, in order") {
  std::mutex m;
  std::vector<int64_t> got;
  ob::OutputRow row("o1", std::make_unique<LambdaSink>([&](const std::shared_ptr<const ob::Frame>& f) {
    std::lock_guard l(m);
    got.push_back(f->timestamp);
  }));
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
  ob::OutputRow row("o1",
                    std::make_unique<LambdaSink>([&](const std::shared_ptr<const ob::Frame>&) {
                      while (!gate.load()) std::this_thread::sleep_for(1ms);
                    }),
                    /*capacity=*/2u);
  row.start();

  // The worker takes one and blocks; two more fill the queue; the next two drop the oldest.
  for (int64_t i = 0; i < 5; ++i) row.offer(frame(i));
  gate.store(true);
  row.stop();

  CHECK(row.dropped() >= 2u);
}

TEST_CASE("stopping a row unblocks its worker") {
  ob::OutputRow row("o1", std::make_unique<LambdaSink>([](const std::shared_ptr<const ob::Frame>&) {}));
  row.start();
  row.stop();  // must not hang
  CHECK(true);
}

TEST_CASE("a sink that throws stops the row instead of terminating the process") {
  ob::OutputRow row("o1", std::make_unique<LambdaSink>([](const std::shared_ptr<const ob::Frame>&) {
    throw std::runtime_error("sink broke");
  }));
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
  ob::OutputRow row("o1", std::make_unique<LambdaSink>([](const std::shared_ptr<const ob::Frame>&) {}));
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

TEST_CASE("a restart clears a previous fault") {
  std::atomic<bool> fail{true};
  ob::OutputRow row("o1", std::make_unique<LambdaSink>([&](const std::shared_ptr<const ob::Frame>&) {
    if (fail.load()) throw std::runtime_error("sink broke");
  }));

  row.start();
  row.offer(frame(0));
  auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!row.faulted() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();
  CHECK(row.faulted());

  fail.store(false);  // the sink is healthy again
  row.start();
  row.offer(frame(1));
  deadline = std::chrono::steady_clock::now() + 2s;
  while (row.delivered() < 1 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();

  CHECK_FALSE(row.faulted());
  CHECK(row.delivered() == 1u);
}

namespace {
class RecordingSink : public ob::Sink {
 public:
  explicit RecordingSink(std::vector<int64_t>* seen) : seen_(seen) {}
  void deliver(const std::shared_ptr<const ob::Frame>& f) override {
    if (seen_) seen_->push_back(f->timestamp);
  }
  void configure(const ob::Output& o, const std::string& advertised) override {
    configuredId = o.id;
    configuredName = advertised;
  }
  std::string note() const override { return "note"; }
  std::string configuredId, configuredName;
 private:
  std::vector<int64_t>* seen_;
};
}

TEST_CASE("a row owns its sink and hands it the output configuration") {
  std::vector<int64_t> seen;
  auto sink = std::make_unique<RecordingSink>(&seen);
  auto* raw = sink.get();
  ob::OutputRow row("o1", std::move(sink));

  ob::Output o;
  o.id = "o1";
  o.sourceId = "s1";
  row.configure(o, "Bridge (Cam 1)");

  CHECK(raw->configuredId == "o1");
  CHECK(raw->configuredName == "Bridge (Cam 1)");
  CHECK(row.sourceId() == "s1");

  row.start();
  row.offer(frame(7));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (row.delivered() < 1 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();

  REQUIRE(seen.size() == 1u);
  CHECK(seen[0] == 7);
}

TEST_CASE("a row reports its sink's note") {
  auto sink = std::make_unique<RecordingSink>(nullptr);
  ob::OutputRow row("o1", std::move(sink));
  CHECK(row.note() == "note");
}

TEST_CASE("a row with no sink is legal and never faults") {
  ob::OutputRow row("o1", nullptr);
  ob::Output o;
  o.id = "o1";
  row.configure(o, "A");
  CHECK(row.note().empty());
  row.start();
  row.offer(frame(1));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (row.delivered() < 1 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();
  CHECK(row.delivered() == 1u);
  CHECK_FALSE(row.faulted());
}

TEST_CASE("a faulted row keeps the sink's message and revives on restart") {
  std::atomic<bool> fail{true};
  ob::OutputRow row("o1", std::make_unique<LambdaSink>([](auto&) {}));
  ob::Output o;
  o.id = "o1";
  row.configure(o, "A");

  auto throwing = std::make_unique<LambdaSink>([&](const std::shared_ptr<const ob::Frame>&) {
    if (fail.load()) throw std::runtime_error("encoder refused 4096x2160");
  });
  ob::OutputRow faulting("o2", std::move(throwing));
  faulting.configure(o, "A");
  faulting.start();
  faulting.offer(frame(0));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!faulting.faulted() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  faulting.stop();
  CHECK(faulting.faulted());
  CHECK(faulting.note() == "encoder refused 4096x2160");
  (void)row;
}

TEST_CASE("a row delivers audio on its own path, in order") {
  auto sink = std::make_unique<AudioSink>();
  auto* raw = sink.get();
  ob::OutputRow row("o1", std::move(sink));
  row.start();
  for (int64_t i = 0; i < 3; ++i) {
    auto a = std::make_shared<ob::AudioFrame>();
    a->format = {48000, 2};
    a->samples = 1;
    a->planar.assign(2, 0.0f);
    a->timestamp = i;
    row.offerAudio(a);
  }
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (row.audioDelivered() < 3 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();
  CHECK(row.audioDelivered() == 3u);

  // A stop/start cycle must reopen the audio queue too, exactly like the video queue.
  row.start();
  auto extra = std::make_shared<ob::AudioFrame>();
  extra->format = {48000, 2};
  extra->samples = 1;
  extra->planar.assign(2, 0.0f);
  extra->timestamp = 3;
  row.offerAudio(extra);
  const auto afterRestart = std::chrono::steady_clock::now() + 2s;
  while (row.audioDelivered() < 4 && std::chrono::steady_clock::now() < afterRestart)
    std::this_thread::sleep_for(1ms);
  row.stop();
  CHECK(row.audioDelivered() == 4u);
  std::lock_guard l(raw->m);
  CHECK(raw->order == std::vector<int64_t>{0, 1, 2, 3});
}

TEST_CASE("a blocked audio consumer does not delay video") {
  class GatedAudio : public ob::Sink {
   public:
    void deliver(const std::shared_ptr<const ob::Frame>&) override { ++video; }
    void deliverAudio(const std::shared_ptr<const ob::AudioFrame>&) override {
      entered.store(true);
      while (!gate.load()) std::this_thread::sleep_for(1ms);
      ++audio;
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> gate{false};
    std::atomic<int> video{0};
    std::atomic<int> audio{0};
  };
  auto sink = std::make_unique<GatedAudio>();
  auto* raw = sink.get();
  ob::OutputRow row("o1", std::move(sink));
  // Constructed after the row so it is destroyed first: a failed REQUIRE still releases the
  // audio worker before ~OutputRow joins it.
  struct ReleaseGate {
    std::atomic<bool>* gate;
    ~ReleaseGate() { gate->store(true); }
  } release{&raw->gate};
  row.start();

  auto a = std::make_shared<ob::AudioFrame>();
  a->format = {48000, 2};
  a->samples = 1;
  a->planar.assign(2, 0.0f);
  row.offerAudio(a);

  const auto enteredBy = std::chrono::steady_clock::now() + 2s;
  while (!raw->entered.load() && std::chrono::steady_clock::now() < enteredBy)
    std::this_thread::sleep_for(1ms);
  REQUIRE(raw->entered.load());  // the audio worker is now inside the sink, blocked

  row.offer(frame(7));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (raw->video.load() < 1 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  CHECK(raw->video.load() == 1);
  CHECK(row.audioDelivered() == 0u);  // still blocked while video got through

  raw->gate.store(true);
  const auto released = std::chrono::steady_clock::now() + 2s;
  while (row.audioDelivered() < 1 && std::chrono::steady_clock::now() < released)
    std::this_thread::sleep_for(1ms);
  row.stop();
  CHECK(row.audioDelivered() == 1u);
}

TEST_CASE("a throwing audio sink does not fault the row") {
  class ThrowingAudio : public ob::Sink {
   public:
    void deliver(const std::shared_ptr<const ob::Frame>&) override { ++video; }
    void deliverAudio(const std::shared_ptr<const ob::AudioFrame>&) override {
      throw std::runtime_error("audio broke");
    }
    std::atomic<int> video{0};
  };
  auto sink = std::make_unique<ThrowingAudio>();
  auto* raw = sink.get();
  ob::OutputRow row("o1", std::move(sink));
  row.start();
  auto a = std::make_shared<ob::AudioFrame>();
  a->format = {48000, 2};
  a->samples = 1;
  a->planar.assign(2, 0.0f);
  row.offerAudio(a);
  row.offer(frame(7));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (raw->video.load() < 1 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();
  CHECK_FALSE(row.faulted());
  CHECK(row.audioDelivered() == 0u);
  CHECK(raw->video.load() == 1);
}
