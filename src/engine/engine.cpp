#include "engine/engine.h"

#include <utility>

#include "core/bridge_store.h"

namespace ob {

Engine::Engine(int controlPort, std::string token, std::string bridgeFile)
    : controlPort_(controlPort), token_(std::move(token)), bridgeFile_(std::move(bridgeFile)) {}

Engine::~Engine() { stop(); }

void Engine::apply(const Bridge& bridge) {
  router_.setBridge(bridge);
  graph_.apply(bridge);
}

void Engine::start() {
  graph_.start();
  if (controlPort_ > 0 && server_ == nullptr) {
    server_ = std::make_unique<ControlServer>(controlPort_, token_);
    ControlServer::Handlers h;
    h.open = [this](const ClientId& c) {
      ++controlConnections_;
      if (server_) server_->send(c, handleCommand(c, R"({"cmd":"state"})"));
    };
    h.close = [this](const ClientId& c) { onControlClientDisconnected(c); };
    h.text = [this](const ClientId& c, const std::string& t) { return handleCommand(c, t); };
    server_->setHandlers(std::move(h));
    server_->start();
  }
}

void Engine::stop() {
  if (server_) {
    server_->stop();
    server_.reset();
  }
  graph_.stop();
}

void Engine::onControlClientDisconnected(const ClientId&) {
  // The graph is owned by the engine, not by a connection. Nothing to do, by design.
}

std::string Engine::handleCommand(const ClientId& client, const std::string& text) {
  // Commands arrive on per-connection ixwebsocket threads. Serialise the model edit together with
  // the reconcile for two reasons: the router's model and id counter are plain members, and
  // BridgeGraph::apply() is the single writer of hub demand — it holds raw row/hub pointers across
  // a deliberate unlock, so a second apply() from another thread could dangle them.
  std::lock_guard l(controlMu_);
  const std::string reply = router_.handle(client, text);
  // Every command may have edited the router's model (add-source, remove-source, add-output, …),
  // so reconcile the graph with it here. Applying unconditionally needs no per-command list:
  // apply() moves demand by delta, so an unchanged model costs nothing and fires no transition.
  // Without this the graph never learns about an edit, and nothing would actually start or stop.
  graph_.apply(router_.bridge());
  return reply;
}

}  // namespace ob
