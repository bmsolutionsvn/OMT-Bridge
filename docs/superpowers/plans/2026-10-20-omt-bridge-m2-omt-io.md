# OMT-Bridge M2 — OMT / OMT-HX In and Out

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the bridge move pictures: real `OmtSource` and `OmtSink` adapters on OMT-HX's existing UYVY/UYVA API, opaque-alpha detection, the H.264 alpha rule, audio passthrough, and the loopback proof `omthx-send → bridge → omthx-recv --dump`.

**Architecture:** The M1 engine is deliberately inert: `BridgeGraph` owns hubs and rows, demand is reference-counted, but nothing calls an adapter and the row sink is a no-op `std::function`. M2 replaces those two seams — a hub's `onRunningChanged` starts and stops a real `omthx::Receiver`, and each output row owns a real `omthx::Sender`. Three M1 deferrals must be closed first because they are precisely what M2 needs: a hub-created hook, demand that follows `start()`/`stop()`, and a per-output sink factory that receives the output's configuration.

**Tech Stack:** C++20, MSVC, CMake + vcpkg, OMT-HX `omthx` at `ae96aa1` (`Receiver`/`Sender`/`discover`/`probe`, UYVY + UYVA), doctest. Spec: `docs/superpowers/specs/2026-10-19-omt-bridge-design.md` §6.3–§10, §16, §20.

---

## Preconditions

- Branch `feat/m2-omt-io` off `main` at `727a965`.
- `main` contains M0+M1: 57 tests green.
- OMT-HX is present at `third_party/OMT-HX` (pinned `ae96aa1`) with its `omt/*` submodules initialised.
- All builds/tests go through `scripts/test.ps1` / `scripts/build.ps1` (they activate the MSVC shell).

## Rules that have bitten this repo before

> **Trailing newline**: every file ends with a single `0x0A`, no `0x0D`. PowerShell 5.1's `Set-Content` writes CRLF; re-normalise with `[IO.File]::WriteAllText($p, ([IO.File]::ReadAllText($p) -replace "`r`n","`n"))` and verify.
>
> **doctest stringifies every `CHECK` operand.** Never assert on an enum, a struct, or a smart pointer: use `bool`/`int`/`std::string`, `static_cast<bool>(ptr)`, `ptr.get() != nullptr`, or read through the pointee. Three separate compile failures in M1 came from this.
>
> **Locks**: order is `Engine::controlMu_` → `BridgeGraph::mu_` → `SourceHub::mu_` → row queue. `BridgeGraph::apply()`, `start()` and `stop()` are owned by ONE thread; only `publish()` may be called from an adapter thread. Never call a graph method from inside a hub's `onRunningChanged`.
>
> **Frame ownership**: the canonical frame is `shared_ptr<const Frame>`; a decoded frame is shared by every output of one source and never copied per row.

## File structure

| Path | Responsibility |
|---|---|
| `src/core/alpha_gate.h`, `.cpp` | Opaque-alpha decision with 2 s hysteresis. Pure, clock-injected, unit-tested. |
| `src/engine/output_row.h`, `.cpp` | **Modified**: row owns a `Sink` (a real output adapter) and its `Output` config. |
| `src/engine/bridge_graph.h`, `.cpp` | **Modified**: sink factory, hub-created hook, `started_`-gated demand, per-hub desired demand. |
| `src/engine/source_hub.h`, `.cpp` | **Modified**: `storeFrame` replaces `publish`; owns an `AlphaGate`. |
| `src/engine/omt_source.h`, `.cpp` | **New**: `omthx::Receiver` → `Frame`, plus video+audio threads and state reporting. |
| `src/engine/omt_sink.h`, `.cpp` | **New**: `Frame` → `omthx::Sender::sendVideoUyvy`, mode/quality/codec, H.264 alpha rule, audio. |
| `src/engine/engine.h`, `.cpp` | **Modified**: builds the sink factory and the hub hook, owns the source adapters, reports runtime state. |
| `src/engine/command_router.h`, `.cpp` | **Modified**: `state` takes a runtime snapshot supplied by the engine. |
| `tests/alpha_gate_test.cpp`, `tests/omt_sink_test.cpp`, `tests/omt_loopback_test.cpp` | **New**. |
| `scripts/m2-e2e.ps1` | **New**: the `omthx-send → bridge → omthx-recv --dump` proof. |

## Interfaces this plan locks in

```cpp
// core/alpha_gate.h — pure decision, so the hysteresis is testable without sleeping.
class AlphaGate {
 public:
  // Returns whether alpha must be dropped this frame. `hasAlpha` is the frame's own flag.
  bool shouldDrop(bool hasAlpha, const uint8_t* alpha, size_t alphaBytes,
                  std::chrono::steady_clock::time_point now);
  void reset();
 private:
  std::chrono::steady_clock::time_point opaqueSince_{};
  bool sawOpaque_ = false;
};

// engine/output_row.h — the row's sink is now an interface it owns.
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void deliver(const std::shared_ptr<const Frame>& frame) = 0;
  virtual void configure(const Output& output, const std::string& advertisedName) {}
  virtual std::string note() const { return {}; }
};
using SinkFactory = std::function<std::unique_ptr<Sink>(const Output&, const std::string&)>;

// engine/bridge_graph.h — the engine installs the adapter hook at hub creation.
using HubReady = std::function<void(SourceHub& hub, const Source& source)>;
BridgeGraph(SinkFactory sinks, HubReady onHubCreated = {});
```

---

### Task 1: `AlphaGate` — opaque alpha with 2 s hysteresis

Design §9.2. A sender that marks frames as alpha but sends all-255 must not double the HX coded picture; switching must not flap because each change reopens the HX encoder.

**Files:**
- Create: `src/core/alpha_gate.h`, `src/core/alpha_gate.cpp`, `tests/alpha_gate_test.cpp`
- Modify: `CMakeLists.txt` (`ob_core` source + test)

- [ ] **Step 1: Write the failing test**

