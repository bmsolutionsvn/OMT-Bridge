# OMT-Bridge M3 — NDI, Tally, and Audio Passthrough

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add NDI in and out on the canonical UYVY/UYVA frame, pass downstream tally upstream, and deliver audio for every transport without mixing or resampling.

**Architecture:** The M2 engine already moves one shared UYVY/UYVA frame per source through a sink-owning row. M3 adds a second, independent media slot for audio (source slot → per-output audio queue, so audio never gates video), two NDI adapters behind the existing `SourceHub`/`Sink` seams, one pure `toStraight()` conversion for NDI output, and tally read from each sink and OR-ed per source. NDI remains optional (`OB_WITH_NDI`), so every NDI source file and test is compiled only when the SDK is found.

**Tech Stack:** C++20, MSVC, CMake + vcpkg, NDI 6 SDK (`C:/Program Files/NDI/NDI 6 SDK`, or `NDI_SDK_DIR`), OMT-HX `e21dc52`, doctest.

**Spec:** `docs/superpowers/specs/2026-10-19-omt-bridge-design.md` §6.3–§10, §12.1–§12.3, §14.3, §15.1–§15.3, §18.1, §20.

## Global Constraints

- Canonical media is 8-bit UYVY, or UYVA when `format.alpha`; limited range 16–235 luma/chroma, full range 0–255 alpha.
- Audio is planar float, the source's channel count **and** sample rate; **no mixing, no resampling, no 48 kHz rule**. Audio never gates video.
- NDI `UYVA` is treated as **straight**; NDI output is straight only, so a premultiplied frame is converted with `toStraight()` at that edge.
- Tally: a source is on air when a downstream receiver has an output on air; never from the bridge's own liveness.
- Every file ends with a single LF (0x0A), no CR.
- doctest stringifies every `CHECK` operand: never assert on an enum, struct, or smart pointer.
- Build/test only via `scripts/build.ps1` / `scripts/test.ps1`.
- NDI code is guarded by `OB_WITH_NDI`; tests that need it are compiled/registered only then.
- Locks: `Engine::controlMu_` → graph `mu_` → hub `mu_` → row queue → sink `mu_`. Adapter callbacks take `runtimeMu_` (state) or nothing (media); never `controlMu_`.

## Review Focus

- **Audio at 44.1 kHz or 5.1** must pass through unchanged — BreadCamera drops both; the bridge must not inherit that. Task 2's validity test and Task 3's plumbing test pin this.
- **NDI stride 0 and negative** (`line_stride_in_bytes`): 0 means tightly packed, negative means bottom-up. The bridge must copy correctly or reject; it must never index past the buffer. Tasks 6 and 8 pin both: the loopback asserts alpha values on a split pattern.
- **`UYVA` alpha-plane stride** is not the video stride in every NDI build; a wrong guess shifts alpha one row per line. Task 6/7's loopback must assert alpha pixel values, not just a non-empty plane.
- **Tally must stay off when nothing downstream is on air**, even while the bridge is live. Task 9 pins the OR and the zero case.
- **A stalled audio consumer must not delay video** for any output. Task 3 pins independent queues and drop-oldest.

---

### Task 1: `toStraight()` — premultiplied → straight, in UYVY

Design §9.1. NDI output is the only edge that needs it.

**Files:**
- Create: `src/core/alpha_convert.h`, `src/core/alpha_convert.cpp`, `tests/alpha_convert_test.cpp`
- Modify: `CMakeLists.txt` (`ob_core`, `ob_tests`)

**Interfaces:**
- Produces: `void ob::toStraight(Frame& frame);` — in place, requires `format.alpha` and `valid()`; leaves `format.alpha` true, sets `premultiplied = false`.

- [ ] **Step 1: failing test**

