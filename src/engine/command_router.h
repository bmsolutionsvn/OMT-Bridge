#pragma once
// Validates the JSON commands the UI sends over the control channel and answers each with a
// JSON string (design 6.1). Validation failures are named and inline; the router never
// throws to its caller.
#include <string>

#include "core/bridge_model.h"
#include "engine/control_server.h"

namespace ob {

class CommandRouter {
 public:
  // Wholly synchronous: no engine threads, so the tests can call it directly.
  std::string handle(const ClientId& client, const std::string& text);

  const Bridge& bridge() const { return bridge_; }
  // Loads a saved model. Seeds the id counter past every id already present, so a later
  // add-source/add-output cannot mint a duplicate id (which would make findSource/remove-source
  // act on the wrong row, and make a state reply list two rows with one id).
  void setBridge(Bridge b);

 private:
  Bridge bridge_;
  // Sources and outputs number independently (s1, s2, … and o1, o2, …): the prefixes exist
  // precisely to tell the two kinds apart, so a shared counter would mint "o2" for a first output.
  uint64_t nextSourceId_ = 1;
  uint64_t nextOutputId_ = 1;
};

}  // namespace ob