```cpp
// tests/alpha_gate_test.cpp
#include <doctest/doctest.h>

#include <chrono>
#include <vector>

#include "core/alpha_gate.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST_CASE("no alpha means nothing to drop") {
  ob::AlphaGate g;
  const auto t0 = Clock::now();
  CHECK_FALSE(g.shouldDrop(false, nullptr, 0, t0));
}

TEST_CASE("one sub-255 pixel turns alpha on immediately and keeps it on") {
  ob::AlphaGate g;
  const auto t0 = Clock::now();
  std::vector<uint8_t> a(16, 255);
  a[7] = 254;
  CHECK_FALSE(g.shouldDrop(true, a.data(), a.size(), t0));       // not opaque: keep it
  CHECK_FALSE(g.shouldDrop(true, a.data(), a.size(), t0 + 3s));  // still not opaque
}

TEST_CASE("alpha is dropped only after 2 s of continuous opacity") {
  ob::AlphaGate g;
  const auto t0 = Clock::now();
  std::vector<uint8_t> opaque(16, 255);
  CHECK_FALSE(g.shouldDrop(true, opaque.data(), opaque.size(), t0));       // starts the clock
  CHECK_FALSE(g.shouldDrop(true, opaque.data(), opaque.size(), t0 + 1s));  // under 2 s
  CHECK(g.shouldDrop(true, opaque.data(), opaque.size(), t0 + 2s));        // 2 s reached
}

TEST_CASE("a single transparent frame resets the hysteresis") {
  ob::AlphaGate g;
  const auto t0 = Clock::now();
  std::vector<uint8_t> opaque(16, 255);
  std::vector<uint8_t> keyed(16, 128);
  CHECK_FALSE(g.shouldDrop(true, opaque.data(), opaque.size(), t0));
  CHECK_FALSE(g.shouldDrop(true, keyed.data(), keyed.size(), t0 + 1s));   // resets
  CHECK_FALSE(g.shouldDrop(true, opaque.data(), opaque.size(), t0 + 2s)); // clock restarted
  CHECK(g.shouldDrop(true, opaque.data(), opaque.size(), t0 + 4s));
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `.\scripts\test.ps1`
Expected: compile error — `core/alpha_gate.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
// src/core/alpha_gate.h
#pragma once
// Opaque-alpha detection (design 9.2). A source that marks frames as alpha while every pixel is
// opaque would double an OMT-HX coded picture for nothing, so alpha is dropped — but only after
// two seconds of continuous opacity. The hysteresis is required: changing alpha changes the HX
// coded height, so omthx reopens the encoder and forces a keyframe. Alpha must not flap.
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace ob {

class AlphaGate {
 public:
  // True when alpha should be dropped for this frame. Scans until the first value below 255.
  bool shouldDrop(bool hasAlpha, const uint8_t* alpha, size_t alphaBytes,
                  std::chrono::steady_clock::time_point now);
  void reset();

 private:
  std::chrono::steady_clock::time_point opaqueSince_{};
  bool sawOpaque_ = false;
};

}  // namespace ob
```

- [ ] **Step 4: Write the implementation**

```cpp
// src/core/alpha_gate.cpp
#include "core/alpha_gate.h"

namespace ob {
namespace {
constexpr auto kOpaqueWindow = std::chrono::seconds(2);
}

bool AlphaGate::shouldDrop(bool hasAlpha, const uint8_t* alpha, size_t alphaBytes,
                           std::chrono::steady_clock::time_point now) {
  if (!hasAlpha || alpha == nullptr || alphaBytes == 0) {
    reset();
    return false;
  }
  for (size_t i = 0; i < alphaBytes; ++i) {
    if (alpha[i] != 255) {
      reset();  // any transparency keeps alpha on and restarts the clock
      return false;
    }
  }
  if (!sawOpaque_) {
    sawOpaque_ = true;
    opaqueSince_ = now;
    return false;
  }
  return now - opaqueSince_ >= kOpaqueWindow;
}

void AlphaGate::reset() {
  sawOpaque_ = false;
  opaqueSince_ = {};
}

}  // namespace ob
```

- [ ] **Step 5: Wire it in and run**

In `CMakeLists.txt`: add `src/core/alpha_gate.cpp` to `ob_core`, `tests/alpha_gate_test.cpp` to `ob_tests`.

Run: `.\scripts\test.ps1`
Expected: `[doctest] Status: SUCCESS!` with **61 cases** (57 + 4).

- [ ] **Step 6: Commit**

```bash
git add src/core/alpha_gate.h src/core/alpha_gate.cpp tests/alpha_gate_test.cpp CMakeLists.txt
git commit -m "feat(core): opaque-alpha detection with a two-second hysteresis"
```

---

### Task 2: `SourceHub` — rename the slot write and apply the alpha gate

The M1 review found two functions named `publish`: `SourceHub::publish` (fills the slot only) and `BridgeGraph::publish` (fans out). An adapter calling the first would see `framesPublished()` climb while no output ever receives a frame. Rename it, and make the hub apply the alpha decision once per source frame (so every output shares one scan).

**Files:**
- Modify: `src/engine/source_hub.h`, `src/engine/source_hub.cpp`, `tests/source_hub_test.cpp`
- Modify: `src/engine/bridge_graph.h`, `src/engine/bridge_graph.cpp` — **signature only**:
  `publish` must take `shared_ptr<Frame>`, because it hands the frame to the now non-const
  `storeFrame`. Without this the tree does not compile.

- [ ] **Step 1: Rename in the header and document the trap**

Replace the `publish` declaration with:

```cpp
  // Stores the newest frame in the single slot and applies opaque-alpha detection. Non-const in,
  // const stored: the hub may clear `format.alpha` before the frame becomes visible to consumers.
  // This does NOT fan out — carrying media to rows is BridgeGraph::publish(sourceId, frame). An
  // adapter must call the graph, never this, or outputs receive nothing while framesPublished()
  // climbs.
  void storeFrame(std::shared_ptr<Frame> frame);
```

Add the gate as a member:

```cpp
  AlphaGate alpha_;
```

and `#include "core/alpha_gate.h"`.

- [ ] **Step 2: Implement `storeFrame`**

```cpp
void SourceHub::storeFrame(std::shared_ptr<Frame> frame) {
  if (frame && frame->format.alpha) {
    // One scan per source frame, shared by every output (design 6.4).
    const size_t offset = frame->alphaOffset();
    const size_t bytes = frame->alphaBytes();
    const uint8_t* plane = offset == Frame::kNoAlpha ? nullptr : frame->uyvy.data() + offset;
    if (alpha_.shouldDrop(true, plane, bytes, std::chrono::steady_clock::now())) {
      // Drop the plane without copying: shrinking a vector does not move the picture, and
      // Frame::valid() requires uyvy.size() == lumaBytes() + alphaBytes() for the new format.
      frame->format.alpha = false;
      frame->premultiplied = false;  // nothing to premultiply once alpha is gone
      frame->uyvy.resize(frame->lumaBytes());
    }
  } else {
    alpha_.reset();
  }

  // The slot is immutable once stored: consumers only ever see shared_ptr<const Frame>.
  std::shared_ptr<const Frame> published = std::move(frame);
  std::shared_ptr<const Frame> previous;
  {
    std::lock_guard l(mu_);
    previous = std::move(latest_);
    latest_ = std::move(published);
    ++published_;
  }
  // `previous` is released here, outside the lock: dropping the last reference frees a full frame.
}
```

The parameter is **non-const** precisely so the gate can clear `format.alpha`; the slot holds
`shared_ptr<const Frame>`, so no consumer can mutate a published frame. Nothing else may write
`latest_`.

- [ ] **Step 2b: Widen `BridgeGraph::publish` to a mutable frame**

`BridgeGraph::publish` takes `std::shared_ptr<const Frame>` today and forwards it to the hub, so it
will not bind to the new `storeFrame`. Change the declaration in `src/engine/bridge_graph.h` and
the definition in `src/engine/bridge_graph.cpp` to `std::shared_ptr<Frame>`. Leave the body alone
in this task — Task 4 revisits fan-out and the hub store. Only tests call `publish`, and
`tests/bridge_graph_test.cpp` already passes a `shared_ptr<Frame>`.

- [ ] **Step 3: Update the test and run**