```cpp
// tests/alpha_convert_test.cpp
// Canonical UYVY: each pair is [U][Y0][V][Y1]. This order is the point of the test.
#include <doctest/doctest.h>
#include "core/alpha_convert.h"

static ob::Frame make(int y, int u, int v, int a) {
  ob::Frame f;
  f.format = ob::VideoFormat{2, 2, 60, 1, true};
  f.premultiplied = true;
  f.uyvy.assign(f.lumaBytes() + f.alphaBytes(), 0);
  for (size_t row = 0; row < 2; ++row) {
    uint8_t* p = f.uyvy.data() + row * 4;
    p[0] = static_cast<uint8_t>(u);
    p[1] = static_cast<uint8_t>(y);
    p[2] = static_cast<uint8_t>(v);
    p[3] = static_cast<uint8_t>(y);
  }
  for (size_t i = 0; i < f.alphaBytes(); ++i) f.uyvy[f.lumaBytes() + i] = static_cast<uint8_t>(a);
  return f;
}

TEST_CASE("toStraight leaves opaque and empty alpha unchanged") {
  auto opaque = make(100, 120, 130, 255);
  ob::toStraight(opaque);
  CHECK(opaque.uyvy[0] == 120);
  CHECK(opaque.uyvy[1] == 100);
  CHECK_FALSE(opaque.premultiplied);

  auto black = make(200, 200, 200, 0);
  ob::toStraight(black);
  CHECK(black.uyvy[1] == 16);
  CHECK(black.uyvy[0] == 128);
}

TEST_CASE("toStraight divides by alpha and clamps") {
  auto mid = make(126, 190, 70, 128);
  ob::toStraight(mid);
  CHECK(mid.uyvy[1] >= 233);
  CHECK(mid.uyvy[1] <= 235);
  CHECK(mid.uyvy[0] == 240);

  auto low = make(20, 130, 130, 10);
  ob::toStraight(low);
  CHECK(low.uyvy[1] > 100);
  CHECK(low.uyvy[1] < 140);
}
```

The committed file also carries a 4x2 mixed-alpha case (per-pair means and a zero pair)
and a guard case (no alpha / invalid frame). Both are required: a single-alpha 2x2 cannot
see a luma/chroma plane swap, which is exactly the bug this task shipped once.

- [ ] **Step 2: run and see it fail** — `.\scripts\test.ps1`; expected: `core/alpha_convert.h` missing.

- [ ] **Step 3: header**

```cpp
#pragma once
#include "core/frame.h"

namespace ob {

// Premultiplied -> straight, in place, for a frame that already has alpha (design 9.1).
// Y = 16 + (Y' - 16) / a, C = 128 + (C' - 128) / a, clamped to 16..235 / 16..240;
// a == 0 becomes colour over black. Chroma uses the mean alpha of its two pixels.
void toStraight(Frame& frame);

}  // namespace ob
```

- [ ] **Step 4: implement** — operate on the UYVY pairs, `alpha = uyvy[lumaBytes() + pixel] / 255.0f`; U/V use the mean of the pair's alphas. Guard `!format.alpha || !valid()` by returning unchanged. Set `premultiplied = false` at the end.

- [ ] **Step 5: CMake, run, commit**

Add `src/core/alpha_convert.cpp` to `ob_core`, `tests/alpha_convert_test.cpp` to `ob_tests`.

```bash
git add src/core/alpha_convert.h src/core/alpha_convert.cpp tests/alpha_convert_test.cpp CMakeLists.txt
git commit -m "feat(core): toStraight for premultiplied UYVA frames"
```

---

### Task 2: `AudioFrame` — the canonical audio packet

**Files:**
- Create: `src/core/audio.h`, `tests/audio_test.cpp`
- Modify: `CMakeLists.txt` (`ob_tests`)

**Interfaces:**
- Produces:
```cpp
namespace ob {
struct AudioFormat {
  int sampleRate = 0;
  int channels = 0;
  bool operator==(const AudioFormat&) const = default;
};
struct AudioFrame {
  AudioFormat format;
  int samples = 0;           // per channel
  int64_t timestamp = 0;     // OMT units: 10,000,000 per second (NDI ticks are the same unit)
  std::vector<float> planar; // channel 0's samples, then channel 1's, ...
  bool valid() const;        // rate > 0, channels > 0, samples > 0, planar.size() == samples*channels
};
}
```

- [ ] **Step 1: failing test** — valid for 48 kHz stereo, 44.1 kHz 6-channel, and mono; invalid for zero rate/channels/samples and a short plane.

