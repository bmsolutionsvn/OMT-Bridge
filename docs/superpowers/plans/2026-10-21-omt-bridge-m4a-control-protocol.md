# OMT-Bridge M4a — Control Protocol, Stats and Discovery

> **Executed 2026-10-21.** The UI also needed `set-running`, output `codec`/`quality`/`alpha` and
> source `codec` in `state`, `add-output` defaults from settings, and the engine's `--discovery`
> file (so a reopened shell adopts the running engine). All are covered by the tests below.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete the control protocol the UI needs: edit and remove outputs and sources, settings, live fps/bitrate/connections, and a discovery list for adding sources.

**Architecture:** The UI is a WebSocket client; the engine already answers `state`, `add-source`, `remove-source`, `add-output`, `set-output-source`. This plan adds the missing edit/remove/settings/discovery commands and extends the per-output runtime with measured rates. Discovery is supplied by the engine (OMT `discover()` plus an NDI find) through an injected provider, so the router stays testable without devices. The UI plan (M4b) consumes only what this plan locks in.

**Tech Stack:** C++20, MSVC, CMake + vcpkg, OMT-HX `e21dc52`, NDI 6 SDK (optional), nlohmann/json, doctest.

**Spec:** `docs/superpowers/specs/2026-10-19-omt-bridge-design.md` §5, §6.1, §6.6, §9.3, §12.1, §13.1–§13.4.

## Global Constraints

- Every command answers one JSON object; failures are `{"error":"<what and why>","ok":false}` and never throw to the caller.
- Wire names come from `core/bridge_model.h` (`toString`/`*FromString`); no second spelling table.
- `Engine::apply` / `applyLocked` remains the only path that reconciles the graph; a command edits the router's model and the engine applies once.
- Lock order stays `Engine::controlMu_` → graph `mu_` → hub `mu_` → row queue → sink `mu_`.
- Every file ends with a single LF (0x0A), no CR.
- doctest stringifies every `CHECK` operand: assert `bool`/`int`/`std::string`, never an enum, struct or smart pointer.
- NDI code is compiled only with `OB_WITH_NDI`; discovery must work with it off.
- Build/test only via `scripts/build.ps1` / `scripts/test.ps1`.

## Review Focus

- **A command that edits an id it does not know** must refuse, not create; a mistyped `set-output` id must never mint a row. Task 3/4 pin this.
- **An invalid enum string** (`target: "OMTX"`) must be refused by name, not silently defaulted. Task 3 pins it.
- **A source kind change** must restart the adapter and keep the outputs attached; demand must not be lost. Task 4 pins it at the router level and reuses the engine test.
- **Stats deltas** must not divide by zero or report a negative rate when the clock is unchanged or a counter resets. Task 2 pins both.
- **Discovery must not hang the control plane forever**: OMT is immediate, NDI waits at most one second, and the router returns even when the SDK is absent. Task 5 pins the timeout contract with a fake provider.

---

### Task 1: Sink and row stats

**Files:**
- Modify: `src/engine/output_row.h`, `.cpp`, `src/engine/omt_sink.h`, `.cpp`, `src/engine/ndi_sink.h`, `.cpp`, `src/engine/bridge_graph.h`, `.cpp`
- Test: `tests/output_row_test.cpp`, `tests/omt_sink_test.cpp`, `tests/bridge_graph_test.cpp`

**Interfaces:**
- Produces:
```cpp
// output_row.h
struct SinkStats {
  uint64_t bytesSent = 0;  // wire bytes; 0 when the transport cannot report them
  int connections = 0;
};
// Sink
virtual SinkStats stats() const { return {}; }
// OutputRow
SinkStats sinkStats() const { return sink_ ? sink_->stats() : SinkStats{}; }
// BridgeGraph::RowStats gains
uint64_t bytesSent = 0;
int connections = 0;
```

- [ ] **Step 1: failing tests**

In `tests/omt_sink_test.cpp`, extend `TestSink` with a `stats_` field and override `stats()`; add:

```cpp
TEST_CASE("an OMT sink reports its sender's counters") {
  TestSink sink;
  sink.configure(output(ob::OutputTarget::OmtHx, ob::HxCodec::Hevc, ob::AlphaMode::Auto), "B (A)");
  sink.stats_ = {1234, 2};
  const auto s = sink.stats();
  CHECK(s.bytesSent == 1234u);
  CHECK(s.connections == 2);
}
```