In `tests/source_hub_test.cpp`:
- `makeFrame` must return `std::shared_ptr<ob::Frame>`. It currently returns
  `std::shared_ptr<const ob::Frame>`, which no longer binds to `storeFrame`.
- Replace **both** `hub.publish(...)` calls with `hub.storeFrame(...)`.

Run: `.\scripts\test.ps1`
Expected: `[doctest] Status: SUCCESS!`, 61 cases (same count as Task 1).

- [ ] **Step 4: Commit**

```bash
git add src/engine/source_hub.h src/engine/source_hub.cpp src/engine/bridge_graph.h src/engine/bridge_graph.cpp tests/source_hub_test.cpp
git commit -m "refactor(engine): SourceHub::storeFrame, with shared opaque-alpha detection"
```

---

### Task 3: Rows own a real sink

`OutputRow` currently delivers through a shared `std::function` with no output identity, so no adapter can be created per output (§7.2: each output owns its own `Sender`). Replace it with a `Sink` the row owns.

**Files:**
- Modify: `src/engine/output_row.h`, `src/engine/output_row.cpp`, `tests/output_row_test.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/output_row_test.cpp`:

```cpp
namespace {
class RecordingSink : public ob::Sink {
 public:
  explicit RecordingSink(std::vector<int64_t>* seen) : seen_(seen) {}
  void deliver(const std::shared_ptr<const ob::Frame>& f) override { seen_->push_back(f->timestamp); }
  void configure(const ob::Output& o, const std::string& advertised) override {
    configuredId = o.id;
    configuredName = advertised;
  }
  std::string note() const override { return "note"; }
  std::string configuredId, configuredName;
 private:
  std::vector<int64_t>* seen_;
};
}

TEST_CASE("a row owns its sink and hands it the output configuration") {
  std::vector<int64_t> seen;
  auto sink = std::make_unique<RecordingSink>(&seen);
  auto* raw = sink.get();
  ob::OutputRow row("o1", std::move(sink));

  ob::Output o;
  o.id = "o1";
  o.sourceId = "s1";
  row.configure(o, "Bridge (Cam 1)");

  CHECK(raw->configuredId == "o1");
  CHECK(raw->configuredName == "Bridge (Cam 1)");
  CHECK(row.sourceId() == "s1");

  row.start();
  row.offer(frame(7));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (row.delivered() < 1 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();

  REQUIRE(seen.size() == 1u);
  CHECK(seen[0] == 7);
}

TEST_CASE("a row reports its sink's note") {
  auto sink = std::make_unique<RecordingSink>(nullptr);
  ob::OutputRow row("o1", std::move(sink));
  CHECK(row.note() == "note");
}
```

`RecordingSink` with `nullptr` must not dereference: guard `deliver` with `if (seen_)`.

- [ ] **Step 2: Change the header**

```cpp
class Sink {
 public:
  virtual ~Sink() = default;
  virtual void deliver(const std::shared_ptr<const Frame>& frame) = 0;
  virtual void configure(const Output& output, const std::string& advertisedName) {}
  virtual std::string note() const { return {}; }
};
using SinkFactory = std::function<std::unique_ptr<Sink>(const Output&, const std::string&)>;

class OutputRow {
 public:
  OutputRow(std::string id, std::unique_ptr<Sink> sink, size_t capacity = 4);
  ...
  // Applies the output's configuration to the row and its sink. Idempotent.
  void configure(const Output& output, const std::string& advertisedName);
  const std::string& sourceId() const { return config_.sourceId; }
  std::string note() const { return sink_ ? sink_->note() : std::string{}; }
  ...
 private:
  std::string id_;
  Output config_;
  std::unique_ptr<Sink> sink_;
  ...
```

Remove the old `Deliver` typedef, the `deliver_` member and `setSourceId`. `#include "core/bridge_model.h"`.

- [ ] **Step 3: Implement**

```cpp
OutputRow::OutputRow(std::string id, std::unique_ptr<Sink> sink, size_t capacity)
    : id_(std::move(id)), sink_(std::move(sink)), queue_(capacity) {}

void OutputRow::configure(const Output& output, const std::string& advertisedName) {
  config_ = output;  // configuration-only copy; Output::operator== is config-only by design
  if (sink_) sink_->configure(config_, advertisedName);
}
```

`run()` calls `if (sink_) sink_->deliver(*f);` inside the existing try/catch. A null sink is legal:
`Engine` passes `SinkFactory{}` until Task 7, and a row with no sink must still count offers and
must not fault.

- [ ] **Step 4: Update the other tests and run**

`tests/output_row_test.cpp`'s existing cases construct rows with lambdas — rewrite them with a small local sink:

```cpp
namespace {
class LambdaSink : public ob::Sink {
 public:
  explicit LambdaSink(std::function<void(const std::shared_ptr<const ob::Frame>&)> f)
      : f_(std::move(f)) {}
  void deliver(const std::shared_ptr<const ob::Frame>& f) override { f_(f); }
 private:
  std::function<void(const std::shared_ptr<const ob::Frame>&)> f_;
};
}
```

and `ob::OutputRow row("o1", std::make_unique<LambdaSink>([&](auto& f) { ... }));`.

`tests/bridge_graph_test.cpp` and `tests/engine_test.cpp` also construct a graph with a deliver
lambda, so they will not compile until Task 4 changes the factory. **Task 3 and Task 4 are one
change**: do both, then run the suite and make a single commit at the end of Task 4. Do not commit
or claim Task 3 green on its own — the tree does not build between them.

Run after Task 4: `.\scripts\test.ps1`
Expected: `[doctest] Status: SUCCESS!` with at least **63 cases** (57 + 4 alpha + the new
row/graph cases). Recount from the runner output; every case must pass.

---

### Task 4: `BridgeGraph` — sink factory, hub hook, demand that follows start/stop

Three M1 deferrals close here, and they are prerequisites for any adapter:

1. a **hub-created hook**, so an adapter's `onRunningChanged` is installed *before* demand moves;
2. demand **gated on `started_`**, so no adapter runs before `Engine::start()`;
3. `stop()` **releases demand** (stopping adapters) and `start()` re-applies it (restarting them).

**Files:**
- Modify: `src/engine/bridge_graph.h`, `src/engine/bridge_graph.cpp`, `tests/bridge_graph_test.cpp`, `tests/engine_test.cpp`
- Modify: `src/engine/engine.h` — one line: the in-class `BridgeGraph graph_{[](const std::shared_ptr<const Frame>&) {}};` no longer matches the new constructor. Use `BridgeGraph graph_{SinkFactory{}};`. Task 7 replaces it with the real factory.

- [ ] **Step 1: Write the failing tests**

Add to `tests/bridge_graph_test.cpp`:

