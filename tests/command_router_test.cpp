#include <doctest/doctest.h>

#include "engine/command_router.h"

TEST_CASE("an unknown command is refused with a named error") {
  ob::CommandRouter router;
  const auto reply = router.handle(ob::ClientId{"c1"}, R"({"cmd":"nope"})");
  CHECK(reply.find("\"ok\":false") != std::string::npos);
  CHECK(reply.find("unknown command") != std::string::npos);
}

TEST_CASE("input that is not JSON is refused, not crashed on") {
  ob::CommandRouter router;
  const auto reply = router.handle(ob::ClientId{"c1"}, "not json at all");
  CHECK(reply.find("\"ok\":false") != std::string::npos);
}

TEST_CASE("add-source rejects an address that is not text") {
  ob::CommandRouter router;
  const auto reply = router.handle(ob::ClientId{"c1"},
                                   R"({"cmd":"add-source","kind":"OMT","address":42})");
  CHECK(reply.find("\"ok\":false") != std::string::npos);
  CHECK(reply.find("address") != std::string::npos);
}

TEST_CASE("add-source rejects an unknown kind") {
  ob::CommandRouter router;
  const auto reply = router.handle(ob::ClientId{"c1"},
                                   R"({"cmd":"add-source","kind":"Telepathy","address":"x"})");
  CHECK(reply.find("\"ok\":false") != std::string::npos);
  CHECK(reply.find("kind") != std::string::npos);
}

TEST_CASE("a valid add-source returns ok and the new id") {
  ob::CommandRouter router;
  const auto reply = router.handle(ob::ClientId{"c1"},
                                   R"({"cmd":"add-source","kind":"NDI","address":"CAM 1"})");
  CHECK(reply.find("\"ok\":true") != std::string::npos);
  CHECK(reply.find("\"id\"") != std::string::npos);
}

TEST_CASE("state reports the model") {
  ob::CommandRouter router;
  router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-source","kind":"NDI","address":"CAM 1"})");
  const auto reply = router.handle(ob::ClientId{"c1"}, R"({"cmd":"state"})");
  CHECK(reply.find("\"ok\":true") != std::string::npos);
  CHECK(reply.find("CAM 1") != std::string::npos);
  // The symmetry that matters: what the reply writes is exactly what add-source accepts.
  CHECK(reply.find("\"kind\":\"NDI\"") != std::string::npos);
}

// A wrong-typed field must be refused, not throw: an exception here would escape a control-server
// thread and call std::terminate.
TEST_CASE("a wrong-typed field is refused rather than thrown on") {
  ob::CommandRouter router;
  const auto a = router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-output","sourceId":42})");
  CHECK(a.find("\"ok\":false") != std::string::npos);
  const auto b = router.handle(ob::ClientId{"c1"},
                               R"({"cmd":"set-output-source","id":"o1","sourceId":42})");
  CHECK(b.find("\"ok\":false") != std::string::npos);
  const auto c = router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-output","name":true})");
  CHECK(c.find("\"ok\":false") != std::string::npos);
}

TEST_CASE("add-output and set-output-source edit the model, and remove-source detaches dependents") {
  ob::CommandRouter router;
  router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-source","kind":"OMT","address":"PC"})");
  const auto made = router.handle(ob::ClientId{"c1"},
                                  R"({"cmd":"add-output","sourceId":"s1","name":"A"})");
  CHECK(made.find("\"ok\":true") != std::string::npos);

  const auto moved = router.handle(ob::ClientId{"c1"},
                                   R"({"cmd":"set-output-source","id":"o1","sourceId":""})");
  CHECK(moved.find("\"ok\":true") != std::string::npos);

  const auto removed = router.handle(ob::ClientId{"c1"}, R"({"cmd":"remove-source","id":"s1"})");
  CHECK(removed.find("\"ok\":true") != std::string::npos);
  CHECK(router.bridge().sources.empty());
  CHECK(router.bridge().outputs.size() == 1u);         // the row is kept...
  CHECK(router.bridge().outputs[0].sourceId.empty());  // ...with its source detached
}

TEST_CASE("state reports runtime liveness when the engine supplies it") {
  ob::CommandRouter router;
  router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-source","kind":"OMT","address":"PC"})");
  router.setRuntime(
      [](const std::string& id) -> std::optional<ob::SourceRuntime> {
        ob::SourceRuntime rt;
        rt.state = ob::SignalState::Live;
        rt.format = ob::VideoFormatView{1920, 1080, 60, 1, false, true};
        return id == "s1" ? std::optional<ob::SourceRuntime>(rt) : std::nullopt;
      },
      [](const std::string&) { return std::nullopt; });

  const auto reply = router.handle(ob::ClientId{"c1"}, R"({"cmd":"state"})");
  CHECK(reply.find("\"state\":\"live\"") != std::string::npos);
  CHECK(reply.find("\"width\":1920") != std::string::npos);
}

TEST_CASE("state reports output audio counters") {
  ob::CommandRouter router;
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});
  router.setBridge(b);
  router.setRuntime(
      [](const std::string&) { return std::nullopt; },
      [](const std::string& id) -> std::optional<ob::OutputRuntime> {
        ob::OutputRuntime rt;
        rt.state = ob::SignalState::Live;
        rt.delivered = 9;
        rt.dropped = 1;
        rt.audioDelivered = 5;
        return id == "o1" ? std::optional<ob::OutputRuntime>(rt) : std::nullopt;
      });

  const auto reply = router.handle(ob::ClientId{"c1"}, R"({"cmd":"state"})");
  CHECK(reply.find("\"audioDelivered\":5") != std::string::npos);
}

TEST_CASE("state omits runtime keys when the engine supplies nothing") {
  ob::CommandRouter router;
  router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-source","kind":"OMT","address":"PC"})");
  const auto reply = router.handle(ob::ClientId{"c1"}, R"({"cmd":"state"})");
  CHECK(reply.find("\"state\":") == std::string::npos);
}

TEST_CASE("state reports measured output rate and connections") {
  ob::CommandRouter router;
  ob::Bridge b;
  b.outputs.push_back(ob::Output{.id = "o1", .target = ob::OutputTarget::OmtHx, .name = "A"});
  router.setBridge(b);
  router.setRuntime(
      [](const std::string&) { return std::nullopt; },
      [](const std::string& id) -> std::optional<ob::OutputRuntime> {
        ob::OutputRuntime rt;
        rt.state = ob::SignalState::Live;
        rt.fps = 59.5;
        rt.mbps = 31.25;
        rt.connections = 2;
        return id == "o1" ? std::optional<ob::OutputRuntime>(rt) : std::nullopt;
      });

  const auto reply = router.handle(ob::ClientId{"c1"}, R"({"cmd":"state"})");
  CHECK(reply.find("\"fps\":59.5") != std::string::npos);
  CHECK(reply.find("\"mbps\":31.25") != std::string::npos);
  CHECK(reply.find("\"connections\":2") != std::string::npos);
}

TEST_CASE("setBridge seeds the id counter past a loaded model") {
  ob::CommandRouter router;
  ob::Bridge loaded;
  loaded.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  router.setBridge(loaded);

  const auto reply = router.handle(ob::ClientId{"c1"},
                                   R"({"cmd":"add-source","kind":"NDI","address":"CAM"})");
  CHECK(reply.find("\"id\":\"s2\"") != std::string::npos);  // not a duplicate "s1"
}