In `tests/output_row_test.cpp`, add a sink with `stats()` and assert `row.sinkStats()` forwards it.

In `tests/bridge_graph_test.cpp`, add `CHECK(graph.rowStats("o1")->bytesSent == ...)` using a counting sink.

- [ ] **Step 2: run and fail**, **Step 3: implement**
  - `OmtSink::stats()`: lock `mu_`; if `sender_ && sender_->sender`, map `SenderStats{bytesSent, connections}`; else `{}`.
  - `NdiSink::stats()`: lock `mu_`; `connections = NDIlib_send_get_no_connections(send, 0)` when open; `bytesSent` stays 0 (the public NDI API has no byte counter).
  - Row and graph forwarding as above.

- [ ] **Step 4: run green**, **Step 5: commit** — `git commit -m "feat(engine): expose sink byte and connection counters"`.

---

### Task 2: Per-output fps, Mbps and connections in the state reply

**Files:**
- Modify: `src/engine/command_router.h`, `src/engine/engine.h`, `.cpp`
- Test: `tests/command_router_test.cpp`, `tests/engine_test.cpp`

**Interfaces:**
- `OutputRuntime` gains `double fps = 0; double mbps = 0; int connections = 0;`, and `Engine::outputRuntime` becomes **non-const** (it records a rate sample); the router lambda already captures a non-const `this`.
- `state` emits `fps`, `mbps`, `connections` beside `delivered`/`dropped`.
- Engine keeps a per-output sample: `struct RateSample { std::chrono::steady_clock::time_point at; uint64_t frames; uint64_t bytes; };` in `std::map<std::string, RateSample> rates_`, guarded by `runtimeMu_` (or `controlMu_`, since only `state` writes it — use `controlMu_`).

- [ ] **Step 1: failing tests**

Router: inject an `OutputRuntime` with `fps = 59.5`, `mbps = 31.25`, `connections = 2` and assert the JSON keys.
Engine: call `outputRuntime(id)` twice with a fake clock? The clock is `steady_clock`; instead assert the first call yields `fps == 0` and `mbps == 0` (no interval yet), which is the zero-guard.

- [ ] **Step 2: implement**
  - On each `outputRuntime(id)`: read `rowStats`; compute `frames = delivered`, `bytes = bytesSent`; look up the previous sample. If a previous sample exists and `now > at`, `fps = (frames - prev.frames) / seconds`, `mbps = (bytes - prev.bytes) * 8.0 / seconds / 1e6`; clamp negatives to 0 (a counter reset after restart). Store the new sample. First call leaves both at 0.
  - `connections` comes from `RowStats`. `Engine::stop()` clears `rates_` so a restart starts from a clean sample.

- [ ] **Step 3: run green**, **Step 4: commit** — `git commit -m "feat(engine): per-output fps and Mbps from counter deltas"`.

---

### Task 3: `remove-output` and `set-output`

**Files:**
- Modify: `src/engine/command_router.cpp`
- Test: `tests/command_router_test.cpp`

**Interfaces:**
- `remove-output`: `{"cmd":"remove-output","id":"o1"}` → `{"ok":true}`; unknown id refuses `remove-output: no such output: o1`.
- `set-output`: `{"cmd":"set-output","id":"o1"}` with any of `sourceId`, `target`, `codec`, `quality`, `alpha`, `name`; each is validated:
  - `sourceId`: text; empty clears; otherwise must exist.
  - `target`: `OMT` | `OMT-HX` | `NDI` via `outputTargetFromString`; unknown refuses with the value.
  - `codec`: `HEVC` | `H.264`; `quality`: `Low` | `Medium` | `High`; `alpha`: `Auto` | `Off` — all via the existing `*FromString`.
  - `name`: non-empty text.
  - Unknown output id refuses; nothing is minted.

- [ ] **Step 1: failing tests** — at least: edit name; move to another source; reject unknown target by name; reject unknown id; remove then state has one fewer output; `remove-output` twice refuses the second time.

- [ ] **Step 2: implement** in `command_router.cpp` next to `set-output-source`, reusing the `*FromString` helpers and the existing `refuse` style. `remove-output` erases by id.

