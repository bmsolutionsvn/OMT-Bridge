#pragma once
// The headless engine (design 6.1, 6.6): owns the graph, the bridge file and the control
// server. Closing the UI drops a control connection; it must never touch the graph.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "core/bridge_model.h"
#include "engine/bridge_graph.h"
#include "engine/command_router.h"
#include "engine/control_server.h"

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
  void apply(const Bridge& bridge);

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

  int controlPort_;
  std::string token_;
  std::string bridgeFile_;
  // Serialises command handling. ixwebsocket runs each connection on its own thread, so two
  // clients (or a supervisor restart overlapping a live UI) would otherwise race the router's
  // model/counter — and would call BridgeGraph::apply() from two threads, breaking its documented
  // one-owner-thread rule (it holds raw row/hub pointers across a deliberate unlock).
  std::mutex controlMu_;
  CommandRouter router_;
  BridgeGraph graph_{[](const std::shared_ptr<const Frame>&) {}};
  std::unique_ptr<ControlServer> server_;
  std::atomic<uint64_t> controlConnections_{0};
};

}  // namespace ob
