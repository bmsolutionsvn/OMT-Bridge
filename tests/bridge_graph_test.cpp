#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "engine/bridge_graph.h"

using namespace std::chrono_literals;

namespace {
std::unique_ptr<ob::Sink> nullSink(const ob::Output&, const std::string&) { return nullptr; }

class CountingSink : public ob::Sink {
 public:
  void deliver(const std::shared_ptr<const ob::Frame>&) override { ++count; }
  int count = 0;
};

class FaultOnceSink : public ob::Sink {
 public:
  void deliver(const std::shared_ptr<const ob::Frame>&) override {
    if (fail) throw std::runtime_error("open failed");
  }
  bool fail = true;
};
}  // namespace  // namespace

TEST_CASE("one source feeding two outputs counts two refs on one hub (N x M)") {
  ob::Bridge b;
  b.name = "B";
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});
  b.outputs.push_back(ob::Output{.id = "o2", .sourceId = "s1", .target = ob::OutputTarget::OmtHx, .name = "B"});

  ob::BridgeGraph graph(nullSink);
  graph.apply(b);
  graph.start();

  CHECK(graph.hubCount() == 1u);
  CHECK(graph.rowCount() == 2u);
  REQUIRE(graph.hub("s1") != nullptr);
  CHECK(graph.hub("s1")->refs() == 2);
  CHECK(graph.hub("s1")->running());
}

TEST_CASE("removing an output releases its ref, and an unreferenced source goes idle") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});

  ob::BridgeGraph graph(nullSink);
  graph.apply(b);
  graph.start();
  REQUIRE(graph.hub("s1")->refs() == 1);

  b.outputs.clear();
  graph.apply(b);

  CHECK(graph.rowCount() == 0u);
  CHECK(graph.hub("s1")->refs() == 0);
  CHECK_FALSE(graph.hub("s1")->running());
}

TEST_CASE("an output with no source is kept as an idle row") {
  ob::Bridge b;
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "", .target = ob::OutputTarget::Omt, .name = "A"});

  ob::BridgeGraph graph(nullSink);
  graph.apply(b);

  CHECK(graph.rowCount() == 1u);
  CHECK(graph.hubCount() == 0u);
}

TEST_CASE("fan-out offers each published frame to every row that reads the source") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});
  b.outputs.push_back(ob::Output{.id = "o2", .sourceId = "s1", .target = ob::OutputTarget::Ndi, .name = "B"});

  ob::BridgeGraph graph(nullSink);
  graph.apply(b);
  graph.start();

  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{4, 2, 60, 1, false};
  f->uyvy.assign(f->lumaBytes(), 0x10);
  graph.publish("s1", f);
  graph.stop();

  // publish() hands the frame to each row's queue; both rows were offered it.
  CHECK(graph.offeredToRow("o1") == 1u);
  CHECK(graph.offeredToRow("o2") == 1u);
}

TEST_CASE("publishing to a source that has no rows is a no-op") {
  ob::BridgeGraph graph(nullSink);
  auto f = std::make_shared<ob::Frame>();
  graph.publish("gone", f);  // must not throw
  CHECK(true);
}

// Re-applying an unchanged model must not churn demand: a reset-to-zero-and-re-add would fire a
// stop-then-start on every source adapter for any unrelated edit (Task 13 calls apply() per edit).
TEST_CASE("re-applying an unchanged model fires no running transitions") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});
  b.outputs.push_back(ob::Output{.id = "o2", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "B"});

  ob::BridgeGraph graph(nullSink);
  graph.apply(b);
  graph.start();

  std::atomic<int> transitions{0};
  graph.hub("s1")->setOnRunningChanged([&](bool) { transitions.fetch_add(1); });

  graph.apply(b);  // the identical model

  CHECK(transitions.load() == 0);
  CHECK(graph.hub("s1")->refs() == 2);
}

// The subtle refcount case: one row leaves one source and joins another.
TEST_CASE("an output that moves to another source moves its ref") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.sources.push_back(ob::Source{.id = "s2", .kind = ob::SourceKind::Omt, .address = "PC2"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});

  ob::BridgeGraph graph(nullSink);
  graph.apply(b);
  graph.start();
  REQUIRE(graph.hub("s1")->refs() == 1);

  b.outputs[0].sourceId = "s2";
  graph.apply(b);

  CHECK(graph.hub("s1")->refs() == 0);
  CHECK_FALSE(graph.hub("s1")->running());
  CHECK(graph.hub("s2")->refs() == 1);
  CHECK(graph.hub("s2")->running());
}

TEST_CASE("a new hub is hooked before demand, even while the graph is started") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});

  std::vector<int> seen;
  ob::BridgeGraph graph(nullSink, [&](ob::SourceHub& hub, const ob::Source&) {
    seen.push_back(hub.refs());
  });
  graph.apply(b);
  graph.start();

  b.sources.push_back(ob::Source{.id = "s2", .kind = ob::SourceKind::Omt, .address = "PC2"});
  b.outputs.push_back(ob::Output{.id = "o2", .sourceId = "s2", .target = ob::OutputTarget::Omt, .name = "B"});
  graph.apply(b);

  REQUIRE(seen.size() == 2u);
  CHECK(seen[0] == 0);
  CHECK(seen[1] == 0);                        // s2 hooked before it received demand
  CHECK(graph.hub("s2")->refs() == 1);
  CHECK(graph.hub("s2")->running());
  graph.stop();
}

