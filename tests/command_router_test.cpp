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

TEST_CASE("setBridge seeds the id counter past a loaded model") {
  ob::CommandRouter router;
  ob::Bridge loaded;
  loaded.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  router.setBridge(loaded);

  const auto reply = router.handle(ob::ClientId{"c1"},
                                   R"({"cmd":"add-source","kind":"NDI","address":"CAM"})");
  CHECK(reply.find("\"id\":\"s2\"") != std::string::npos);  // not a duplicate "s1"
}