```cpp
namespace {
std::unique_ptr<ob::Sink> nullSink(const ob::Output&, const std::string&) { return nullptr; }
}

TEST_CASE("a hub-created hook runs before the hub is given any demand") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});

  int installedAtRefs = -1;
  ob::BridgeGraph graph(nullSink, [&](ob::SourceHub& hub, const ob::Source&) {
    hub.setOnRunningChanged([&](bool) {});
    installedAtRefs = hub.refs();
  });
  graph.apply(b);

  CHECK(installedAtRefs == 0);  // the hook saw the hub before demand arrived
}

TEST_CASE("no adapter is demanded before start, and stop releases it") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});

  std::vector<bool> transitions;
  ob::BridgeGraph graph(nullSink, [&](ob::SourceHub& hub, const ob::Source&) {
    hub.setOnRunningChanged([&](bool on) { transitions.push_back(on); });
  });
  graph.apply(b);
  CHECK(graph.hub("s1")->refs() == 0);       // configured, but not started
  CHECK_FALSE(graph.hub("s1")->running());

  graph.start();
  CHECK(graph.hub("s1")->refs() == 1);
  CHECK(graph.hub("s1")->running());

  graph.stop();
  CHECK(graph.hub("s1")->refs() == 0);       // stop really stops the adapter
  CHECK_FALSE(graph.hub("s1")->running());
  REQUIRE(transitions.size() == 2u);         // start once, stop once
  CHECK(transitions[0] == true);
  CHECK(transitions[1] == false);
}

TEST_CASE("an unchanged re-apply still fires no transition") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});

  std::atomic<int> transitions{0};
  ob::BridgeGraph graph(nullSink);
  graph.apply(b);
  graph.start();
  graph.hub("s1")->setOnRunningChanged([&](bool) { transitions.fetch_add(1); });

  graph.apply(b);
  CHECK(transitions.load() == 0);
}
```

- [ ] **Step 1b: Update the existing graph tests for started-gated demand**

Task 4 deliberately stops demand before `start()`, so every existing case that asserts `refs()` or
`running()` after a bare `apply()` must now start the graph. Change **all seven** existing
constructions from the deliver lambda to `ob::BridgeGraph graph(nullSink);`, and add
`graph.start();` as noted:

| existing case | change |
|---|---|
| "one source feeding two outputs…" | `graph.start();` after `apply(b)` |
| "removing an output releases its ref…" | `graph.start();` after the first `apply(b)`; the second apply stays on the started graph |
| "an output with no source is kept as an idle row" | constructor only |
| "fan-out offers each published frame…" | constructor only (already calls `start()`) |
| "publishing to a source that has no rows…" | constructor only |
| "re-applying an unchanged model fires no running transitions" | `graph.start();` after `apply(b)`, before installing the counter |
| "an output that moves to another source moves its ref" | `graph.start();` after `apply(b)` |

Do not weaken any assertion: `refs()` and `running()` keep their expected values because the graph
is now started.

Also rewrite the first `tests/engine_test.cpp` case, because demand is no longer applied at
`apply()`:

```cpp
TEST_CASE("applying a bridge starts rows and refs the hub") {
  ob::Engine engine(0, "token");
  engine.apply(twoOutputsOneSource());
  REQUIRE(engine.graph().hub("s1") != nullptr);
  // Demand is gated on start(): configuring must not open an adapter.
  CHECK(engine.graph().hub("s1")->refs() == 0);
  CHECK_FALSE(engine.graph().hub("s1")->running());
  engine.start();
  CHECK(engine.graph().hub("s1")->refs() == 2);
  CHECK(engine.graph().rowCount() == 2u);
  engine.stop();
}
```

The second engine test already calls `apply()` then `start()`, so it needs no change.

- [ ] **Step 2: Change the header**

```cpp
  // SinkFactory is namespace-scope in output_row.h; both are in namespace ob.
  using HubReady = std::function<void(SourceHub& hub, const Source& source)>;

  explicit BridgeGraph(SinkFactory sinks, HubReady onHubCreated = {});
  ...
  // Non-const: tests install the hub-owned running hook through this accessor. Demand is still
  // the graph's business — never addRef()/release() through it.
  SourceHub* hub(const std::string& sourceId) const;
  ...
 private:
  // Moves every hub to `wanted`, by delta, with mu_ released. Called by apply/start.
  void applyDemand(const std::map<std::string, int>& wanted);
  ...
  SinkFactory sinks_;
  HubReady onHubCreated_;
  std::map<std::string, int> demand_;  // per source id, the demand the model implies
```

- [ ] **Step 3: Implement `apply`, `start`, `stop`, `applyDemand`, `publish`**

```cpp
void BridgeGraph::apply(const Bridge& bridge) {
  std::vector<std::pair<SourceHub*, Source>> created;
  std::vector<OutputRow*> toStart;
  std::vector<std::unique_ptr<OutputRow>> retired;
  std::map<std::string, int> wanted;
  {
    std::lock_guard l(mu_);

    for (const Source& s : bridge.sources) {
      if (hubs_.find(s.id) == hubs_.end()) {
        auto hub = std::make_unique<SourceHub>(s.id);
        created.emplace_back(hub.get(), s);
        hubs_.emplace(s.id, std::move(hub));
      }
    }
    for (auto it = hubs_.begin(); it != hubs_.end();) {
      if (bridge.findSource(it->first) == nullptr) it = hubs_.erase(it); else ++it;
    }

    for (auto it = rows_.begin(); it != rows_.end();) {
      if (bridge.findOutput(it->first) == nullptr) {
        retired.push_back(std::move(it->second.row));  // destroyed after mu_ is released
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
        if (started_) toStart.push_back(found->second.row.get());
      } else {
        found->second.row->configure(o, advertised);  // the sink decides whether to reopen
      }
    }

    for (auto& [id, hub] : hubs_) wanted[id] = 0;
    for (const Output& o : bridge.outputs)
      if (hubUnlocked(o.sourceId) != nullptr) ++wanted[o.sourceId];
  }

  retired.clear();                                    // join retired rows with mu_ released
  for (auto& [hub, source] : created)
    if (onHubCreated_) onHubCreated_(*hub, source);   // install the adapter BEFORE demand
  for (OutputRow* r : toStart) r->start();            // spawn workers outside mu_
  applyDemand(wanted);
  std::lock_guard l(mu_);
  demand_ = std::move(wanted);                        // remember for start()/stop()
}
```

`applyDemand` (called with mu_ released):

```cpp
void BridgeGraph::applyDemand(const std::map<std::string, int>& wanted) {
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
```

`start` / `stop`:

```cpp
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
  applyDemand(wanted);  // now the adapters may run
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
    while (h->refs() > 0) h->release();  // closes every adapter, including a demanded one
}
```

`applyDemand` must be a no-op while stopped: guard it at the top with `{ std::lock_guard l(mu_); if (!started_) return; }` — otherwise `apply()` while stopped would still start adapters.

`publish` now stores through the hub:

```cpp
void BridgeGraph::publish(const std::string& sourceId, std::shared_ptr<Frame> frame) {
  std::lock_guard l(mu_);
  // Pass a copy, not std::move: our own reference is still needed for the row fan-out below.
  // storeFrame adjusts `format.alpha` on the shared Frame object, so rows see the same decision.
  if (SourceHub* h = hubUnlocked(sourceId)) h->storeFrame(frame);
  for (auto& [id, r] : rows_)
    if (r.row->sourceId() == sourceId) { r.row->offer(frame); ++r.offered; }
}
```