TEST_CASE("no adapter is demanded before start, and stop releases it") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});

  std::vector<bool> transitions;
  ob::BridgeGraph graph(nullSink, [&](ob::SourceHub& hub, const ob::Source&) {
    hub.setOnRunningChanged([&](bool on) { transitions.push_back(on); });
  });
  graph.apply(b);
  CHECK(graph.hub("s1")->refs() == 0);
  CHECK_FALSE(graph.hub("s1")->running());

  graph.start();
  CHECK(graph.hub("s1")->refs() == 1);
  CHECK(graph.hub("s1")->running());

  graph.stop();
  CHECK(graph.hub("s1")->refs() == 0);
  CHECK_FALSE(graph.hub("s1")->running());
  REQUIRE(transitions.size() == 2u);
  CHECK(transitions[0] == true);
  CHECK(transitions[1] == false);
}

TEST_CASE("demand configured while stopped is applied by start") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});

  ob::BridgeGraph graph(nullSink);
  graph.apply(b);                       // stopped
  b.outputs.push_back(ob::Output{.id = "o2", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "B"});
  graph.apply(b);                       // still stopped
  CHECK(graph.hub("s1")->refs() == 0);

  graph.start();
  CHECK(graph.hub("s1")->refs() == 2);
  CHECK(graph.hub("s1")->running());
  graph.stop();
}

TEST_CASE("removing the last output while stopped leaves nothing running after start") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});

  ob::BridgeGraph graph(nullSink);
  graph.apply(b);
  graph.start();
  graph.stop();

  b.outputs.clear();
  graph.apply(b);                       // stopped; stale demand must not survive
  graph.start();
  CHECK(graph.rowCount() == 0u);
  REQUIRE(graph.hub("s1") != nullptr);
  CHECK(graph.hub("s1")->refs() == 0);
  CHECK_FALSE(graph.hub("s1")->running());
  graph.stop();
}

TEST_CASE("start and stop are idempotent") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});

  ob::BridgeGraph graph(nullSink);
  graph.apply(b);
  graph.start();
  graph.start();
  CHECK(graph.hub("s1")->refs() == 1);
  graph.stop();
  graph.stop();
  CHECK(graph.hub("s1")->refs() == 0);
  CHECK_FALSE(graph.hub("s1")->running());
}

TEST_CASE("removing a running source stops its adapter before the hub dies") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});

  std::vector<bool> transitions;
  ob::BridgeGraph graph(nullSink, [&](ob::SourceHub& hub, const ob::Source&) {
    hub.setOnRunningChanged([&](bool on) { transitions.push_back(on); });
  });
  graph.apply(b);
  graph.start();
  REQUIRE(transitions.size() == 1u);

  b.sources.clear();
  b.outputs.clear();
  graph.apply(b);                       // must fire the stop before destroying the hub

  REQUIRE(transitions.size() == 2u);
  CHECK(transitions[0] == true);
  CHECK(transitions[1] == false);
  CHECK(graph.hub("s1") == nullptr);
  CHECK(graph.rowCount() == 0u);
}

TEST_CASE("clearing an output's source stops the row and clears its liveness") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Ndi, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Ndi, .name = "A"});

  ob::BridgeGraph graph([](const ob::Output&, const std::string&) -> std::unique_ptr<ob::Sink> {
    return std::make_unique<CountingSink>();
  });
  graph.apply(b);
  graph.start();

  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{4, 2, 60, 1, false};
  f->uyvy.assign(f->lumaBytes(), 0x10);
  graph.publish("s1", f);

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!(graph.rowStats("o1") && graph.rowStats("o1")->live) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  REQUIRE(graph.rowStats("o1").has_value());
  CHECK(graph.rowStats("o1")->live);

  b.outputs[0].sourceId = "";
  graph.apply(b);

  REQUIRE(graph.rowStats("o1").has_value());
  CHECK_FALSE(graph.rowStats("o1")->live);
  graph.stop();
}

TEST_CASE("changing an output's configuration revives a faulted row") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Ndi, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Ndi, .name = "A"});

  ob::BridgeGraph graph([](const ob::Output&, const std::string&) -> std::unique_ptr<ob::Sink> {
    return std::make_unique<FaultOnceSink>();
  });
  graph.apply(b);
  graph.start();

  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{4, 2, 60, 1, false};
  f->uyvy.assign(f->lumaBytes(), 0x10);
  graph.publish("s1", f);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!(graph.rowStats("o1") && graph.rowStats("o1")->faulted) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  REQUIRE(graph.rowStats("o1").has_value());
  CHECK(graph.rowStats("o1")->faulted);

  b.outputs[0].name = "B";  // operator fixes the configuration
  graph.apply(b);
  CHECK_FALSE(graph.rowStats("o1")->faulted);
  graph.stop();
}
