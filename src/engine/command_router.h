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
  void setBridge(Bridge b) { bridge_ = std::move(b); }

 private:
  Bridge bridge_;
  uint64_t nextId_ = 1;
};

}  // namespace ob