Do **not** `std::move(frame)` into `storeFrame`: it would leave `frame` empty and every row would
be offered a null pointer. Row fan-out happens under `mu_`, after the hub store, on the same Frame
object — so a row never sees a frame whose alpha decision differs from the hub's.

- [ ] **Step 4: Engine's null sinks**

`Engine`'s graph is constructed with `SinkFactory{}` for now, so every row's sink is null.
`OutputRow` must tolerate that: `run()` calls `sink_->deliver(*f)` only when `sink_ != nullptr`,
and delivers nothing otherwise. `twoOutputsOneSource()` rows therefore have null sinks, and the
fan-out case still counts offers (`offeredToRow`), not deliveries.

- [ ] **Step 5: Run and commit**

Run: `.\scripts\test.ps1`
Expected: `[doctest] Status: SUCCESS!` with at least **63 cases** (57 + 4 alpha + the new
row/graph cases). Recount from the runner output; every case must pass.

```bash
git add src/engine/output_row.h src/engine/output_row.cpp src/engine/bridge_graph.h src/engine/bridge_graph.cpp src/engine/engine.h tests/output_row_test.cpp tests/bridge_graph_test.cpp tests/engine_test.cpp
git commit -m "feat(engine): rows own their sink; the graph starts adapters only when started"
```

---

### Task 5: `OmtSource` — OMT and OMT-HX in

**Files:**
- Create: `src/engine/omt_source.h`, `src/engine/omt_source.cpp`
- Modify: `CMakeLists.txt` (`ob_engine`)

- [ ] **Step 1: Header**

```cpp
#pragma once
// One OMT or OMT-HX input (design 8). Runs only while its SourceHub is demanded: start() opens an
// omthx::Receiver, stop() closes it. ONE receiver serves both loops — the library states that
// receiveVideo and receiveAudio may run on two threads, one call type per thread.
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/frame.h"
#include "core/signal_state.h"

namespace ob {

struct OmtReceiverHolder;  // hides omthx from this header

class OmtSource {
 public:
  using FrameSink = std::function<void(std::shared_ptr<Frame>)>;
  // Called from the start/stop caller and from the video/audio threads. It must not block, must
  // not re-enter OmtSource, and must synchronise its own storage.
  using StateSink = std::function<void(SignalState, std::optional<ErrorInfo>)>;

  OmtSource(std::string address, FrameSink onFrame, StateSink onState);
  ~OmtSource();
  OmtSource(const OmtSource&) = delete;
  OmtSource& operator=(const OmtSource&) = delete;

  void start();  // idempotent; opens the receiver; Connecting (Offline if it cannot open)
  void stop();   // idempotent; joins both threads, closes the receiver; Ready
  bool running() const;

 private:
  void stopImpl(bool notify);
  void videoLoop(OmtReceiverHolder& rx);
  void audioLoop(OmtReceiverHolder& rx);

  std::string address_;
  FrameSink onFrame_;
  StateSink onState_;
  std::atomic<bool> stop_{false};
  mutable std::mutex mu_;   // fields only; never join while holding it
  std::mutex lifecycle_;    // serialises whole start()/stop() bodies
  std::unique_ptr<OmtReceiverHolder> rx_;  // guarded by mu_; alive only while running
  std::thread video_, audio_;
};

}  // namespace ob
```

- [ ] **Step 2: Implementation**

```cpp
#include "engine/omt_source.h"

#include <chrono>
#include <exception>

#include <omthx/omthx.h>

namespace ob {

struct OmtReceiverHolder {
  explicit OmtReceiverHolder(const std::string& address)
      : receiver(address, omthx::PixelFormat::Uyvy) {}
  omthx::Receiver receiver;
};

namespace {
constexpr int kReceiveTimeoutMs = 100;
constexpr auto kOfflineAfter = std::chrono::seconds(2);
}

OmtSource::OmtSource(std::string address, FrameSink onFrame, StateSink onState)
    : address_(std::move(address)), onFrame_(std::move(onFrame)), onState_(std::move(onState)) {}

OmtSource::~OmtSource() { stopImpl(false); }  // no state callback from a destructor

bool OmtSource::running() const {
  std::lock_guard l(mu_);
  return rx_ != nullptr;  // true while a receiver is open, including the join window
}

void OmtSource::start() {
  std::lock_guard lifecycle(lifecycle_);
  {
    std::lock_guard l(mu_);
    if (video_.joinable() || audio_.joinable()) return;
  }
  std::unique_ptr<OmtReceiverHolder> rx;
  try {
    rx = std::make_unique<OmtReceiverHolder>(address_);
  } catch (const std::exception& e) {
    // Never let an open failure escape into the graph's demand path.
    if (onState_) onState_(SignalState::Offline, ErrorInfo{"omt-open", e.what()});
    return;
  }
  OmtReceiverHolder* holder = rx.get();
  {
    std::lock_guard l(mu_);
    stop_.store(false);
    rx_ = std::move(rx);
  }
  if (onState_) onState_(SignalState::Connecting, std::nullopt);

  std::thread v, a;
  try {
    v = std::thread([this, holder] { videoLoop(*holder); });
    a = std::thread([this, holder] { audioLoop(*holder); });
  } catch (const std::exception& e) {
    stop_.store(true);
    if (v.joinable()) v.join();
    {
      std::lock_guard l(mu_);
      rx_.reset();
    }
    if (onState_) onState_(SignalState::Offline, ErrorInfo{"omt-thread", e.what()});
    return;
  }
  {
    std::lock_guard l(mu_);
    video_ = std::move(v);
    audio_ = std::move(a);
  }
}

void OmtSource::stop() { stopImpl(true); }

void OmtSource::stopImpl(bool notify) {
  std::lock_guard lifecycle(lifecycle_);
  std::thread v, a;
  bool hadThreads = false;
  {
    std::lock_guard l(mu_);
    hadThreads = video_.joinable() || audio_.joinable();
    if (hadThreads) {
      stop_.store(true);
      v = std::move(video_);
      a = std::move(audio_);
    }
  }
  if (v.joinable()) v.join();
  if (a.joinable()) a.join();
  {
    std::lock_guard l(mu_);
    rx_.reset();  // closed only after both loops are gone
  }
  if (notify && onState_) onState_(SignalState::Ready, std::nullopt);
}

void OmtSource::videoLoop(OmtReceiverHolder& rx) {
  try {
    // Uyvy is the canonical frame: the UYVY plane followed by the alpha plane, exactly UYVA.
    bool live = false;
    auto last = std::chrono::steady_clock::now();
    while (!stop_.load()) {
      omthx::VideoFrame vf;
      if (rx.receiver.receiveVideo(vf, kReceiveTimeoutMs)) {
        last = std::chrono::steady_clock::now();
        auto f = std::make_shared<Frame>();
        f->format = VideoFormat{vf.format.width, vf.format.height, vf.format.fpsN,
                                vf.format.fpsD, vf.format.alpha};
        f->premultiplied = vf.premultiplied;
        f->timestamp = vf.timestamp;
        f->uyvy = std::move(vf.uyvy);
        if (!f->valid()) continue;  // malformed peer: never publish a frame the hub rejects
        if (!live) {
          live = true;
          if (onState_) onState_(SignalState::Live, std::nullopt);
        }
        if (onFrame_) onFrame_(std::move(f));
      } else if (live && std::chrono::steady_clock::now() - last > kOfflineAfter) {
        live = false;  // was live, no video
        if (onState_) onState_(SignalState::Offline, std::nullopt);
      }
    }
  } catch (const std::exception& e) {
    if (onState_) onState_(SignalState::Offline, ErrorInfo{"omt-receive", e.what()});
  }
}

void OmtSource::audioLoop(OmtReceiverHolder& rx) {
  omthx::AudioFrame af;  // hoisted: receiveAudio reuses the planar vector's capacity
  try {
    while (!stop_.load()) {
      if (rx.receiver.receiveAudio(af, kReceiveTimeoutMs)) {
        // Audio passthrough is M3 (design 20 does not list it in M2's done-criteria). Draining
        // keeps the session's audio path moving instead of queuing behind an unread stream.
      }
    }
  } catch (const std::exception&) {
    // Audio failure must not take video down; the video path is independent.
  }
}

}  // namespace ob
```

