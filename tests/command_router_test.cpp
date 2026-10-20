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

TEST_CASE("set-output edits a row and refuses an unknown one") {
  ob::CommandRouter router;
  router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-source","kind":"OMT","address":"PC"})");
  router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-output","sourceId":"s1","name":"A"})");

  const auto reply = router.handle(
      ob::ClientId{"c1"},
      R"({"cmd":"set-output","id":"o1","name":"Program","target":"NDI","codec":"H.264","quality":"High","alpha":"Off","sourceId":""})");
  CHECK(reply.find("\"ok\":true") != std::string::npos);
  REQUIRE(router.bridge().outputs.size() == 1u);
  CHECK(router.bridge().outputs[0].name == "Program");
  CHECK(static_cast<int>(router.bridge().outputs[0].target) == static_cast<int>(ob::OutputTarget::Ndi));
  CHECK(static_cast<int>(router.bridge().outputs[0].codec) == static_cast<int>(ob::HxCodec::H264));
  CHECK(static_cast<int>(router.bridge().outputs[0].quality) == static_cast<int>(ob::Quality::High));
  CHECK(static_cast<int>(router.bridge().outputs[0].alpha) == static_cast<int>(ob::AlphaMode::Off));
  CHECK(router.bridge().outputs[0].sourceId.empty());

  const auto missing =
      router.handle(ob::ClientId{"c1"}, R"({"cmd":"set-output","id":"o9","name":"X"})");
  CHECK(missing.find("\"ok\":false") != std::string::npos);
  CHECK(missing.find("no such output") != std::string::npos);
  CHECK(router.bridge().outputs.size() == 1u);  // nothing minted
}

TEST_CASE("a bad enum in set-output is named and changes nothing") {
  ob::CommandRouter router;
  router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-output","name":"A"})");
  const auto reply =
      router.handle(ob::ClientId{"c1"}, R"({"cmd":"set-output","id":"o1","target":"OMTX","name":"B"})");
  CHECK(reply.find("\"ok\":false") != std::string::npos);
  CHECK(reply.find("OMTX") != std::string::npos);
  CHECK(router.bridge().outputs[0].name == "A");  // the valid field was not applied either
  CHECK(static_cast<int>(router.bridge().outputs[0].target) == static_cast<int>(ob::OutputTarget::Omt));
}

TEST_CASE("remove-output removes the row and refuses the second time") {
  ob::CommandRouter router;
  router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-output","name":"A"})");
  const auto first = router.handle(ob::ClientId{"c1"}, R"({"cmd":"remove-output","id":"o1"})");
  CHECK(first.find("\"ok\":true") != std::string::npos);
  CHECK(router.bridge().outputs.empty());
  const auto second = router.handle(ob::ClientId{"c1"}, R"({"cmd":"remove-output","id":"o1"})");
  CHECK(second.find("\"ok\":false") != std::string::npos);
  CHECK(second.find("no such output") != std::string::npos);
}

TEST_CASE("set-source edits kind, address and codec, and rejects an unknown id") {
  ob::CommandRouter router;
  router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-source","kind":"OMT","address":"PC"})");

  const auto reply = router.handle(
      ob::ClientId{"c1"},
      R"j({"cmd":"set-source","id":"s1","kind":"OMT-HX","address":"PC (Cam)","codec":"H.264"})j");
  CHECK(reply.find("\"ok\":true") != std::string::npos);
  CHECK(static_cast<int>(router.bridge().sources[0].kind) == static_cast<int>(ob::SourceKind::OmtHx));
  CHECK(router.bridge().sources[0].address == "PC (Cam)");
  REQUIRE(router.bridge().sources[0].codec.has_value());
  CHECK(static_cast<int>(*router.bridge().sources[0].codec) == static_cast<int>(ob::HxCodec::H264));

  const auto cleared = router.handle(ob::ClientId{"c1"}, R"({"cmd":"set-source","id":"s1","codec":null})");
  CHECK(cleared.find("\"ok\":true") != std::string::npos);
  CHECK_FALSE(router.bridge().sources[0].codec.has_value());

  const auto missing =
      router.handle(ob::ClientId{"c1"}, R"({"cmd":"set-source","id":"s9","address":"X"})");
  CHECK(missing.find("no such source") != std::string::npos);

  const auto bad = router.handle(ob::ClientId{"c1"}, R"({"cmd":"set-source","id":"s1","kind":"?"})");
  CHECK(bad.find("\"ok\":false") != std::string::npos);
  const auto emptyAddress =
      router.handle(ob::ClientId{"c1"}, R"({"cmd":"set-source","id":"s1","address":""})");
  CHECK(emptyAddress.find("\"ok\":false") != std::string::npos);
}

TEST_CASE("set-settings round-trips through state and rejects bad values") {
  ob::CommandRouter router;
  router.handle(
      ob::ClientId{"c1"},
      R"({"cmd":"set-settings","defaultTarget":"NDI","defaultQuality":"High","thumbnailIntervalMs":500,"startEngineWithApp":true})");

  const auto state = router.handle(ob::ClientId{"c1"}, R"({"cmd":"state"})");
  CHECK(state.find("\"defaultTarget\":\"NDI\"") != std::string::npos);
  CHECK(state.find("\"defaultQuality\":\"High\"") != std::string::npos);
  CHECK(state.find("\"thumbnailIntervalMs\":500") != std::string::npos);
  CHECK(state.find("\"startEngineWithApp\":true") != std::string::npos);

  const auto badTarget =
      router.handle(ob::ClientId{"c1"}, R"({"cmd":"set-settings","defaultTarget":"OMTX"})");
  CHECK(badTarget.find("\"ok\":false") != std::string::npos);
  const auto badInterval =
      router.handle(ob::ClientId{"c1"}, R"({"cmd":"set-settings","thumbnailIntervalMs":-1})");
  CHECK(badInterval.find("\"ok\":false") != std::string::npos);
  // The refusals changed nothing.
  CHECK(static_cast<int>(router.bridge().settings.defaultTarget) == static_cast<int>(ob::OutputTarget::Ndi));
  CHECK(router.bridge().settings.thumbnailIntervalMs == 500);
}

TEST_CASE("discover returns the provider's names, and empty lists without one") {
  ob::CommandRouter router;
  const auto bare = router.handle(ob::ClientId{"c1"}, R"({"cmd":"discover"})");
  CHECK(bare.find("\"ok\":true") != std::string::npos);
  CHECK(bare.find("\"omt\":[]") != std::string::npos);
  CHECK(bare.find("\"ndi\":[]") != std::string::npos);

  router.setDiscovery([] {
    ob::Discovery d;
    d.omt = {"PC (Cam)"};
    d.ndi = {"MACHINE (Camera)"};
    return d;
  });
  const auto found = router.handle(ob::ClientId{"c1"}, R"({"cmd":"discover"})");
  CHECK(found.find("PC (Cam)") != std::string::npos);
  CHECK(found.find("MACHINE (Camera)") != std::string::npos);
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