```cpp
TEST_CASE("audio validity keeps any rate and channel count") {
  ob::AudioFrame f;
  f.format = {44100, 6};
  f.samples = 4;
  f.planar.assign(24, 0.0f);
  CHECK(f.valid());

  f.format = {48000, 2};
  f.planar.assign(8, 0.0f);
  CHECK(f.valid());

  f.format = {0, 2};
  CHECK_FALSE(f.valid());
  f.format = {48000, 2};
  f.samples = 5;
  CHECK_FALSE(f.valid());
}
```

- [ ] **Step 2: run and fail**, **Step 3: implement the header only**, **Step 4: run green** (89 → 90 cases).

- [ ] **Step 5: commit** — `git commit -m "feat(core): canonical planar-float AudioFrame"`.

---

### Task 3: Audio plumbing — hub slot, per-row queue, sink hook

Audio is independent of video: a source stores the newest packet in a slot; each row has its own small audio queue and worker, so a slow audio sink cannot delay video.

**Files:**
- Modify: `src/engine/source_hub.h`, `.cpp`, `src/engine/output_row.h`, `.cpp`, `src/engine/bridge_graph.h`, `.cpp`
- Test: `tests/source_hub_test.cpp`, `tests/output_row_test.cpp`, `tests/bridge_graph_test.cpp`

**Interfaces:**
- `SourceHub`: `void storeAudio(std::shared_ptr<AudioFrame>); std::shared_ptr<const AudioFrame> latestAudio() const; uint64_t audioFramesPublished() const;`
- `Sink`: `virtual void deliverAudio(const std::shared_ptr<const AudioFrame>&) {}`
- `OutputRow`: `void offerAudio(std::shared_ptr<const AudioFrame>); uint64_t audioDelivered() const;`
- `BridgeGraph`: `void publishAudio(const std::string& sourceId, std::shared_ptr<AudioFrame> frame);`
- `RowStats`: add `uint64_t audioDelivered = 0;`

- [ ] **Step 1: tests**

```cpp
// output_row_test.cpp — a recording sink gains audio
class AudioSink : public ob::Sink {
 public:
  void deliver(const std::shared_ptr<const ob::Frame>&) override {}
  void deliverAudio(const std::shared_ptr<const ob::AudioFrame>& a) override { ++count; }
  std::atomic<int> count{0};
};

TEST_CASE("a row delivers audio on its own path, in order") {
  auto sink = std::make_unique<AudioSink>();
  auto* raw = sink.get();
  ob::OutputRow row("o1", std::move(sink));
  row.start();
  for (int i = 0; i < 3; ++i) {
    auto a = std::make_shared<ob::AudioFrame>();
    a->format = {48000, 2};
    a->samples = 1;
    a->planar.assign(2, 0.0f);
    a->timestamp = i;
    row.offerAudio(a);
  }
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (raw->count.load() < 3 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();
  CHECK(raw->count.load() == 3);
}
```

Add a graph case: `publishAudio` offers to every row of the source, and `rowStats(...).audioDelivered` increments. Add a hub case: `storeAudio` keeps the newest and counts.

- [ ] **Step 2: run and fail**, **Step 3: implement**:
  - `SourceHub`: a second slot `latestAudio_`, `audioPublished_`, same single-writer rule as `storeFrame`; release the previous packet outside `mu_`.
  - `OutputRow`: `BoundedQueue<std::shared_ptr<const AudioFrame>> audioQueue_`, `std::thread audioThread_`, `std::atomic<uint64_t> audioDelivered_`. `start()` reopens and spawns both; `stop()` closes and joins both. `audioRun()` pops with the same 100 ms timeout, calls `deliverAudio`, and **swallows exceptions** (audio failure must not fault the video row).
  - `BridgeGraph::publishAudio` mirrors `publish`: store a copy in the hub, then `offerAudio` to each matching row under `mu_`.
- [ ] **Step 4: run green; every existing case must still pass.**

- [ ] **Step 5: commit** — `git commit -m "feat(engine): independent audio slot, row queue and sink hook"`.

---

### Task 4: OMT audio in and out

M2 drained OMT audio; now it is delivered.

**Files:**
- Modify: `src/engine/omt_source.h`, `.cpp`, `src/engine/omt_sink.h`, `.cpp`

**Interfaces:**
- `OmtSource` ctor gains `using AudioSink = std::function<void(std::shared_ptr<AudioFrame>)>;` as the fourth parameter.
- `OmtSink` implements `void deliverAudio(const std::shared_ptr<const AudioFrame>&) override;`