The receiver is opened **before** either thread starts and closed **after** both join, so the two
loops share one session and one network subscription. Do **not** create a second `Receiver` for audio:
the header explicitly permits both calls on one object, one call type per thread.

`start()` and `stop()` may be called from different threads. A `lifecycle_` mutex serialises the
**whole** bodies, joins included; `mu_` only guards fields. Without it, one `stop()` could move the
threads out (making them non-joinable) while another `stop()` or `start()` sees no threads and
resets `rx_` under loops that are still running — a use-after-free. Serialising the whole bodies
also orders state: a waiting `start()` runs only after the previous `stop()` has emitted `Ready`.

> **Audio is drained, not delivered.** Design §10 requires passthrough, but §20 does not list
> audio in M2's done-criteria, so delivery is **deferred to M3** with the NDI adapters
> (`Sender::sendAudio` takes planar float, sample rate and channel count; the same plumbing
> serves both). Draining here keeps the session healthy — it is not a delivery path, and M2 must
> never be reported complete while claiming audio works.

- [ ] **Step 3: Build**

Add `src/engine/omt_source.cpp` to `ob_engine`, and link OMT-HX from it publicly:

```cmake
target_link_libraries(ob_engine PUBLIC omthx)
```

`omthx` always exists: `cmake/omt-runtime.cmake` fails configuration when OMT-HX is missing. Only
`ob_tests` linked it before this task, so without this line the engine library does not resolve
`omthx::Receiver`.

Run: `.\scripts\test.ps1`
Expected: `SUCCESS`; no new cases yet.

- [ ] **Step 4: Commit**

```bash
git add src/engine/omt_source.h src/engine/omt_source.cpp CMakeLists.txt
git commit -m "feat(engine): OmtSource, an OMT/OMT-HX receiver producing canonical frames"
```

---

### Task 6: `OmtSink` — OMT and OMT-HX out

**Files:**
- Create: `src/engine/omt_sink.h`, `src/engine/omt_sink.cpp`, `tests/omt_sink_test.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

The test does not need a network peer: it asserts the *decision* logic (alpha off, H.264 rule) through a seam. Give `OmtSink` a protected `virtual void send(const VideoFormat&, const uint8_t*, bool alpha, bool premultiplied, int64_t)`, so a test subclass records what would be sent.

```cpp
// tests/omt_sink_test.cpp
#include <doctest/doctest.h>

#include "engine/omt_sink.h"

namespace {
class TestSink : public ob::OmtSink {
 public:
  using ob::OmtSink::OmtSink;
  struct Sent { int w, h; bool alpha, premultiplied; int64_t ts; };
  std::vector<Sent> sent;
 protected:
  void openLocked() override {}  // no network sender in a unit test
  void send(const ob::VideoFormat& f, const uint8_t*, bool alpha, bool premultiplied,
            int64_t ts) override {
    sent.push_back({f.width, f.height, alpha, premultiplied, ts});
  }
};
}

static ob::Output output(ob::OutputTarget target, ob::HxCodec codec, ob::AlphaMode alpha) {
  ob::Output o;
  o.id = "o1";
  o.target = target;
  o.codec = codec;
  o.alpha = alpha;
  return o;
}

TEST_CASE("alpha follows the source under Auto, and is dropped under Off") {
  TestSink autoSink;
  autoSink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::Hevc, ob::AlphaMode::Auto), "B (A)");
  auto frame = std::make_shared<ob::Frame>();
  frame->format = ob::VideoFormat{4, 2, 60, 1, true};
  frame->uyvy.assign(frame->lumaBytes() + frame->alphaBytes(), 0x80);
  autoSink.deliver(frame);
  REQUIRE(autoSink.sent.size() == 1u);
  CHECK(autoSink.sent[0].alpha);

  TestSink offSink;
  offSink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::Hevc, ob::AlphaMode::Off), "B (A)");
  offSink.deliver(frame);
  REQUIRE(offSink.sent.size() == 1u);
  CHECK_FALSE(offSink.sent[0].alpha);
}

TEST_CASE("H.264 above 1080p drops alpha and says why") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::H264, ob::AlphaMode::Auto), "B (A)");
  auto frame = std::make_shared<ob::Frame>();
  frame->format = ob::VideoFormat{1920, 2160, 60, 1, true};
  frame->uyvy.assign(frame->lumaBytes() + frame->alphaBytes(), 0x80);
  sink.deliver(frame);
  REQUIRE(sink.sent.size() == 1u);
  CHECK_FALSE(sink.sent[0].alpha);
  CHECK(sink.note() == "alpha dropped — H.264 above 1080p, use HEVC");
}

TEST_CASE("an OMT-HX output of 1080p or less keeps H.264 alpha") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::H264, ob::AlphaMode::Auto), "B (A)");
  auto frame = std::make_shared<ob::Frame>();
  frame->format = ob::VideoFormat{1920, 1080, 60, 1, true};
  frame->uyvy.assign(frame->lumaBytes() + frame->alphaBytes(), 0x80);
  sink.deliver(frame);
  REQUIRE(sink.sent.size() == 1u);
  CHECK(sink.sent[0].alpha);
}
```

- [ ] **Step 2: Header**

```cpp
#pragma once
// One OMT or OMT-HX output (design 8, 9.3). Each row owns its own omthx::Sender, so a new output
// joins cleanly and a keyframe is forced on its first frame by the library.
#include <memory>
#include <mutex>
#include <string>

#include "core/bridge_model.h"
#include "core/frame.h"
#include "engine/output_row.h"

namespace ob {

class OmtSink : public Sink {
 public:
  OmtSink() = default;
  ~OmtSink() override;
  OmtSink(const OmtSink&) = delete;
  OmtSink& operator=(const OmtSink&) = delete;

  void configure(const Output& output, const std::string& advertisedName) override;
  void deliver(const std::shared_ptr<const Frame>& frame) override;
  std::string note() const override;

 protected:
  // Overridden by the test to observe the decision without opening a sender.
  virtual void send(const VideoFormat& format, const uint8_t* uyvy, bool alpha,
                    bool premultiplied, int64_t timestamp);
  // Overridden by the test so configure() opens no network sender.
  virtual void openLocked();

