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
