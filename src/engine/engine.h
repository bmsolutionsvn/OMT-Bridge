#pragma once
// The headless engine (design 6.1, 6.6): owns the graph, the bridge file and the control
// server. Closing the UI drops a control connection; it must never touch the graph.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "core/bridge_model.h"
#include "engine/bridge_graph.h"
#include "engine/command_router.h"
#include "engine/control_server.h"
#include "engine/ndi_source.h"
#include "engine/omt_sink.h"
#include "engine/omt_source.h"

namespace ob {

#ifdef OB_WITH_NDI
struct NdiFinderHolder;  // hides the NDI SDK from this header
#endif

class Engine {
 public:
  // adapters is false only in unit tests: it keeps the graph deterministic and opens no device.
  Engine(int controlPort, std::string token, std::string bridgeFile = {}, bool adapters = true);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Builds the graph from the model. Safe to call repeatedly, but it REPLACES the router's model
  // (discarding anything commands minted) and re-seeds the id counters — so it is a startup call:
  // run it on the same thread as start()/stop(), never from a command, adaptor or deliver thread.
  // Holds controlMu_ for its whole body, so it cannot interleave a command's apply.
  void apply(const Bridge& bridge);

  // A source's runtime view, or nullopt when no such source is configured. Reads the runtime map
  // and the hub, never the control mutex — safe from a reply thread.
  // Non-const: a state poll also refreshes tally, which pushes it to the adapters.
  std::optional<SourceRuntime> sourceRuntime(const std::string& id);
  // Non-const: a poll records a rate sample, so the next poll measures over a real interval.
  std::optional<OutputRuntime> outputRuntime(const std::string& id);

  // Starts the graph and, when controlPort > 0, the control server. On an AUTHORISED open the
  // server pushes a full `state` reply before any command, so a UI should treat its first message
  // as its initial snapshot rather than the answer to something it sent.
  void start();
  void stop();

  // The UI's "Stop all outputs" / "Start" switch: stops or starts the graph while keeping the
  // control server and the model, so rows can still be edited and the bridge file saved.
  void setRunning(bool on);

  BridgeGraph& graph() { return graph_; }

  // Called when a UI disconnects. Deliberately does not stop the graph (design 6.1, 1.1).
  void onControlClientDisconnected(const ClientId& client);

  uint64_t controlConnections() const { return controlConnections_; }
  // Every start decision the demand path made, including the NDI branch (which has no adapter).
  // A test-visible diagnostic: it pins that an edited source is stopped and restarted.
  uint64_t adapterStarts() const { return adapterStarts_.load(); }

 private:
  std::string handleCommand(const ClientId& client, const std::string& text);

  // The graph's running hook, called synchronously during apply() — i.e. with controlMu_ held when
  // apply() came from a command. It only installs the adapter transition callback.
  void onHubCreated(SourceHub& hub, const Source& source);
  // UI client count drives thumbnail demand (design 11). Both take controlMu_.
  void onControlClientConnected();
  // One preview per source per settings interval, pushed as binary frames. controlMu_ held.
  void maybeSendThumbnails();
  // Reconciles router, adapters, runtime map and graph. controlMu_ held.
  void applyLocked(const Bridge& bridge);
  void startAdapter(const std::string& sourceId);  // controlMu_ held
  void pollTally();                                // controlMu_ held
  void stopAdapter(const std::string& sourceId);   // controlMu_ held
  bool setRunningLocked(bool on);                  // controlMu_ held
  // OMT names plus one NDI find; the NDI branch waits at most one second, so this blocks a
  // control command for that long. controlMu_ held.
  Discovery discover();

  int controlPort_;
  std::string token_;
  std::string bridgeFile_;
  // Serialises command handling. ixwebsocket runs each connection on its own thread, so two
  // clients (or a supervisor restart overlapping a live UI) would otherwise race the router's
  // model/counter — and would call BridgeGraph::apply() from two threads, breaking its no-concurrent-apply rule (it holds raw row/hub pointers across a deliberate unlock).
  std::mutex controlMu_;
  CommandRouter router_;
  // The model the graph was last reconciled against. Stored before graph_.apply() because the
  // running hook (startAdapter) reads it while apply() runs.
  Bridge model_;                                               // controlMu_
  std::map<std::string, std::unique_ptr<OmtSource>> adapters_;      // controlMu_
#ifdef OB_WITH_NDI
  std::map<std::string, std::unique_ptr<NdiSource>> ndiAdapters_;   // controlMu_
  // Kept alive so remote mDNS announcements accumulate between Discover clicks; a fresh
  // one-second find misses a source on another machine (it needs ~3 s to appear).
  std::unique_ptr<NdiFinderHolder> ndiFinder_;                      // controlMu_
#endif
  bool adaptersEnabled_ = true;
  // The UI's Start / Stop all outputs switch. It gates the graph, not the control server, so the
  // UI can still edit and restart rows while outputs are stopped.
  bool running_ = false;                                        // controlMu_
  // A state callback runs on an adapter thread, and stopAdapter() joins that adapter while
  // holding controlMu_, so this map must never be guarded by controlMu_.
  mutable std::mutex runtimeMu_;                                // guards sourceRuntime_
  std::map<std::string, SourceRuntime> sourceRuntime_;          // runtimeMu_
  // The last counter sample per output, for fps/Mbps. Guards itself: outputRuntime() is called
  // from the control thread, but the public method may also be called by a test without
  // controlMu_.
  struct RateSample {
    std::chrono::steady_clock::time_point at;
    uint64_t frames = 0;
    uint64_t bytes = 0;
  };
  std::mutex rateMu_;
  std::map<std::string, RateSample> rates_;                     // rateMu_
  BridgeGraph graph_;
  std::unique_ptr<ControlServer> server_;
  std::atomic<uint64_t> controlConnections_{0};
  std::atomic<uint64_t> adapterStarts_{0};
  int uiClients_ = 0;  // controlMu_: authorised control clients, i.e. thumbnail demand
  std::map<std::string, std::chrono::steady_clock::time_point> lastThumbnail_;  // controlMu_
};

}  // namespace ob
