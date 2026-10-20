#pragma once
// Reconciles a Bridge model into running hubs and rows (design 6.4, 6.5). apply() may be
// called with a whole new model; it diffs against what it holds. No master tick: publish()
// is called by each SourceHub's adapter, and fan-out comes from the ref counts.
//
// THREADING: apply(), start() and stop() must never run concurrently. apply() is serialised by its
// owner — in production, Engine::controlMu_ around each command. start()/stop() are called on the
// engine's main thread, which must not overlap a live command: Engine::stop() stops the control
// server first, so its handlers have joined, before graph_.stop() runs. That serialisation is what
// lets these three hold raw row/hub pointers across a deliberate unlock. Only publish() runs on
// adapter threads; never call apply() from an adapter.
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include "core/bridge_model.h"
#include "engine/output_row.h"
#include "engine/source_hub.h"

namespace ob {

class BridgeGraph {
 public:
  // SinkFactory is namespace-scope in output_row.h; both are in namespace ob.
  // Called synchronously on the caller's thread during apply(), before that hub receives demand.
  // When apply() comes from a command, the caller holds Engine::controlMu_: the hook must not take
  // controlMu_ and must not block — it may start an adapter thread but must not wait on one.
  // Never call back into BridgeGraph.
  using HubReady = std::function<void(SourceHub& hub, const Source& source)>;

  explicit BridgeGraph(SinkFactory sinks, HubReady onHubCreated = {});
  BridgeGraph(const BridgeGraph&) = delete;
  BridgeGraph& operator=(const BridgeGraph&) = delete;
  // Calls stop(), which fires hub transitions. The owner must stop the graph before destroying
  // anything the hook captures; in Engine, adapter state must be declared before graph_ so it
  // outlives the callbacks.
  ~BridgeGraph();

  // Creates, updates and removes hubs and rows so they match `bridge`. Idempotent.
  void apply(const Bridge& bridge);

  void start();
  void stop();

  // Hands a frame to every row whose output reads this source.
  void publish(const std::string& sourceId, std::shared_ptr<Frame> frame);
  // The audio twin: stores the packet in the hub's audio slot and offers it to every row of the
  // source. Never touches the video offer counter — audio and video are counted separately.
  void publishAudio(const std::string& sourceId, std::shared_ptr<AudioFrame> frame);

  // Locks; for callers outside the graph.
  // READ-ONLY uses (latest(), running(), framesPublished()) are fine from any thread. Demand
  // mutation is NOT: addRef/release/setThumbnailWanted have a single writer — the thread that
  // calls apply() — so two transitions cannot interleave and the running callback's delivery
  // order always matches the last state change. Toggle thumbnail demand by changing the model and
  // calling apply(), not by reaching a hub from another thread.
  // Returns a mutable hub for tests; demand mutation stays with the graph.
  SourceHub* hub(const std::string& sourceId) const;
  size_t hubCount() const;
  size_t rowCount() const;
  uint64_t offeredToRow(const std::string& outputId) const;

  // A snapshot of one row's counters, for the state reply. nullopt when no such row exists.
  struct RowStats {
    uint64_t offered = 0, delivered = 0, dropped = 0, audioDelivered = 0;
    bool faulted = false;
    bool live = false;
    std::string note;
  };
  std::optional<RowStats> rowStats(const std::string& outputId) const;

 private:
  // Caller holds mu_. apply() and publish() use these, never the locked public ones.
  SourceHub* hubUnlocked(const std::string& sourceId) const;

  // Moves every hub to `wanted`, by delta, with mu_ released. No-op while stopped.
  void applyDemand(const std::map<std::string, int>& wanted);

  struct Row {
    std::unique_ptr<OutputRow> row;
    uint64_t offered = 0;
  };

  mutable std::mutex mu_;
  // The factory runs under mu_ (as does Sink::configure).
  SinkFactory sinks_;
  HubReady onHubCreated_;
  std::map<std::string, std::unique_ptr<SourceHub>> hubs_;
  std::map<std::string, Row> rows_;  // by output id
  std::map<std::string, int> demand_;  // per source id, the demand the model implies
  std::set<std::string> hooked_;  // source ids whose onHubCreated_ hook succeeded
  bool started_ = false;
};

}  // namespace ob