- [ ] **Step 1: no new unit test** (needs a peer); the M3 loopback proves it.

- [ ] **Step 2: implement**
  - `audioLoop`: on `receiveAudio`, build `AudioFrame{ {af.sampleRate, af.channels}, af.samples, af.timestamp, std::move(af.planar) }`, call `onAudio_` when non-empty. Keep the catch-and-ignore. Do not validate the rate.
  - `OmtSink::deliverAudio`: under `mu_`, if the sender exists, call `sendAudio(std::span<const float>(frame->planar.data(), frame->planar.size()), frame->format.sampleRate, frame->format.channels, frame->timestamp)`.
  - Include `core/audio.h` and `<span>` where needed.

- [ ] **Step 3: build and run** — `.\scripts\test.ps1` (85+ cases, unchanged count).

- [ ] **Step 4: commit** — `git commit -m "feat(engine): deliver OMT audio instead of draining it"`.

---

### Task 5: NDI build wiring and runtime guard

**Files:**
- Create: `src/engine/ndi_runtime.h`, `.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `bool ob::ensureNdi();` — calls `NDIlib_initialize()` once (static), returns success; `NDI::NDI` and `OB_WITH_NDI=1` are available to `ob_engine` and `ob_tests` when the SDK is found.

- [ ] **Step 1: CMake**

Inside `if(OB_WITH_NDI)`:

```cmake
target_sources(ob_engine PRIVATE src/engine/ndi_runtime.cpp)
target_link_libraries(ob_engine PUBLIC NDI::NDI)
target_compile_definitions(ob_engine PUBLIC OB_WITH_NDI=1)
```

Tasks 6 and 7 append `src/engine/ndi_source.cpp` and `src/engine/ndi_sink.cpp` to the same
`target_sources` list; do not list a file before it exists, or configure fails.

- [ ] **Step 2: `ensureNdi()`**

```cpp
// ndi_runtime.cpp
#include "engine/ndi_runtime.h"
#include <Processing.NDI.Lib.h>

namespace ob {
bool ensureNdi() {
  static const bool ok = NDIlib_initialize();
  return ok;
}
}
```

- [ ] **Step 3: guarded test**

```cpp
#include "engine/ndi_runtime.h"
TEST_CASE("the NDI runtime initializes when built with NDI") {
  CHECK(ob::ensureNdi());
}
```

Treat it as a normal test but wrap registration in `#ifdef OB_WITH_NDI` so a build without the SDK skips it.

- [ ] **Step 4: build and test**, **Step 5: commit** — `git commit -m "feat(engine): NDI build wiring and one-time runtime init"`.

---

### Task 6: `NdiSource` — NDI in

**Files:**
- Create: `src/engine/ndi_source.h`, `.cpp`
- Modify: `CMakeLists.txt` (already names the source in Task 5)

**Interfaces:**
- Consumes: `AudioFrame`, `SignalState`, `ErrorInfo`, `ensureNdi()`.
- Produces:

```cpp
class NdiSource {
 public:
  using FrameSink = std::function<void(std::shared_ptr<Frame>)>;
  using AudioSink = std::function<void(std::shared_ptr<AudioFrame>)>;
  using StateSink = std::function<void(SignalState, std::optional<ErrorInfo>)>;
  using TallySink = std::function<void(bool program, bool preview)>;

  NdiSource(std::string name, FrameSink onFrame, AudioSink onAudio, StateSink onState);
  ~NdiSource();
  void start();  // idempotent
  void stop();   // idempotent
  bool running() const;
  void setTally(bool program, bool preview);  // NDIlib_recv_set_tally
};
```

- [ ] **Step 1: no unit test here** — Task 8's loopback is the device-free proof.

