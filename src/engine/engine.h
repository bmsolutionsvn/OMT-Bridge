#pragma once
// The headless engine (design 6.1, 6.6): owns the graph, the bridge file and the control
// server. Closing the UI drops a control connection; it must never touch the graph.
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "core/bridge_model.h"
#include "engine/bridge_graph.h"
#include "engine/command_router.h"
#include "engine/control_server.h"
#include "engine/omt_sink.h"
#include "engine/omt_source.h"

namespace ob {

class Engine {
 public:
  Engine(int controlPort, std::string token, std::string bridgeFile = {});
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Builds the graph from the model. Safe to call repeatedly, but it REPLACES the router's model
  // (discarding anything commands minted) and re-seeds the id counters — so it is a startup call:
  // run it on the same thread as start()/stop(), never from a command, adaptor or deliver thread.
  // Holds controlMu_ for its whole body, so it cannot interleave a command's apply.
  void apply(const Bridge& bridge);

  // A source's runtime view, or nullopt when no such source is configured. Reads the runtime map
  // and the hub, never the control mutex — safe from a reply thread.
  std::optional<SourceRuntime> sourceRuntime(const std::string& id) const;
  std::optional<OutputRuntime> outputRuntime(const std::string& id) const;

  // Starts the graph and, when controlPort > 0, the control server. On an AUTHORISED open the
  // server pushes a full `state` reply before any command, so a UI should treat its first message
  // as its initial snapshot rather than the answer to something it sent.
  void start();
  void stop();

  BridgeGraph& graph() { return graph_; }

  // Called when a UI disconnects. Deliberately does not stop the graph (design 6.1, 1.1).
  void onControlClientDisconnected(const ClientId& client);

  uint64_t controlConnections() const { return controlConnections_; }

 private:
  std::string handleCommand(const ClientId& client, const std::string& text);

  // The graph's running hook, called synchronously during apply() — i.e. with controlMu_ held when
  // apply() came from a command. It only installs the adapter transition callback.
  void onHubCreated(SourceHub& hub, const Source& source);
  // Reconciles router, adapters, runtime map and graph. controlMu_ held.
  void applyLocked(const Bridge& bridge);
  void startAdapter(const std::string& sourceId);  // controlMu_ held
  void stopAdapter(const std::string& sourceId);   // controlMu_ held

  int controlPort_;
  std::string token_;
  std::string bridgeFile_;
  // Serialises command handling. ixwebsocket runs each connection on its own thread, so two
  // clients (or a supervisor restart overlapping a live UI) would otherwise race the router's
  // model/counter — and would call BridgeGraph::apply() from two threads, breaking its no-concurrent-apply rule (it holds raw row/hub pointers across a deliberate unlock).
  std::mutex controlMu_;
  CommandRouter router_;
  // The model the graph was last reconciled against. Stored before graph_.apply() because the
  // running hook (startAdapter) reads it while apply() runs.
  Bridge model_;                                               // controlMu_
  std::map<std::string, std::unique_ptr<OmtSource>> adapters_;  // controlMu_
  // A state callback runs on an adapter thread, and stopAdapter() joins that adapter while
  // holding controlMu_, so this map must never be guarded by controlMu_.
  mutable std::mutex runtimeMu_;                                // guards sourceRuntime_
  std::map<std::string, SourceRuntime> sourceRuntime_;          // runtimeMu_
  BridgeGraph graph_;
  std::unique_ptr<ControlServer> server_;
  std::atomic<uint64_t> controlConnections_{0};
};

}  // namespace ob
