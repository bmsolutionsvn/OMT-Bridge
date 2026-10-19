#include <doctest/doctest.h>

#include <string>

#include "core/signal_state.h"

TEST_CASE("the four signal states serialise to their lowercase wire names") {
  CHECK(std::string(ob::toString(ob::SignalState::Ready)) == "ready");
  CHECK(std::string(ob::toString(ob::SignalState::Connecting)) == "connecting");
  CHECK(std::string(ob::toString(ob::SignalState::Live)) == "live");
  CHECK(std::string(ob::toString(ob::SignalState::Offline)) == "offline");
}

TEST_CASE("an absent error serialises to JSON null and a present one to an object") {
  CHECK(ob::errorToJson(std::nullopt).is_null());
  const auto j = ob::errorToJson(ob::ErrorInfo{"ndi-failed", "no runtime"});
  CHECK(j["code"] == "ndi-failed");
  CHECK(j["message"] == "no runtime");
}