- [ ] **Step 2: implement** (adapt `BreadCamera/src/io/ndi_input.cpp`):
  - `NDIlib_find_create_v2` with `show_local_sources = true`; match the exact `p_ndi_name`, then a substring match; wait 200 ms per attempt.
  - `NDIlib_recv_create_v3_t`: `color_format = NDIlib_recv_color_format_fastest`, `bandwidth = NDIlib_recv_bandwidth_highest`, `p_ndi_recv_name = "OMT Bridge"`.
  - `NDIlib_recv_capture_v3(recv, &v, &a, nullptr, 200)`.
  - **Video**: accept `NDIlib_FourCC_type_UYVY` and `NDIlib_FourCC_type_UYVA` only; reject others as `ndi-format` once. Copy rows into a tightly packed `Frame::uyvy`: video `width*2` bytes per row from `p_data + row*line_stride` (`line_stride == 0` means `width*2`; **negative stride is bottom-up** — flip rows rather than reject). For `UYVA`, the alpha plane follows the video plane; verify its row stride from the header/docs and copy `width` bytes per row (the loopback in Task 8 must assert alpha values).
  - `format = {xres, yres, frame_rate_N, frame_rate_D, alpha}`; `premultiplied = false` (NDI is straight); `timestamp = v.timestamp == NDIlib_recv_timestamp_undefined ? 0 : v.timestamp` (100 ns units are OMT units).
  - **Audio**: `NDIlib_frame_type_audio` → `AudioFrame{{a.sample_rate, a.no_channels}, a.no_samples, a.timestamp, planar}`; copy each channel with `channel_stride_in_bytes`; no 48 kHz filter.
  - Liveness: first frame → `Live`; no video for 2 s → `Offline`; destroy on stop and report `Ready` (same conventions as `OmtSource`, including the lifecycle mutex and `stopImpl(notify)` pattern).
  - `setTally` stores the values atomically and applies them to the live receiver under the same lifecycle serialisation.

- [ ] **Step 3: build**, **Step 4: commit** — `git commit -m "feat(engine): NdiSource, UYVY/UYVA video and planar audio"`.

---

### Task 7: `NdiSink` — NDI out

**Files:**
- Create: `src/engine/ndi_sink.h`, `.cpp`
- Modify: `CMakeLists.txt` (named in Task 5)

**Interfaces:**
- Produces: `class NdiSink : public Sink` with `configure`, `deliver`, `deliverAudio`, `note`, `tally()`.
- Consumes: `toStraight()`, `ensureNdi()`.

- [ ] **Step 1: failing unit test for the conversion decision**

Use a protected virtual seam like `OmtSink`, so the test sends no network traffic:

```cpp
class TestNdiSink : public ob::NdiSink {
 public:
  struct Sent { int w, h; bool alpha, premultiplied; };
  std::vector<Sent> sent;
 protected:
  void openLocked() override {}
  void send(const ob::Frame& f, bool premultiplied) override {
    sent.push_back({f.format.width, f.format.height, f.format.alpha, premultiplied});
  }
};

TEST_CASE("NDI output sends premultiplied frames as straight") {
  TestNdiSink sink;
  ob::Output o;
  o.id = "o1";
  o.target = ob::OutputTarget::Ndi;
  sink.configure(o, "Bridge (A)");
  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{4, 2, 60, 1, true};
  f->premultiplied = true;
  f->uyvy.assign(f->lumaBytes() + f->alphaBytes(), 0x80);
  sink.deliver(f);
  REQUIRE(sink.sent.size() == 1u);
  CHECK_FALSE(sink.sent[0].premultiplied);
  CHECK(sink.sent[0].alpha);
}
```

Add: a straight frame stays straight and no copy is required; `alpha: Off` sends no alpha; the sink's note is empty for NDI (unlike M2's placeholder).

- [ ] **Step 2: run and fail**, **Step 3: implement**
  - `configure` / `openLocked`: `NDIlib_send_create` with `p_ndi_name = advertisedName`, `clock_video = false`, `clock_audio = false`; note `"cannot open NDI output"` and keep the sender null on failure (no throw, matching `OmtSink`).
  - `deliver`: reject `!valid()`; if `alpha && premultiplied`, copy `uyvy` to `scratch_`, `toStraight(scratchFrame)`, send the scratch with `premultiplied = false`; otherwise send the frame directly. FourCC `UYVA` when alpha, else `UYVY`; `line_stride_in_bytes = width*2`; `frame_rate_N/D`; progressive; `timecode = NDIlib_send_timecode_synthesize`; `timestamp = frame->timestamp`.
  - `deliverAudio`: `NDIlib_audio_frame_v3_t` with `FourCC_audio_type_FLTP`, `sample_rate`, `no_channels`, `no_samples`, `channel_stride_in_bytes = samples*sizeof(float)`, `p_data = planar.data()`.
  - `tally()`: `NDIlib_send_get_tally(send, &t, 0)` → `Tally{t.program, t.preview}`.
  - Document that the UYVA alpha-plane stride must be verified against the SDK and pinned by Task 8's loopback.

