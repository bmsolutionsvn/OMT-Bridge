#include <doctest/doctest.h>

#include <chrono>
#include <thread>

#include "engine/engine.h"

using namespace std::chrono_literals;

static ob::Bridge twoOutputsOneSource() {
  ob::Bridge b;
  b.name = "Test";
  // NDI: no adapter and no socket, and NDI targets keep OmtSink::configure from constructing a
// sender, so these tests never touch the network.
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Ndi, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Ndi, .name = "A"});
  b.outputs.push_back(ob::Output{.id = "o2", .sourceId = "s1", .target = ob::OutputTarget::Ndi, .name = "B"});
  return b;
}

TEST_CASE("applying a bridge starts rows and refs the hub") {
  ob::Engine engine(0, "token", {}, false);  // no devices in a unit test  // port 0: do not bind, tests drive apply() directly
  engine.apply(twoOutputsOneSource());
  REQUIRE(engine.graph().hub("s1") != nullptr);
  // Demand is gated on start(): configuring must not open an adapter.
  CHECK(engine.graph().hub("s1")->refs() == 0);
  CHECK_FALSE(engine.graph().hub("s1")->running());
  engine.start();
  CHECK(engine.graph().hub("s1")->refs() == 2);
  CHECK(engine.graph().rowCount() == 2u);
  engine.stop();
}

TEST_CASE("outputs survive a control-channel disconnect (the UI-close guarantee)") {
  ob::Engine engine(0, "token", {}, false);  // no devices in a unit test
  engine.apply(twoOutputsOneSource());
  engine.start();

  engine.onControlClientDisconnected(ob::ClientId{"c1"});
  std::this_thread::sleep_for(10ms);

  // The graph is owned by the engine, not by the connection, so a disconnect must leave demand and
  // rows exactly as they were. (framesPublished cannot move here — M1 has no adapter publishing —
  // so assert the invariant that can actually fail if a disconnect tore the graph down.)
  CHECK(engine.graph().hub("s1")->running());
  CHECK(engine.graph().hub("s1")->refs() == 2);
  CHECK(engine.graph().rowCount() == 2u);
  engine.stop();
}

TEST_CASE("runtime state is Ready when configured, and after the last output leaves") {
  ob::Engine engine(0, "token", {}, false);  // no devices in a unit test
  auto b = twoOutputsOneSource();  // NDI: deterministic, no socket
  engine.apply(b);
  auto rt = engine.sourceRuntime("s1");
  REQUIRE(rt.has_value());
  CHECK(static_cast<int>(rt->state) == static_cast<int>(ob::SignalState::Ready));

  engine.start();  // demand arrives: the NDI branch marks it Offline
  rt = engine.sourceRuntime("s1");
  REQUIRE(rt.has_value());
  CHECK(static_cast<int>(rt->state) == static_cast<int>(ob::SignalState::Offline));

  b.outputs.clear();
  engine.apply(b);  // demand drops
  rt = engine.sourceRuntime("s1");
  REQUIRE(rt.has_value());
  CHECK(static_cast<int>(rt->state) == static_cast<int>(ob::SignalState::Ready));

  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Ndi, .name = "A"});
  engine.apply(b);  // demand returns
  rt = engine.sourceRuntime("s1");
  REQUIRE(rt.has_value());
  CHECK(static_cast<int>(rt->state) == static_cast<int>(ob::SignalState::Offline));
  engine.stop();
}

TEST_CASE("editing a configured source restarts its adapter decision") {
  ob::Engine engine(0, "token", {}, false);  // no devices in a unit test
  auto b = twoOutputsOneSource();  // NDI source, NDI outputs: no sockets
  engine.apply(b);
  engine.start();
  CHECK(engine.adapterStarts() == 1u);

  b.sources[0].address = "PC2";
  engine.apply(b);  // changed source: stop + start decision
  CHECK(engine.adapterStarts() == 2u);
  CHECK(engine.graph().hub("s1")->refs() == 2);
  engine.stop();
}

TEST_CASE("the first rate sample reports no rate") {
  ob::Engine engine(0, "token", {}, false);  // no devices in a unit test
  engine.apply(twoOutputsOneSource());
  engine.start();

  const auto rt = engine.outputRuntime("o1");
  REQUIRE(rt.has_value());
  CHECK(rt->fps == 0.0);
  CHECK(rt->mbps == 0.0);
  engine.stop();
}

TEST_CASE("the running switch stops and restarts the graph without losing rows") {
  ob::Engine engine(0, "token", {}, false);  // no devices in a unit test
  engine.apply(twoOutputsOneSource());
  engine.start();
  REQUIRE(engine.graph().hub("s1") != nullptr);
  CHECK(engine.graph().hub("s1")->refs() == 2);

  engine.setRunning(false);
  CHECK_FALSE(engine.graph().hub("s1")->running());
  CHECK(engine.graph().rowCount() == 2u);  // the model and the rows are kept

  engine.setRunning(true);
  CHECK(engine.graph().hub("s1")->running());
  CHECK(engine.graph().hub("s1")->refs() == 2);
  engine.stop();
}

TEST_CASE("adding the first output reopens a preview-only NDI source") {
  ob::Engine engine(0, "token", {}, false);  // no devices: adapterStarts counts the decisions
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Ndi, .address = "PC"});
  engine.apply(b);
  engine.start();

  engine.graph().setThumbnailsWanted(true);  // a connected UI wants a preview
  REQUIRE(engine.graph().hub("s1") != nullptr);
  CHECK(engine.graph().hub("s1")->running());
  CHECK(engine.adapterStarts() == 1u);

  b.outputs.push_back(
      ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Ndi, .name = "A"});
  engine.apply(b);
  CHECK(engine.adapterStarts() == 2u);  // reopened at full bandwidth for the output

  b.outputs.clear();
  engine.apply(b);
  CHECK(engine.adapterStarts() == 3u);  // back to the low-bandwidth preview receiver

  engine.graph().setThumbnailsWanted(false);
  CHECK_FALSE(engine.graph().hub("s1")->running());
  engine.stop();
}

TEST_CASE("adding the first output reopens a preview-only OMT-HX source") {
  ob::Engine engine(0, "token", {}, false);
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::OmtHx, .address = "PC (Cam)"});
  engine.apply(b);
  engine.start();

  engine.graph().setThumbnailsWanted(true);
  REQUIRE(engine.graph().hub("s1") != nullptr);
  CHECK(engine.graph().hub("s1")->running());
  CHECK(engine.adapterStarts() == 1u);

  b.outputs.push_back(
      ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::OmtHx, .name = "A"});
  engine.apply(b);
  CHECK(engine.adapterStarts() == 2u);  // reopened at full size for the output

  b.outputs.clear();
  engine.apply(b);
  CHECK(engine.adapterStarts() == 3u);  // back to the small preview stream

  engine.graph().setThumbnailsWanted(false);
  CHECK_FALSE(engine.graph().hub("s1")->running());
  engine.stop();
}

#ifdef OB_WITH_NDI
TEST_CASE("an NDI source starts a real adapter and reports Connecting") {
  ob::Engine engine(0, "token");  // adapters enabled: a real NdiSource
  ob::Bridge b;
  b.name = "NDI Test";
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Ndi, .address = "no-such-source"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Ndi, .name = "A"});
  engine.apply(b);
  engine.start();

  auto rt = engine.sourceRuntime("s1");
  REQUIRE(rt.has_value());
  CHECK(static_cast<int>(rt->state) == static_cast<int>(ob::SignalState::Connecting));
  engine.stop();
}
#endif
