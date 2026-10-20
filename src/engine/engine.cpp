#include "engine/engine.h"

#include <exception>
#include <utility>
#include <vector>

#include <omthx/omthx.h>

#include "core/bridge_store.h"
#include "engine/ndi_sink.h"
#ifdef OB_WITH_NDI
#include <Processing.NDI.Lib.h>

#include "engine/ndi_runtime.h"
#endif

namespace ob {

Engine::Engine(int controlPort, std::string token, std::string bridgeFile, bool adapters)
    : controlPort_(controlPort),
      token_(std::move(token)),
      bridgeFile_(std::move(bridgeFile)),
      adaptersEnabled_(adapters),
      graph_(
          [this](const Output& o, const std::string& advertised) -> std::unique_ptr<Sink> {
            if (!adaptersEnabled_) return nullptr;  // unit tests: no device, deterministic graph
            if (o.target == OutputTarget::Ndi) {
#ifdef OB_WITH_NDI
              auto sink = std::make_unique<NdiSink>();
              sink->configure(o, advertised);
              return sink;
#else
              return nullptr;  // NDI support was not built
#endif
            }
            auto sink = std::make_unique<OmtSink>();
            sink->configure(o, advertised);
            return sink;
          },
          [this](SourceHub& hub, const Source& s) { onHubCreated(hub, s); }) {
  router_.setRuntime([this](const std::string& id) { return sourceRuntime(id); },
                     [this](const std::string& id) { return outputRuntime(id); });
  router_.setDiscovery([this] { return discover(); });
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
#ifdef OB_WITH_NDI
  if (adapters_.count(id) != 0 || ndiAdapters_.count(id) != 0) return;
#else
  if (adapters_.count(id) != 0) return;
#endif
  const Source* s = model_.findSource(id);
  if (s == nullptr) return;  // removed while stopping
  adapterStarts_.fetch_add(1);  // every start decision
  if (!adaptersEnabled_) {
    std::lock_guard l(runtimeMu_);
    sourceRuntime_[id].state = SignalState::Offline;
    return;
  }
  if (s->kind == SourceKind::Ndi) {
#ifdef OB_WITH_NDI
    auto src = std::make_unique<NdiSource>(
        s->address,
        [this, id](std::shared_ptr<Frame> f) { graph_.publish(id, std::move(f)); },
        [this, id](std::shared_ptr<AudioFrame> a) { graph_.publishAudio(id, std::move(a)); },
        [this, id](SignalState st, std::optional<ErrorInfo>) {
          // Adapter thread: own mutex only, never controlMu_ (stopAdapter joins this thread
          // while holding it).
          std::lock_guard l(runtimeMu_);
          sourceRuntime_[id].state = st;
        });
    ndiAdapters_[id] = std::move(src);
    ndiAdapters_[id]->start();
#else
    std::lock_guard l(runtimeMu_);
    sourceRuntime_[id].state = SignalState::Offline;  // NDI support was not built
#endif
    return;
  }
  auto src = std::make_unique<OmtSource>(
      s->address,
      [this, id](std::shared_ptr<Frame> f) { graph_.publish(id, std::move(f)); },
      [this, id](SignalState st, std::optional<ErrorInfo>) {
        std::lock_guard l(runtimeMu_);
        sourceRuntime_[id].state = st;
      },
      [this, id](std::shared_ptr<AudioFrame> a) { graph_.publishAudio(id, std::move(a)); });
  adapters_[id] = std::move(src);
  adapters_[id]->start();
}

void Engine::stopAdapter(const std::string& id) {
  adapters_.erase(id);      // joins the loops; their state callbacks have finished
#ifdef OB_WITH_NDI
  ndiAdapters_.erase(id);
#endif
  // Retire this run's liveness only after the join, so no late callback can overwrite it.
  std::lock_guard l(runtimeMu_);
  const auto it = sourceRuntime_.find(id);
  if (it != sourceRuntime_.end()) {
    it->second.state = SignalState::Ready;
    it->second.format = {};
  }
}

std::optional<SourceRuntime> Engine::sourceRuntime(const std::string& id) {
  pollTally();  // the stats poll also refreshes upstream tally (design 12.3)
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

Discovery Engine::discover() {
  Discovery found;
  found.omt = omthx::discover();
#ifdef OB_WITH_NDI
  if (!ensureNdi()) return found;
  NDIlib_find_instance_t find = NDIlib_find_create_v2(nullptr);
  if (find == nullptr) return found;
  NDIlib_find_wait_for_sources(find, 1000);  // the documented blocking bound
  uint32_t count = 0;
  const NDIlib_source_t* sources = NDIlib_find_get_current_sources(find, &count);
  for (uint32_t i = 0; sources != nullptr && i < count; ++i)
    if (sources[i].p_ndi_name != nullptr) found.ndi.emplace_back(sources[i].p_ndi_name);
  NDIlib_find_destroy(find);
#endif
  return found;
}

void Engine::pollTally() {
  const auto tallies = graph_.pollTallies();
  for (const auto& [sourceId, t] : tallies) {
    if (const auto omt = adapters_.find(sourceId); omt != adapters_.end())
      omt->second->setTally(t.program, t.preview);
#ifdef OB_WITH_NDI
    if (const auto ndi = ndiAdapters_.find(sourceId); ndi != ndiAdapters_.end())
      ndi->second->setTally(t.program, t.preview);
#endif
  }
}

std::optional<OutputRuntime> Engine::outputRuntime(const std::string& id) {
  const auto stats = graph_.rowStats(id);
  if (!stats) {
    std::lock_guard l(rateMu_);
    rates_.erase(id);
    return std::nullopt;
  }
  OutputRuntime rt;
  rt.state = stats->faulted ? SignalState::Offline
                            : (stats->live ? SignalState::Live : SignalState::Ready);
  rt.note = stats->note;
  rt.delivered = stats->delivered;
  rt.dropped = stats->dropped;
  rt.audioDelivered = stats->audioDelivered;
  rt.connections = stats->connections;

  const auto now = std::chrono::steady_clock::now();
  std::lock_guard l(rateMu_);
  const auto prev = rates_.find(id);
  if (prev != rates_.end()) {
    const double seconds = std::chrono::duration<double>(now - prev->second.at).count();
    if (seconds > 0.0) {
      // A reopened sender restarts its byte counter; report 0, never a negative rate.
      const uint64_t frames = stats->delivered >= prev->second.frames
                                  ? stats->delivered - prev->second.frames
                                  : 0;
      const uint64_t bytes =
          stats->bytesSent >= prev->second.bytes ? stats->bytesSent - prev->second.bytes : 0;
      rt.fps = static_cast<double>(frames) / seconds;
      rt.mbps = static_cast<double>(bytes) * 8.0 / seconds / 1e6;
    }
  }
  rates_[id] = RateSample{now, stats->delivered, stats->bytesSent};
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
  adapters_.clear();  // entries the stop callbacks did not already erase
#ifdef OB_WITH_NDI
  ndiAdapters_.clear();
#endif
  {
    std::lock_guard r(rateMu_);
    rates_.clear();  // a restart starts a new measurement, not a delta over the stopped gap
  }
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