- [ ] **Step 4: run green**, **Step 5: commit** — `git commit -m "feat(engine): NdiSink, straight UYVA and planar audio"`.

---

### Task 8: NDI loopback and the premultiplied-to-NDI colour proof

**Files:**
- Test: `tests/ndi_loopback_test.cpp` (registered only when `OB_WITH_NDI`)
- Modify: `CMakeLists.txt`

- [ ] **Step 1: test**

```cpp
#ifdef OB_WITH_NDI
TEST_CASE("an NDI sink and source round-trip video, alpha and audio") {
  // One process, local source: NdiSink advertises, NdiSource finds it.
  // 64x36 keeps the test quick.
  // 1. Configure an NdiSink named "<Bridge> (<Out>)".
  // 2. Start an NdiSource for "<MACHINE> (<Bridge> (<Out>))" with a 2 s connect budget.
  // 3. Publish a UYVA frame with a known alpha pattern (left half 255, right half 0)
  //    and 44.1 kHz 2-channel audio.
  // 4. REQUIRE a video frame and an audio packet arrive within 5 s.
  // 5. CHECK alpha pixels on both halves (plane stride bug detector) and audio rate/channels.
}
#endif
```

Use `std::promise`/`std::future` or atomics with deadlines; never sleep forever. Skip gracefully (doctest `MESSAGE` + return) if `ensureNdi()` is false, so a machine without the runtime does not fail CI.

