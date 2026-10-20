#include "engine/bridge_graph.h"

#include <vector>

namespace ob {

BridgeGraph::BridgeGraph(SinkFactory sinks, HubReady onHubCreated)
    : sinks_(std::move(sinks)), onHubCreated_(std::move(onHubCreated)) {}

BridgeGraph::~BridgeGraph() { stop(); }

void BridgeGraph::apply(const Bridge& bridge) {
  std::vector<std::unique_ptr<SourceHub>> pruned;
  std::vector<std::pair<SourceHub*, Source>> toHook;
  std::vector<OutputRow*> toStart;
  std::vector<std::unique_ptr<OutputRow>> retired;
  std::vector<OutputRow*> toRestart;
  std::map<std::string, int> wanted;
  try {
    std::lock_guard l(mu_);

    for (const Source& s : bridge.sources) {
      if (hubs_.find(s.id) == hubs_.end()) hubs_.emplace(s.id, std::make_unique<SourceHub>(s.id));
    }
    for (auto it = hubs_.begin(); it != hubs_.end();) {
      if (bridge.findSource(it->first) == nullptr) {
        pruned.push_back(std::move(it->second));
        hooked_.erase(it->first);
        it = hubs_.erase(it);
      } else {
        ++it;
      }
    }

    for (auto it = rows_.begin(); it != rows_.end();) {
      if (bridge.findOutput(it->first) == nullptr) {
        retired.push_back(std::move(it->second.row));
        it = rows_.erase(it);
      } else {
        ++it;
      }
    }

    for (const Output& o : bridge.outputs) {
      const std::string advertised = bridge.advertisedName(o);
      auto found = rows_.find(o.id);
      if (found == rows_.end()) {
        auto sink = sinks_ ? sinks_(o, advertised) : nullptr;
        Row r;
        r.row = std::make_unique<OutputRow>(o.id, std::move(sink));
        found = rows_.emplace(o.id, std::move(r)).first;
        found->second.row->configure(o, advertised);
      } else {
        const bool sourceChanged = found->second.row->sourceId() != o.sourceId;
        const bool configChanged = found->second.row->config() != o;
        found->second.row->configure(o, advertised);
        // Restart below, outside mu_: a retargeted row must not keep the old source's liveness, and
        // a faulted row gets a fresh worker when the operator changes its configuration.
        if (started_ && (sourceChanged || (configChanged && found->second.row->faulted())))
          toRestart.push_back(found->second.row.get());
      }
    }

    for (const Source& s : bridge.sources)
      if (hooked_.find(s.id) == hooked_.end())
        if (SourceHub* h = hubUnlocked(s.id)) toHook.emplace_back(h, s);

    for (auto& [id, hub] : hubs_) wanted[id] = 0;
    for (const Output& o : bridge.outputs)
      if (hubUnlocked(o.sourceId) != nullptr) ++wanted[o.sourceId];

    if (started_)
      for (auto& [id, r] : rows_) toStart.push_back(r.row.get());
  } catch (...) {
    // A throwing factory, configure or name must not leak a pruned hub's adapter: stop it while
    // the hub is still alive, then let the caller report the failure.
    for (auto& h : pruned)
      while (h->refs() > 0) h->release();
    throw;
  }

  for (auto& h : pruned)
    while (h->refs() > 0) h->release();  // stop the removed source's adapter before it dies
  pruned.clear();
  retired.clear();                                    // join retired rows with mu_ released
  for (OutputRow* r : toRestart) {                    // a source edit: clear the stale queue and liveness
    r->stop();
    r->start();
  }
  for (auto& [hub, source] : toHook) {
    if (onHubCreated_) onHubCreated_(*hub, source);
    std::lock_guard l(mu_);
    hooked_.insert(source.id);  // only after success, so a throwing hook is retried
  }
  for (OutputRow* r : toStart) r->start();            // spawn workers outside mu_
  applyDemand(wanted);
  std::lock_guard l(mu_);
  demand_ = std::move(wanted);                        // remember for start()/stop()
}

void BridgeGraph::applyDemand(const std::map<std::string, int>& wanted) {
  {
    std::lock_guard l(mu_);
    if (!started_) return;  // configured but not started: no adapter may run yet
  }
  for (const auto& [id, want] : wanted) {
    SourceHub* h = nullptr;
    {
      std::lock_guard l(mu_);
      h = hubUnlocked(id);
    }
    if (h == nullptr) continue;
    int cur = h->refs();
    for (; cur > want; --cur) h->release();
    for (; cur < want; ++cur) h->addRef();
  }
}

void BridgeGraph::start() {
  std::vector<OutputRow*> rows;
  std::map<std::string, int> wanted;
  {
    std::lock_guard l(mu_);
    started_ = true;
    wanted = demand_;
    for (auto& [id, r] : rows_) rows.push_back(r.row.get());
  }
  for (OutputRow* r : rows) r->start();
  applyDemand(wanted);
}

void BridgeGraph::stop() {
  std::vector<OutputRow*> rows;
  std::vector<SourceHub*> hubs;
  {
    std::lock_guard l(mu_);
    started_ = false;
    for (auto& [id, r] : rows_) rows.push_back(r.row.get());
    for (auto& [id, h] : hubs_) hubs.push_back(h.get());
  }
  for (OutputRow* r : rows) r->stop();
  for (SourceHub* h : hubs)
    while (h->refs() > 0) h->release();  // closes every ref-driven adapter
}

void BridgeGraph::publish(const std::string& sourceId, std::shared_ptr<Frame> frame) {
  std::lock_guard l(mu_);
  SourceHub* h = hubUnlocked(sourceId);
  if (h == nullptr) return;
  h->storeFrame(frame);
  for (auto& [id, r] : rows_) {
    if (r.row->sourceId() == sourceId) {
      r.row->offer(frame);
      ++r.offered;
    }
  }
}

void BridgeGraph::publishAudio(const std::string& sourceId, std::shared_ptr<AudioFrame> frame) {
  std::lock_guard l(mu_);
  SourceHub* h = hubUnlocked(sourceId);
  if (h == nullptr) return;
  h->storeAudio(frame);
  for (auto& [id, r] : rows_) {
    if (r.row->sourceId() == sourceId) r.row->offerAudio(frame);
  }
}

SourceHub* BridgeGraph::hubUnlocked(const std::string& sourceId) const {
  const auto it = hubs_.find(sourceId);
  return it == hubs_.end() ? nullptr : it->second.get();
}

SourceHub* BridgeGraph::hub(const std::string& sourceId) const {
  std::lock_guard l(mu_);
  return hubUnlocked(sourceId);
}

size_t BridgeGraph::hubCount() const {
  std::lock_guard l(mu_);
  return hubs_.size();
}

size_t BridgeGraph::rowCount() const {
  std::lock_guard l(mu_);
  return rows_.size();
}

std::map<std::string, Tally> BridgeGraph::pollTallies() {
  std::lock_guard l(mu_);
  std::map<std::string, Tally> out;
  for (auto& [id, r] : rows_) {
    const std::string& source = r.row->sourceId();
    if (source.empty()) continue;
    const Tally t = r.row->tally();
    Tally& acc = out[source];
    acc.program = acc.program || t.program;
    acc.preview = acc.preview || t.preview;
  }
  for (auto& [id, hub] : hubs_)
    hub->setDownstreamTally(out.count(id) != 0 ? out[id] : Tally{});
  return out;
}

uint64_t BridgeGraph::offeredToRow(const std::string& outputId) const {
  std::lock_guard l(mu_);
  const auto it = rows_.find(outputId);
  return it == rows_.end() ? 0u : it->second.offered;
}

std::optional<BridgeGraph::RowStats> BridgeGraph::rowStats(const std::string& outputId) const {
  // note() takes the sink mutex, which is the documented graph -> sink order (output_row.h).
  std::lock_guard l(mu_);
  const auto it = rows_.find(outputId);
  if (it == rows_.end()) return std::nullopt;
  RowStats s;
  s.offered = it->second.offered;
  s.delivered = it->second.row->delivered();
  s.dropped = it->second.row->dropped();
  s.audioDelivered = it->second.row->audioDelivered();
  s.faulted = it->second.row->faulted();
  s.live = it->second.row->live();
  s.note = it->second.row->note();
  return s;
}

}  // namespace ob
