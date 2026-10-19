#include <doctest/doctest.h>

#include "engine/bridge_graph.h"

TEST_CASE("one source feeding two outputs counts two refs on one hub (N x M)") {
  ob::Bridge b;
  b.name = "B";
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});
  b.outputs.push_back(ob::Output{.id = "o2", .sourceId = "s1", .target = ob::OutputTarget::OmtHx, .name = "B"});

  ob::BridgeGraph graph([](const std::shared_ptr<const ob::Frame>&) {});
  graph.apply(b);

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

  ob::BridgeGraph graph([](const std::shared_ptr<const ob::Frame>&) {});
  graph.apply(b);
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

  ob::BridgeGraph graph([](const std::shared_ptr<const ob::Frame>&) {});
  graph.apply(b);

  CHECK(graph.rowCount() == 1u);
  CHECK(graph.hubCount() == 0u);
}

TEST_CASE("fan-out offers each published frame to every row that reads the source") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});
  b.outputs.push_back(ob::Output{.id = "o2", .sourceId = "s1", .target = ob::OutputTarget::Ndi, .name = "B"});

  ob::BridgeGraph graph([](const std::shared_ptr<const ob::Frame>&) {});
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
  ob::BridgeGraph graph([](const std::shared_ptr<const ob::Frame>&) {});
  auto f = std::make_shared<ob::Frame>();
  graph.publish("gone", f);  // must not throw
  CHECK(true);
}