- [ ] **Step 2: premultiplied colour proof** — send a 2-pixel frame with `premultiplied = true`, alpha 128, colour `Y=126, U=190, V=70`; receive it and assert the straight value is ~235/240/16 (the same values as `toStraight`'s unit test, end to end).

- [ ] **Step 3: run** — `.\scripts\test.ps1`; expect two new cases and no hangs. If the NDI runtime cannot discover a local source in CI, guard the test on `OB_WITH_NDI` only (the SDK is absent in CI anyway).

- [ ] **Step 4: commit** — `git commit -m "test(engine): NDI loopback for video, alpha and audio"`.

---

### Task 9: Tally — read downstream, OR per source, push upstream

**Files:**
- Modify: `src/engine/output_row.h`, `src/engine/bridge_graph.h`, `.cpp`, `src/engine/source_hub.h`, `.cpp`, `src/engine/omt_source.h`, `.cpp`, `src/engine/ndi_source.h`, `.cpp`
- Test: `tests/bridge_graph_test.cpp`

**Interfaces:**
- `struct Tally { bool program = false; bool preview = false; };` in `output_row.h`.
- `Sink`: `virtual Tally tally() const { return {}; }`
- `SourceHub`: `void setDownstreamTally(Tally t); Tally downstreamTally() const;`
- `BridgeGraph`: `std::map<std::string, Tally> pollTallies();` — locks `mu_`, reads each row's `sink_->tally()`, ORs per `sourceId`, stores it on the hub, returns the map.
- `OmtSource`/`NdiSource`: `void setTally(bool program, bool preview);`

- [ ] **Step 1: test**

```cpp
class TallySink : public ob::Sink {
 public:
  ob::Tally t;
  void deliver(const std::shared_ptr<const ob::Frame>&) override {}
  ob::Tally tally() const override { return t; }
};

TEST_CASE("tally is OR-ed across a source's outputs and nothing lights when all are off") {
  // Two outputs on s1, both TallySink; one program -> hub program.
  // Clear both -> nothing upstream.
}
```

Pin: program OR, preview OR, and a live output with no downstream tally leaves the hub off.

- [ ] **Step 2: implement** — `pollTallies` runs from `Engine` when the UI asks for `state` (the design's "polled with the stats"), not on a timer. Engine applies each map entry to the adapter (`setTally`) and leaves the hub's copy for the state reply.

- [ ] **Step 3: run green**, **Step 4: commit** — `git commit -m "feat(engine): downstream tally OR-ed per source and pushed upstream"`.

---

### Task 10: Engine wiring for NDI and audio

**Files:**
- Create: `src/engine/source_adapter.h`
- Modify: `src/engine/engine.h`, `.cpp`
- Test: `tests/engine_test.cpp`

**Interfaces:**
- Consumes: `NdiSource`, `NdiSink`, `OmtSource` audio sink, `BridgeGraph::publishAudio`, `pollTallies`.

- [ ] **Step 1: test (no devices)**

```cpp
TEST_CASE("an NDI source is marked Offline and creates no adapter") {
  // NDI source + NDI output; apply; start; sourceRuntime("s1")->state == Offline;
  // no adapter start beyond the NDI branch (adapterStarts() unchanged after the branch).
}
```

- [ ] **Step 2: implement**
  - Sink factory: `OutputTarget::Ndi` → `NdiSink`; others → `OmtSink`.
  - `startAdapter`: NDI → `NdiSource(address, frameSink, audioSink, stateSink)`; OMT → `OmtSource(..., audioSink)`. Store adapters behind a small variant or two maps; prefer a `std::unique_ptr<SourceAdapter>` interface with `start/stop/setTally` implemented by both, in `src/engine/source_adapter.h`.
  - `frameSink` → `graph_.publish`; `audioSink` → `graph_.publishAudio`.
  - `sourceRuntime(id)` calls `pollTallies()` once (or the router calls it per state reply) and pushes `setTally`.
  - Notes: NDI unavailable (`!OB_WITH_NDI`) → `Offline` with note `"NDI support was not built"`.

- [ ] **Step 3: run**, **Step 4: commit** — `git commit -m "feat(engine): NDI adapters, audio wiring and tally polling"`.

---

### Task 11: M3 loopback script and the manual checks

**Files:**
- Create: `scripts/m3-ndi-e2e.ps1`
- Modify: `docs/superpowers/specs/2026-10-19-omt-bridge-design.md` §16 (measurements) and §20 (only if a done-criterion changes)

- [ ] **Step 1: script** — an NDI self-loop using the engine alone:
  - bridge JSON: `source` kind `NDI` named `"M3 In"`, `output` target `NDI` named `"M3 Out"`;
  - start the engine, then use a small NDI helper (or `omthx` tools cannot do NDI — use the NDI SDK's own `NDIlib` sample if installed) to send a `UYVA` 1080p60 pattern and receive `"MACHINE (M3 Bridge (M3 Out))"`;
  - assert: video received, alpha values preserved on a split pattern, audio at 44.1 kHz stereo passes (the non-48 kHz case);
  - fail loudly on missing input; clean up in `finally`; print `M3 NDI E2E OK`.
  - If no NDI send/receive helper is available on the machine, record the exact blocker and leave the script with a clear `throw` message rather than a fake pass.

- [ ] **Step 2: run it**, **Step 3: manual checks** — NDI Studio Monitor alpha smoke test (§15.3/§18.1) and the tally test (an OMT output to program lights the camera); record the results and date in §16.

- [ ] **Step 4: commit** — `git commit -m "test(m3): NDI self-loop, alpha and audio proof"`.

---

## Self-Review

**Spec coverage.** §8 NDI adapters — Tasks 6/7. §9.1 `toStraight` — Task 1. §10 audio — Tasks 2/3/4/6/7/10. §12.3 tally — Task 9. §15.1 unit tests — Tasks 1/2/3/9. §15.2 NDI self-loop and premultiplied-to-NDI — Task 8/11. §15.3 NDI alpha smoke and tally — Task 11 (manual). §20 M3 — Tasks 6/7/8/9/11.

**Known gaps, stated.** NDI code is optional; on a machine without the SDK the adapters are absent and the loopback test is compiled out. The `UYVA` alpha-plane stride is the one fact the NDI headers do not state consistently; Task 8's loopback with a split alpha pattern is the gate for it, and Task 7 must not guess silently.

**Type consistency.** `AudioFrame` is used identically by hub, row, OMT and NDI; `Tally` is the one tally type; `Sink::deliverAudio` takes `shared_ptr<const AudioFrame>` everywhere; `publishAudio` takes `shared_ptr<AudioFrame>` (the adapter owns the mutable packet) and stores a const copy in the hub.