 private:

  mutable std::mutex mu_;
  Output config_;
  std::string name_;
  std::string note_;
  std::unique_ptr<struct OmtSenderHolder> sender_;  // hides omthx from this header
};

}  // namespace ob
```

`sender_` is hidden behind an opaque holder so `omthx/omthx.h` stays out of `ob_engine`'s public header.

- [ ] **Step 3: Implementation**

```cpp
#include "engine/omt_sink.h"

#include <omthx/omthx.h>

namespace ob {

struct OmtSenderHolder {
  std::unique_ptr<omthx::Sender> sender;
};

namespace {
omthx::Quality qualityOf(Quality q) {
  switch (q) {
    case Quality::Low: return omthx::Quality::Low;
    case Quality::Medium: return omthx::Quality::Medium;
    case Quality::High: return omthx::Quality::High;
  }
  return omthx::Quality::Medium;
}
omthx::Codec codecOf(HxCodec c) { return c == HxCodec::H264 ? omthx::Codec::H264 : omthx::Codec::Hevc; }
}

OmtSink::~OmtSink() = default;

// A Sender cannot be reconfigured, so configure() closes and reopens when anything relevant
// changed, comparing against the previously stored config.
void OmtSink::configure(const Output& output, const std::string& advertisedName) {
  std::lock_guard l(mu_);
  const bool same = sender_ && config_.target == output.target && config_.codec == output.codec &&
                    config_.quality == output.quality && name_ == advertisedName;
  config_ = output;
  if (same) return;
  name_ = advertisedName;
  openLocked();
}

void OmtSink::openLocked() {
  sender_.reset();
  note_.clear();
  if (config_.target == OutputTarget::Ndi) {
    note_ = "NDI output needs the NDI adapter (M3)";
    return;  // no sender: deliver() will drop frames, which is correct for an unavailable target
  }
  const auto mode = config_.target == OutputTarget::OmtHx ? omthx::Mode::Hx : omthx::Mode::Full;
  sender_ = std::make_unique<OmtSenderHolder>();
  sender_->sender = std::make_unique<omthx::Sender>(name_, mode, qualityOf(config_.quality),
                                                    codecOf(config_.codec));
}

void OmtSink::deliver(const std::shared_ptr<const Frame>& frame) {
  if (!frame || !frame->valid()) return;  // never index a plane the frame does not have
  std::lock_guard l(mu_);

  bool alpha = frame->format.alpha && config_.alpha != AlphaMode::Off;
  if (alpha && config_.target == OutputTarget::OmtHx && config_.codec == HxCodec::H264 &&
      frame->format.height > 1080) {
    alpha = false;
    note_ = "alpha dropped — H.264 above 1080p, use HEVC";
  }
  send(frame->format, frame->uyvy.data(), alpha, frame->premultiplied, frame->timestamp);
}

void OmtSink::send(const VideoFormat& format, const uint8_t* uyvy, bool alpha, bool premultiplied,
                   int64_t timestamp) {
  if (!sender_ || !sender_->sender) return;
  omthx::VideoFormat vf{format.width, format.height, format.fpsN, format.fpsD, alpha};
  const uint8_t* plane = alpha ? uyvy + static_cast<size_t>(format.width) * 2u * format.height : nullptr;
  const int alphaStride = static_cast<int>(format.width);
  sender_->sender->sendVideoUyvy(vf, uyvy, format.width * 2, plane, alphaStride, premultiplied,
                                 timestamp);
}

std::string OmtSink::note() const {
  std::lock_guard l(mu_);
  return note_;
}

}  // namespace ob
```

`send` is called with `mu_` held, so the test override must not take `mu_`.

- [ ] **Step 4: Make `OmtSink` constructible by the factory**

`SinkFactory` returns `std::unique_ptr<Sink>`, so the engine uses:

```cpp
std::unique_ptr<ob::Sink> makeOmtSink(const ob::Output& o, const std::string& advertised) {
  auto sink = std::make_unique<ob::OmtSink>();
  sink->configure(o, advertised);
  return sink;
}
```

- [ ] **Step 5: Run and commit**

Add `src/engine/omt_sink.cpp` to `ob_engine`, `tests/omt_sink_test.cpp` to `ob_tests`.

Run: `.\scripts\test.ps1`
Expected: `SUCCESS` with the three new OMT-sink cases passing.

```bash
git add src/engine/omt_sink.h src/engine/omt_sink.cpp tests/omt_sink_test.cpp CMakeLists.txt
git commit -m "feat(engine): OmtSink, with the alpha:Off and H.264-above-1080p rules"
```

---

### Task 7: Wire the adapters into `Engine`, and report runtime state

**Files:**
- Modify: `src/engine/engine.h`, `src/engine/engine.cpp`, `src/engine/command_router.h`, `src/engine/command_router.cpp`, `tests/command_router_test.cpp`, `tests/engine_test.cpp`

- [ ] **Step 1: Router runtime provider**

```cpp
// command_router.h
struct SourceRuntime {
  SignalState state = SignalState::Ready;
  VideoFormatView format;
};
struct OutputRuntime {
  SignalState state = SignalState::Ready;
  std::string note;
  uint64_t delivered = 0;
  uint64_t dropped = 0;
};
using SourceRuntimeFn = std::function<std::optional<SourceRuntime>(const std::string& id)>;
using OutputRuntimeFn = std::function<std::optional<OutputRuntime>(const std::string& id)>;
void setRuntime(SourceRuntimeFn sources, OutputRuntimeFn outputs);
```

`state` adds, per source, `"state": toString(rt.state)` and, when `rt.format.known`, `"width"/"height"/"fpsN"/"fpsD"/"alpha"`; per output, `"state"`, `"note"`, `"delivered"`, `"dropped"`.

- [ ] **Step 2: Engine owns adapters and runtime**

```cpp
// engine.h (private)
  void onHubCreated(SourceHub& hub, const Source& source);
  void startAdapter(const std::string& sourceId);
  void stopAdapter(const std::string& sourceId);
  SourceRuntime sourceRuntime(const std::string& id) const;
  OutputRuntime outputRuntime(const std::string& id) const;

  Bridge model_;                                               // last applied model
  std::map<std::string, std::unique_ptr<OmtSource>> adapters_;  // guarded by controlMu_
  std::map<std::string, SourceRuntime> sourceRuntime_;          // guarded by controlMu_
