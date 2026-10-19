#include "engine/bridge_graph.h"

#include <vector>

namespace ob {

BridgeGraph::BridgeGraph(Deliver deliver) : deliver_(std::move(deliver)) {}

BridgeGraph::~BridgeGraph() { stop(); }

void BridgeGraph::apply(const Bridge& bridge) {
  // Two phases. Under mu_ we only restructure (hubs and rows). Demand is deferred to phase 2,
  // because SourceHub::addRef()/release() fire the running callback SYNCHRONOUSLY, and that
  // callback starts or stops an input adapter — which may publish back into this graph. Firing it
  // while mu_ is held would stall on, or self-deadlock with, a non-recursive mutex.
  std::map<std::string, int> wanted;                // sourceId -> refs the new model implies
  std::vector<std::unique_ptr<OutputRow>> retired;  // destroyed after mu_ is released
  {
    std::lock_guard l(mu_);

    for (const Source& s : bridge.sources) {
      if (hubs_.find(s.id) == hubs_.end()) hubs_.emplace(s.id, std::make_unique<SourceHub>(s.id));
    }
    for (auto it = hubs_.begin(); it != hubs_.end();) {
      if (bridge.findSource(it->first) == nullptr)
        it = hubs_.erase(it);
      else
        ++it;
    }

    // Remove rows whose output is gone. Every surviving hub starts from zero demand, so a
    // re-apply is exact and a hub left with no outputs is released to idle. Retired rows are moved
    // out here and destroyed AFTER mu_ is released: OutputRow's destructor joins its worker, which
    // may be inside deliver_, and holding mu_ across that join would stall publish() for every
    // other row (design 6.5: one slow sink must not block the others).
    for (auto it = rows_.begin(); it != rows_.end();) {
      if (bridge.findOutput(it->first) == nullptr) {
        retired.push_back(std::move(it->second.row));
        it = rows_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto& [id, hub] : hubs_) wanted[id] = 0;

    for (const Output& o : bridge.outputs) {
      auto found = rows_.find(o.id);
      if (found == rows_.end()) {
        Row r;
        r.row = std::make_unique<OutputRow>(o.id, o.sourceId, deliver_);
        found = rows_.emplace(o.id, std::move(r)).first;
      }
      found->second.row->setSourceId(o.sourceId);
      if (started_) found->second.row->start();
      if (hubUnlocked(o.sourceId) != nullptr) ++wanted[o.sourceId];
    }
  }
  retired.clear();  // joins each retired row's worker with mu_ released

  // Phase 2, with mu_ released: move each hub to exactly the demand the model implies, by DELTA.
  // Resetting to zero and re-adding would fire a stop-then-start transition on every apply(), so an
  // unrelated edit (renaming an output) would bounce every running source adapter. Only the
  // difference moves, so an unchanged model produces no transition at all.
  for (auto& [id, want] : wanted) {
    SourceHub* h = hub(id);  // the public accessor locks mu_ only for the lookup
    if (h == nullptr) continue;
    int cur = h->refs();
    for (; cur > want; --cur) h->release();
    for (; cur < want; ++cur) h->addRef();
  }
}

void BridgeGraph::start() {
  std::vector<OutputRow*> rows;
  {
    std::lock_guard l(mu_);
    started_ = true;
    for (auto& [id, r] : rows_) rows.push_back(r.row.get());
  }
  for (OutputRow* r : rows) r->start();  // outside mu_: start() spawns a worker
}

void BridgeGraph::stop() {
  std::vector<OutputRow*> rows;
  {
    std::lock_guard l(mu_);
    started_ = false;
    for (auto& [id, r] : rows_) rows.push_back(r.row.get());
  }
  // Join outside mu_: a worker that is inside deliver_ must not hold the lock publish() needs.
  for (OutputRow* r : rows) r->stop();
}

void BridgeGraph::publish(const std::string& sourceId, std::shared_ptr<const Frame> frame) {
  std::lock_guard l(mu_);
  SourceHub* h = hubUnlocked(sourceId);
  if (h == nullptr) return;
  h->publish(frame);
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
