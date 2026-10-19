#pragma once
// Reconciles a Bridge model into running hubs and rows (design 6.4, 6.5). apply() may be
// called with a whole new model; it diffs against what it holds. No master tick: publish()
// is called by each SourceHub's adapter, and fan-out comes from the ref counts.
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/bridge_model.h"
#include "engine/output_row.h"
#include "engine/source_hub.h"

namespace ob {

class BridgeGraph {
 public:
  using Deliver = OutputRow::Deliver;

  explicit BridgeGraph(Deliver deliver);
  BridgeGraph(const BridgeGraph&) = delete;
  BridgeGraph& operator=(const BridgeGraph&) = delete;
  ~BridgeGraph();

  // Creates, updates and removes hubs and rows so they match `bridge`. Idempotent.
  void apply(const Bridge& bridge);

  void start();
  void stop();

  // Hands a frame to every row whose output reads this source.
  void publish(const std::string& sourceId, std::shared_ptr<const Frame> frame);

  // Locks; for callers outside the graph.
  // READ-ONLY uses (latest(), running(), framesPublished()) are fine from any thread. Demand
  // mutation is NOT: addRef/release/setThumbnailWanted have a single writer — the thread that
  // calls apply() — so two transitions cannot interleave and the running callback's delivery
  // order always matches the last state change. Toggle thumbnail demand by changing the model and
  // calling apply(), not by reaching a hub from another thread.
  SourceHub* hub(const std::string& sourceId) const;
  size_t hubCount() const;
  size_t rowCount() const;
  uint64_t offeredToRow(const std::string& outputId) const;

 private:
  // Caller holds mu_. apply() and publish() use these, never the locked public ones.
  SourceHub* hubUnlocked(const std::string& sourceId) const;

  struct Row {
    std::unique_ptr<OutputRow> row;
    uint64_t offered = 0;
  };

  mutable std::mutex mu_;
  Deliver deliver_;
  std::map<std::string, std::unique_ptr<SourceHub>> hubs_;
  std::map<std::string, Row> rows_;  // by output id
  bool started_ = false;
};

}  // namespace ob