```

`Engine::Engine` builds the graph with `makeOmtSink` and `[this](SourceHub& h, const Source& s) { onHubCreated(h, s); }`.

`onHubCreated` installs `setOnRunningChanged([this, id](bool on){ if (on) startAdapter(id); else stopAdapter(id); })`.

`startAdapter` looks the source up in `model_`; `SourceKind::Ndi` sets `sourceRuntime_[id].state = Offline` with the note "NDI needs M3" and returns; otherwise it creates `OmtSource(address, frameSink, stateSink)` where
- `frameSink` = `[this, id](std::shared_ptr<Frame> f) { graph_.publish(id, std::move(f)); }`
- `stateSink` = `[this, id](SignalState s, std::optional<ErrorInfo>) { sourceRuntime_[id].state = s; }`

`sourceRuntime`/`outputRuntime` fill the format from `graph_.hub(id)->latest()` and the counters from a new `BridgeGraph::rowStats(id)`. Add:

```cpp
struct RowStats { uint64_t offered = 0, delivered = 0, dropped = 0; bool faulted = false; };
std::optional<RowStats> rowStats(const std::string& outputId) const;
```

- [ ] **Step 3: Test the state reply carries liveness**

Add to `tests/command_router_test.cpp`:

```cpp
TEST_CASE("state reports runtime liveness when the engine supplies it") {
  ob::CommandRouter router;
  router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-source","kind":"OMT","address":"PC"})");
  router.setRuntime(
      [](const std::string& id) -> std::optional<ob::SourceRuntime> {
        ob::SourceRuntime rt;
        rt.state = ob::SignalState::Live;
        rt.format = ob::VideoFormatView{1920, 1080, 60, 1, false, true};
        return id == "s1" ? std::optional<ob::SourceRuntime>(rt) : std::nullopt;
      },
      [](const std::string&) { return std::nullopt; });

  const auto reply = router.handle(ob::ClientId{"c1"}, R"({"cmd":"state"})");
  CHECK(reply.find("\"state\":\"live\"") != std::string::npos);
  CHECK(reply.find("\"width\":1920") != std::string::npos);
}
```

- [ ] **Step 4: Run and commit**

Run: `.\scripts\test.ps1`
Expected: `SUCCESS`, all cases.

```bash
git add src/engine/engine.h src/engine/engine.cpp src/engine/command_router.h src/engine/command_router.cpp src/engine/bridge_graph.h src/engine/bridge_graph.cpp tests/command_router_test.cpp tests/engine_test.cpp
git commit -m "feat(engine): wire OMT adapters to hubs and rows, and report runtime state"
```

---

### Task 8: The loopback proof, and the measurement the design asks for

Design §20 makes M2 done when `omthx-send → bridge → omthx-recv --dump` works and the per-output cost is written down.

**Files:**
- Create: `scripts/m2-e2e.ps1`
- Modify: `docs/superpowers/specs/2026-10-19-omt-bridge-design.md` §16 (the measured numbers)

- [ ] **Step 1: Write the script**

```powershell
# scripts/m2-e2e.ps1 — OMT-HX in -> OMT Bridge -> OMT-HX out, on loopback, using OMT-HX's own tools.
param(
  [string]$Engine    = "build\release\omt-bridge-engine.exe",
  [string]$SendExe   = "build\release\omthx-send.exe",
  [string]$RecvExe   = "build\release\omthx-recv.exe",
  [string]$Bridge    = "$env:TEMP\m2-e2e.ombridge.json",
  [string]$Dump      = "$env:TEMP\m2-e2e.bmp"
)
$ErrorActionPreference = "Stop"
$port = 5996; $token = "e2e-token"

# One source (the omthx-send we are about to start) and one OMT-HX output.
$json = @'
{
  "name": "E2E",
  "sources": [ { "id": "s1", "kind": "OMT-HX", "address": "E2E SRC" } ],
  "outputs": [ { "id": "o1", "sourceId": "s1", "target": "OMT-HX", "codec": "HEVC", "quality": "Medium", "name": "OUT" } ]
}
'@
[IO.File]::WriteAllText($Bridge, ($json -replace "`r`n","`n"))

# The far ends. omthx-send advertises "E2E SRC"; the bridge republishes as "E2E (OUT)".
$send  = Start-Process $SendExe -ArgumentList '--name','E2E SRC' -PassThru -NoNewWindow
$eng   = Start-Process $Engine  -ArgumentList '--port',$port,'--token',$token,'--bridge',$Bridge -RedirectStandardOutput "$env:TEMP\m2-eng.out" -PassThru -NoNewWindow
Start-Sleep -Seconds 3   # discovery, and the HX encoder to open

$recv  = Start-Process $RecvExe -ArgumentList 'E2E (OUT)','--dump',$Dump -PassThru -NoNewWindow
Start-Sleep -Seconds 6

foreach ($p in @($recv,$eng,$send)) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
Start-Sleep -Milliseconds 500

if (-not (Test-Path $Dump)) { throw "no dump written: the bridge did not deliver a picture" }
$len = (Get-Item $Dump).Length
Write-Output "DUMP=$Dump bytes=$len"
if ($len -lt 1000) { throw "dump too small to be a picture: $len bytes" }
Write-Output "M2 E2E OK"
```

- [ ] **Step 2: Run it**

Run: `.\scripts\build.ps1` (to get `omthx-send`/`omthx-recv`; if OMT-HX's tools are not built, build them with `cmake --build --preset release --target omthx-send omthx-recv`), then `.\scripts\m2-e2e.ps1`.

Expected: `M2 E2E OK`, a BMP larger than 1 KB.

If the tools are not part of the build, add them in `CMakeLists.txt` with `set(OMTHX_BUILD_TESTS ON)` before `add_subdirectory`, or run the equivalent OMT-HX preset — record which in the commit message.

- [ ] **Step 3: Measure and write it down**

Run the bridge with one HX source and 1, 2 and 4 HX outputs at 1080p60, and record: per-output CPU cost, the achieved fps per output, and the 1080p60-alpha HX→HX rate. Add the numbers to the design's §16 bullet that currently says *"M2 measures the bridge's own per-output cost and documents it"*, replacing the promise with the measurement and the date.

- [ ] **Step 4: Commit**

```bash
git add scripts/m2-e2e.ps1 docs/superpowers/specs/2026-10-19-omt-bridge-design.md
git commit -m "test(m2): loopback proof and measured per-output cost"
```

---

## Self-review

**Spec coverage.** §6.3 canonical UYVY frame — already M1. §6.4 hub/adapter demand — Tasks 2 and 4. §6.5 row isolation with a real sink — Tasks 3 and 6. §7.2 each output owns its Sender — Task 6. §8 `OmtSource`/`OmtSink` — Tasks 5 and 6. §9.2 opaque alpha — Tasks 1 and 2. §9.3 H.264 alpha — Task 6. §10 audio — Task 5 drains it; **delivery is deferred and must be named as such** (see below). §12.1 states — Tasks 5 and 7. §15.2 loopback — Task 8. §20 M2 done-criteria — Tasks 6 and 8 plus the §16 measurement.

**Known gaps, stated rather than hidden.** Audio is drained in Task 5 and not delivered to sinks;
§10 requires passthrough. **Deferred to M3**, on the grounds that §20's M2 done-criteria do not
include audio and that the same audio plumbing (`Sender::sendAudio`, planar float + rate +
channels) then serves OMT and NDI together. M2 must not be reported complete while claiming audio
works.

**Type consistency.** `Sink::deliver` takes `shared_ptr<const Frame>`; `BridgeGraph::publish` takes `shared_ptr<Frame>` (the hub may clear alpha before publishing); `SourceHub::storeFrame` takes `shared_ptr<Frame>` and stores `shared_ptr<const Frame>`. `SourceHub::storeFrame` is the only writer of the slot; `BridgeGraph::publish` is the only fan-out.
