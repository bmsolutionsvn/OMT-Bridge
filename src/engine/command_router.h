#pragma once
// Validates the JSON commands the UI sends over the control channel and answers each with a
// JSON string (design 6.1). Validation failures are named and inline; the router never
// throws to its caller.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "core/bridge_model.h"
#include "core/signal_state.h"
#include "engine/control_server.h"

namespace ob {

// The runtime half of a source row, as far as a state reply needs it. Kept here rather than in
// core/bridge_model.h so the router's reply shape does not force the model to carry liveness.
struct SourceRuntime {
  SignalState state = SignalState::Ready;
  VideoFormatView format;
};

struct OutputRuntime {
  SignalState state = SignalState::Ready;
  std::string note;
  uint64_t delivered = 0;
  uint64_t dropped = 0;
  uint64_t audioDelivered = 0;
};

// Return nullopt for an id with no runtime row; the reply then carries no runtime keys at all.
using SourceRuntimeFn = std::function<std::optional<SourceRuntime>(const std::string& id)>;
using OutputRuntimeFn = std::function<std::optional<OutputRuntime>(const std::string& id)>;

class CommandRouter {
 public:
  // Wholly synchronous: no engine threads, so the tests can call it directly.
  std::string handle(const ClientId& client, const std::string& text);

  const Bridge& bridge() const { return bridge_; }
  // Loads a saved model. Seeds the id counter past every id already present, so a later
  // add-source/add-output cannot mint a duplicate id (which would make findSource/remove-source
  // act on the wrong row, and make a state reply list two rows with one id).
  void setBridge(Bridge b);

  // Runtime liveness is supplied by the engine; the router only formats it.
  void setRuntime(SourceRuntimeFn sources, OutputRuntimeFn outputs);

 private:
  Bridge bridge_;
  SourceRuntimeFn sourceRuntime_;
  OutputRuntimeFn outputRuntime_;
  // Sources and outputs number independently (s1, s2, … and o1, o2, …): the prefixes exist
  // precisely to tell the two kinds apart, so a shared counter would mint "o2" for a first output.
  uint64_t nextSourceId_ = 1;
  uint64_t nextOutputId_ = 1;
};

}  // namespace ob
