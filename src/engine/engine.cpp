#include "engine/engine.h"

#include <exception>
#include <utility>

#include "core/bridge_store.h"

namespace ob {

Engine::Engine(int controlPort, std::string token, std::string bridgeFile)
    : controlPort_(controlPort),
      token_(std::move(token)),
      bridgeFile_(std::move(bridgeFile)),
      graph_(
          [](const Output& o, const std::string& advertised) -> std::unique_ptr<Sink> {
            auto sink = std::make_unique<OmtSink>();
            sink->configure(o, advertised);
            return sink;
          },
          [this](SourceHub& hub, const Source& s) { onHubCreated(hub, s); }) {
  router_.setRuntime([this](const std::string& id) { return sourceRuntime(id); },
                     [this](const std::string& id) { return outputRuntime(id); });
}

Engine::~Engine() { stop(); }

void Engine::apply(const Bridge& bridge) {
  router_.setBridge(bridge);
  model_ = bridge;  // before apply(): the running hook reads it
  graph_.apply(model_);
}

void Engine::onHubCreated(SourceHub& hub, const Source& source) {
  // Runs with controlMu_ held (command path) or from start()/stop(), which take it too. The
  // callback itself fires later, from the demand path: startAdapter/stopAdapter must not take
  // controlMu_ — they are already called under it.
  hub.setOnRunningChanged([this, id = source.id](bool on) {
    if (on)
      startAdapter(id);
    else
      stopAdapter(id);
  });
}

void Engine::startAdapter(const std::string& id) {
  if (adapters_.count(id) != 0) return;
  const Source* s = model_.findSource(id);
  if (s == nullptr) return;  // removed while stopping
  if (s->kind == SourceKind::Ndi) {
    // NDI has no adapter in M2: report it offline rather than silently Ready forever.
    std::lock_guard l(runtimeMu_);
    sourceRuntime_[id].state = SignalState::Offline;
    return;
  }
  auto src = std::make_unique<OmtSource>(
      s->address,
      [this, id](std::shared_ptr<Frame> f) { graph_.publish(id, std::move(f)); },
      [this, id](SignalState st, std::optional<ErrorInfo>) {
        // Adapter thread: own mutex only, never controlMu_ (stopAdapter below joins this thread
        // while holding it).
        std::lock_guard l(runtimeMu_);
        sourceRuntime_[id].state = st;
      });
  adapters_[id] = std::move(src);
  adapters_[id]->start();
}

void Engine::stopAdapter(const std::string& id) { adapters_.erase(id); }

std::optional<SourceRuntime> Engine::sourceRuntime(const std::string& id) const {
  SourceRuntime rt;
  {
    std::lock_guard l(runtimeMu_);
    const auto it = sourceRuntime_.find(id);
    if (it == sourceRuntime_.end()) return std::nullopt;
    rt.state = it->second.state;
  }
  // Released runtimeMu_ before the graph lock: an adapter's state callback takes runtimeMu_ and
  // may run while a hub transition is in flight, so hold the two locks one at a time only.
  SourceHub* hub = graph_.hub(id);
  if (hub != nullptr) {
    if (const auto latest = hub->latest()) {
      rt.format = FormatView{latest->format.width, latest->format.height, latest->format.fpsN,
                             latest->format.fpsD, latest->format.alpha, true};
    }
  }
  return rt;
}

std::optional<OutputRuntime> Engine::outputRuntime(const std::string& id) const {
  const auto stats = graph_.rowStats(id);
  if (!stats) return std::nullopt;
  OutputRuntime rt;
  rt.state = stats->faulted ? SignalState::Offline
                            : (stats->delivered > 0 ? SignalState::Live : SignalState::Ready);
  rt.note = stats->note;
  rt.delivered = stats->delivered;
  rt.dropped = stats->dropped;
  return rt;
}

void Engine::start() {
  // The hook installed during apply()/start() mutates adapters_ on whichever thread calls this.
  std::lock_guard l(controlMu_);
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
    server_->stop();  // handlers have joined by here
    server_.reset();
  }
  std::lock_guard l(controlMu_);
  graph_.stop();
  adapters_.clear();
}

void Engine::onControlClientDisconnected(const ClientId&) {
  // The graph is owned by the engine, not by a connection. Nothing to do, by design.
}

std::string Engine::handleCommand(const ClientId& client, const std::string& text) {
  // Commands arrive on per-connection ixwebsocket threads. Serialise the model edit together with
  // the reconcile for two reasons: the router's model and id counters are plain members, and
  // BridgeGraph::apply() is the single writer of hub demand — it holds raw row/hub pointers across
  // a deliberate unlock, so a second apply() from another thread could dangle them.
  std::lock_guard l(controlMu_);
  try {
    const std::string reply = router_.handle(client, text);
    // Every command may have edited the router's model (add-source, remove-source, add-output, …),
    // so reconcile the graph with it here. Applying unconditionally needs no per-command list:
    // apply() moves demand by delta, so an unchanged model costs nothing and fires no transition.
    // Without this the graph never learns about an edit, and nothing would actually start or stop.
    model_ = router_.bridge();
    graph_.apply(model_);
    return reply;
  } catch (const std::exception&) {
    // The router itself never throws, but apply() can (thread creation, allocation). An exception
    // escaping here would reach an ixwebsocket callback thread and call std::terminate, so refuse.
    return R"({"error":"internal error","ok":false})";
  }
}

}  // namespace ob
