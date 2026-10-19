#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <vector>

#include "engine/bridge_graph.h"

namespace {
std::unique_ptr<ob::Sink> nullSink(const ob::Output&, const std::string&) { return nullptr; }
}

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

TEST_CASE("a hub-created hook runs before the hub is given any demand") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});

  int installedAtRefs = -1;
  ob::BridgeGraph graph(nullSink, [&](ob::SourceHub& hub, const ob::Source&) {
    hub.setOnRunningChanged([&](bool) {});
    installedAtRefs = hub.refs();
  });
  graph.apply(b);

  CHECK(installedAtRefs == 0);
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

TEST_CASE("an unchanged re-apply still fires no transition") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});

  std::atomic<int> transitions{0};
  ob::BridgeGraph graph(nullSink);
  graph.apply(b);
  graph.start();
  graph.hub("s1")->setOnRunningChanged([&](bool) { transitions.fetch_add(1); });

  graph.apply(b);
  CHECK(transitions.load() == 0);
}
