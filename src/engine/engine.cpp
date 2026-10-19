#include "engine/engine.h"

#include <exception>
#include <utility>
#include <vector>

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
  std::lock_guard l(controlMu_);
  applyLocked(bridge);
}

void Engine::applyLocked(const Bridge& bridge) {
  // An edited source must regain its adapter: demand never drops for an id that stays demanded, so
  // apply() alone would keep the old receiver (old address/kind) running forever.
  std::vector<std::string> changed;
  for (const Source& s : bridge.sources) {
    const Source* old = model_.findSource(s.id);
    if (old == nullptr || !(*old == s)) changed.push_back(s.id);
  }
  for (const auto& id : changed) stopAdapter(id);  // an edited source must restart

  router_.setBridge(bridge);
  model_ = bridge;

  {
    std::lock_guard l(runtimeMu_);
    // Every configured source starts Ready: it is known, but no adapter has measured anything.
    for (const Source& s : bridge.sources)
      if (sourceRuntime_.find(s.id) == sourceRuntime_.end())
        sourceRuntime_[s.id].state = SignalState::Ready;
    for (auto it = sourceRuntime_.begin(); it != sourceRuntime_.end();) {
      if (model_.findSource(it->first) == nullptr)
        it = sourceRuntime_.erase(it);
      else
        ++it;
    }
  }

  const auto restartChanged = [&] {
    for (const auto& id : changed) {
      try {
        SourceHub* h = graph_.hub(id);
        if (h != nullptr && h->running()) startAdapter(id);  // restart under the same demand
      } catch (...) {
        // Keep the original failure; a source that cannot restart is reported by its own state.
      }
    }
  };

  try {
    graph_.apply(model_);
  } catch (...) {
    restartChanged();  // the hub may still be running with the old demand
    throw;
  }
  restartChanged();
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
  adapterStarts_.fetch_add(1);  // every start decision, including the NDI branch
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

void Engine::stopAdapter(const std::string& id) {
  adapters_.erase(id);  // joins the loops; their state callbacks have finished
  // Retire this run's liveness only after the join, so no late callback can overwrite it.
  std::lock_guard l(runtimeMu_);
  const auto it = sourceRuntime_.find(id);
  if (it != sourceRuntime_.end()) {
    it->second.state = SignalState::Ready;
    it->second.format = {};
  }
}

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
  if (hub != nullptr && hub->running()) {
    if (const auto latest = hub->latest()) {
      rt.format = VideoFormatView{latest->format.width, latest->format.height,
                                  latest->format.fpsN, latest->format.fpsD,
                                  latest->format.alpha, true};
    }
  }
  return rt;
}

std::optional<OutputRuntime> Engine::outputRuntime(const std::string& id) const {
  const auto stats = graph_.rowStats(id);
  if (!stats) return std::nullopt;
  OutputRuntime rt;
  rt.state = stats->faulted ? SignalState::Offline
                            : (stats->live ? SignalState::Live : SignalState::Ready);
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
    // applyLocked() costs nothing when nothing changed and fires no transition.
    // Without this the graph never learns about an edit, and nothing would actually start or stop.
    applyLocked(router_.bridge());
    return reply;
  } catch (const std::exception&) {
    // The router itself never throws, but apply() can (thread creation, allocation). An exception
    // escaping here would reach an ixwebsocket callback thread and call std::terminate, so refuse.
    return R"({"error":"internal error","ok":false})";
  }
}

}  // namespace ob
