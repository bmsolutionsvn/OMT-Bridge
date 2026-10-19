#include <doctest/doctest.h>

#include <chrono>
#include <thread>

#include "engine/engine.h"

using namespace std::chrono_literals;

static ob::Bridge twoOutputsOneSource() {
  ob::Bridge b;
  b.name = "Test";
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});
  b.outputs.push_back(ob::Output{.id = "o2", .sourceId = "s1", .target = ob::OutputTarget::OmtHx, .name = "B"});
  return b;
}

TEST_CASE("applying a bridge starts rows and refs the hub") {
  ob::Engine engine(0, "token");  // port 0: do not bind, tests drive apply() directly
  engine.apply(twoOutputsOneSource());
  REQUIRE(engine.graph().hub("s1") != nullptr);
  CHECK(engine.graph().hub("s1")->refs() == 2);
  CHECK(engine.graph().rowCount() == 2u);
}

TEST_CASE("outputs survive a control-channel disconnect (the UI-close guarantee)") {
  ob::Engine engine(0, "token");
  engine.apply(twoOutputsOneSource());
  engine.start();

  const uint64_t before = engine.graph().hub("s1")->framesPublished();
  engine.onControlClientDisconnected(ob::ClientId{"c1"});
  std::this_thread::sleep_for(10ms);
  const uint64_t after = engine.graph().hub("s1")->framesPublished();

  CHECK(after == before);  // a disconnect never tears the graph down
  CHECK(engine.graph().rowCount() == 2u);
  engine.stop();
}
