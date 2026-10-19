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
  std::map<std::string, int> wanted;
  {
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
        found->second.row->configure(o, advertised);
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
  }

  for (auto& h : pruned)
    while (h->refs() > 0) h->release();  // stop the removed source's adapter before it dies
  pruned.clear();
  retired.clear();                                    // join retired rows with mu_ released
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

uint64_t BridgeGraph::offeredToRow(const std::string& outputId) const {
  std::lock_guard l(mu_);
  const auto it = rows_.find(outputId);
  return it == rows_.end() ? 0u : it->second.offered;
}

}  // namespace ob