- [ ] **Step 3: run green**, **Step 4: commit** — `git commit -m "feat(engine): remove-output and set-output commands"`.

---

### Task 4: `set-source`

**Files:**
- Modify: `src/engine/command_router.cpp`
- Test: `tests/command_router_test.cpp`, `tests/engine_test.cpp`

**Interfaces:**
- `{"cmd":"set-source","id":"s1"}` with any of `kind` (`OMT`|`OMT-HX`|`NDI`), `address` (non-empty text), `codec` (`HEVC`|`H.264` or null/absent to clear).
- Unknown id refuses; kind/address validated; `codec` is stored in `Source::codec` (only meaningful for OMT-HX, but accepted for any kind per the model).

- [ ] **Step 1: failing tests** — change address and observe `state`; change OMT→NDI; unknown kind refuses; unknown id refuses.

- [ ] **Step 2: implement**; the engine's existing `applyLocked` already stops and restarts an edited source (M3), so no engine change is needed. Keep the router test only — the restart path is already covered by `engine_test.cpp`.

- [ ] **Step 3: run green**, **Step 4: commit** — `git commit -m "feat(engine): set-source command"`.

---

### Task 5: `set-settings` and settings in `state`

**Files:**
- Modify: `src/engine/command_router.cpp`
- Test: `tests/command_router_test.cpp`

**Interfaces:**
- `{"cmd":"set-settings"}` with any of `defaultTarget` (target wire name), `defaultQuality`, `thumbnailIntervalMs` (int 0..60000), `startEngineWithApp` (bool).
- `state` gains `{"settings":{"defaultTarget":"OMT-HX","defaultQuality":"Medium","thumbnailIntervalMs":2000,"startEngineWithApp":false}}`.

- [ ] **Step 1: failing tests** — set each field, read them back from `state`; reject a bad target and a negative interval.
- [ ] **Step 2: implement**; settings live in `bridge_.settings`.
- [ ] **Step 3: run green**, **Step 4: commit** — `git commit -m "feat(engine): settings command and settings in state"`.

---

### Task 6: Discovery

**Files:**
- Modify: `src/engine/command_router.h`, `.cpp`, `src/engine/engine.h`, `.cpp`
- Test: `tests/command_router_test.cpp`
- CMake: none (NDI already linked conditionally)

**Interfaces:**
```cpp
// command_router.h
struct Discovery {
  std::vector<std::string> omt;
  std::vector<std::string> ndi;
};
using DiscoveryFn = std::function<Discovery()>;
void setDiscovery(DiscoveryFn fn);
```
- `{"cmd":"discover"}` → `{"ok":true,"omt":["PC (Cam)"],"ndi":["MACHINE (Camera)"]}`. With no provider injected it returns empty arrays, never an error.
- Engine installs a provider: `omthx::discover()` for OMT (`#include <omthx/omthx.h>` in `engine.cpp`); when `OB_WITH_NDI`, a find instance with a **1000 ms** wait, names copied, destroyed; otherwise empty.

- [ ] **Step 1: failing tests** — inject a provider returning one OMT and one NDI name; assert both arrays. With no provider, assert empty arrays and `ok:true`.
- [ ] **Step 2: implement**.
- [ ] **Step 3: run green**, **Step 4: commit** — `git commit -m "feat(engine): discovery command for OMT and NDI sources"`.

---

## Self-Review

**Spec coverage.** §13.1 sources/outputs editing — Tasks 3/4. §13.1 live fps/Mbps — Task 2. §13.1 add-source discovery — Task 6. §13.2 settings — Task 5. §13.4 validation/refusals — every command task. §6.6 file ownership stays with the UI (no engine save command).

**Known gaps, stated.** Discovery is synchronous under `controlMu_` for at most the NDI wait; the UI must show progress. Byte counters are unavailable for NDI (public API), so NDI Mbps reads 0 until NDI exposes them or the bridge counts payload bytes; Task 2's field is present either way.

**Type consistency.** `SinkStats.bytesSent` → `RowStats.bytesSent` → `OutputRuntime.mbps`; names match across the three layers. `Discovery` carries plain names, not addresses, because that is what both SDKs list.
