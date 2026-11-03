# OMT-Bridge Phase 1: Coded Lane and Pass-Through — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An OMT source feeding an OMT output of the same family (OMT→OMT, HX→HX) passes its coded frames through with no decode and no encode; the source decodes only while some output or the UI thumbnail needs a picture.

**Architecture:** OMT-HX gains a public `FrameDecoder` that decodes a `CodedFrame` the way `Receiver` would. In OMT-Bridge, `Frame` carries an optional shared `omthx::CodedFrame` beside its UYVY picture. `OmtSource` (with outputs) opens one `Coded` receiver, asks the graph per frame whether a picture is wanted, and decodes only then. `OmtSink` sends the coded frame with `sendCoded` when its output passes through, and hands keyframe requests back to the source through `BridgeGraph::publish`'s return value.

**Tech Stack:** C++20, OMT-HX (submodule `third_party/OMT-HX`), libvmx, doctest, CMake + vcpkg.

**Spec:** `OMT-Bridge/docs/superpowers/specs/2026-11-03-omt-bridge-data-path-design.md` (§3.1–3.3, §3.7, phase 1)

**Runs after:** the encode-on-demand plan (`BreadCamera/docs/superpowers/plans/2026-11-03-encode-on-demand.md`) is merged in OMT-HX and OMT-Bridge.

## Global Constraints

- Plain OMT stays standard on the wire; libomt and `omt/libomtnet` are not changed.
- A pass-through output keeps the source's codec; the output's codec setting applies only when it re-encodes (spec §3.3).
- Only sources with at least one output use the coded receiver. A thumbnail-only source keeps today's preview receiver (`PixelFormat::Uyvy`, `preview = true`).
- NDI sources and sinks are unchanged in behaviour; they never see a frame without a picture.
- OMT-Bridge builds OMT-HX from its submodule; move the pin only by fetching the remote `sibling` (`F:\Coding\BreadMedia\OMT-HX`). Never push.
- OMT-HX holds the owner's uncommitted `omt/libomtnet`, `dist/`, `scripts/link-check.ps1`: never stage or discard them.

## Review Focus

1. **A pass-through output whose receiver joins mid-GOP** must get a picture within about one GOP, sooner when the source honours keyframe requests. Test in Task 4 (the end-to-end test connects its receiver after the stream is running).
2. **The picture lane turning on mid-stream** (an NDI output or the UI added to a pass-through source) must not hand out a broken picture: the decoder restarts at a keyframe and the source asks for one. Test in Task 3.
3. **Alpha:** an output with `AlphaMode::Off` on a source with alpha must re-encode (it cannot drop alpha from coded frames). Test in Task 2 (`passesThrough`).
4. **Coded-only frames reaching picture consumers** (the NDI sink, thumbnails, the opaque-alpha gate) must be ignored, not indexed. Test in Task 2 (hub) and Task 3 (thumbnail guard).
5. **A source switching between HX and plain OMT** while running: outputs re-decide per frame from the frame's own mode. Test in Task 2 (`pictureWanted` per family).

---

### Task 1: OMT-HX `FrameDecoder`

**Files:**
- Modify: `OMT-HX/include/omthx/omthx.h`, `OMT-HX/src/link.cpp`
- Test: `OMT-HX/tests/link_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // Decodes coded frames from a Coded receiver into pictures, as a Receiver of `pixels` would hand them back.
  // One thread at a time. Feed frames in order; after a gap, call reset().
  class OMTHX_API FrameDecoder {
   public:
    explicit FrameDecoder(PixelFormat pixels = PixelFormat::Uyvy);  // Uyvy only for now; others throw invalid_argument
    ~FrameDecoder();
    // False: no picture yet (waiting for a keyframe, the decoder filling its pipeline, or a bad frame).
    bool decode(const CodedFrame& frame, VideoFrame& out);
    void reset();  // forget the stream: the next picture comes from a keyframe
    std::string name() const;  // "vmx", "hevc (d3d11va)", ... ; empty before the first frame
   private:
    struct Impl;
    std::unique_ptr<Impl> d_;
  };
  ```
- HX frames: the same decode and alpha handling as `receiveVideo`'s Hx path (`alphaBytes` split, `alphaRepeats`, pending alpha by timestamp, stacked alpha by `codedHeight`, `unpackUyvy` with its checks). Factor that path out of `Receiver::Impl` into one internal helper that both use; there must not be a second copy of the alpha and pending-alpha logic. Waiting for a keyframe uses `frame.keyframe`.
- VMX frames: libvmx directly, as libomt does (`VMX_Create(size, VMX_PROFILE_OMT_SQ, VMX_COLORSPACE_BT709)`, recreated on a size change; `VMX_LoadFrom`; `VMX_DecodeUYVY`, or `VMX_DecodeUYVA` with stride `width * 2` when `format.alpha`, which writes the UYVA layout `VideoFrame::uyvy` documents).

- [ ] **Step 1: Write the failing tests**
  - `"FrameDecoder decodes an HX source's coded frames like a Uyvy receiver"` (hardware HEVC): an HX `Pump` at 640×360 with alpha; a `Coded` receiver and a `Uyvy` receiver on it. Decode 30 coded frames → at least 25 pictures; each `out.format == f`, `out.uyvy.size() == 640u * 360 * 3`. The centre pixel's Y is within ±4 of the Uyvy receiver's at the same timestamp, when one matches.
  - `"FrameDecoder decodes plain OMT VMX with alpha"` (no hardware): a `Mode::Full` `Pump` with alpha; `Coded` receiver → `decode` true on the first frame, `out.uyvy.size() == w * h * 3`, alpha plane not all zero, `name() == "vmx"`.
  - `"FrameDecoder starts at a keyframe after reset"` (hardware HEVC): feed frames until a picture, `reset()`, then feed only non-key frames → `decode` false each time; feed the next keyframe (wait for `frame.keyframe`, calling `requestKeyframe` on the receiver) → true within 2 frames.
  - `"FrameDecoder rejects pixel formats it does not offer"`: `CHECK_THROWS_AS(FrameDecoder(PixelFormat::Gpu), std::invalid_argument)`.

- [ ] **Step 2: Run them to see them fail**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "FrameDecoder*"`. Expected: build error, `FrameDecoder` unknown.

- [ ] **Step 3: Implement**, as above. Existing receiver tests must stay green after the refactor.

- [ ] **Step 4: Run the new tests, then the full OMT-HX suite.** Expected: green.

- [ ] **Step 5: Commit on `omthx-frame-decoder`, merge into OMT-HX `main`**

```bash
git add include/omthx/omthx.h src/link.cpp tests/link_test.cpp
git commit -F - <<'EOF'
feat: FrameDecoder decodes a coded receiver's frames on demand
EOF
git checkout main
git merge --no-ff omthx-frame-decoder -m "merge: FrameDecoder"
```
Run the full suite on `main`. Leave OMT-HX on `main`.

### Task 2: OMT-Bridge core — coded frames, the pass-through rule, the picture question

Start OMT-Bridge's branch `ob-coded-pass-through`. First move the pin (one commit, before any build, because `scripts/build.ps1` runs `git submodule update`):
```bash
git -C third_party/OMT-HX fetch sibling main
git -C third_party/OMT-HX checkout <OMT-HX main after Task 1>
git add third_party/OMT-HX
git commit -F - <<'EOF'
deps: OMT-HX <short hash> with FrameDecoder
EOF
```

**Files:**
- Modify: `src/core/frame.h`, `src/core/bridge_model.h`, `src/core/bridge_model.cpp`, `src/engine/source_hub.cpp`, `src/engine/output_row.h`, `src/engine/output_row.cpp`, `src/engine/bridge_graph.h`, `src/engine/bridge_graph.cpp`
- Test: `tests/frame_test.cpp`, `tests/bridge_model_test.cpp`, `tests/source_hub_test.cpp`, `tests/bridge_graph_test.cpp`

**Interfaces:**
- Produces:
  - `frame.h`: forward-declare `namespace omthx { struct CodedFrame; }`; `Frame` gains `std::shared_ptr<const omthx::CodedFrame> coded;` (the source's coded frame, or null) and `bool codedOnly() const { return coded && uyvy.empty(); }`. `valid()` keeps meaning "has a valid picture", so the NDI sink and thumbnails already skip coded-only frames.
  - `bridge_model.h`: `bool passesThrough(const Output& o, OutputTarget sourceFamily, bool sourceAlpha);` — true when `o.target == sourceFamily`, `sourceFamily != OutputTarget::Ndi`, and not (`sourceAlpha && o.alpha == AlphaMode::Off`).
  - `SourceHub::storeFrame`: a coded-only frame skips the opaque-alpha gate and leaves its state as it is.
  - `Sink`: `virtual bool takeKeyframeRequest() { return false; }` (any thread; true once per request). `OutputRow::takeKeyframeRequest()` forwards to its sink.
  - `BridgeGraph::publish` returns `bool`: true when any row reading this source took a keyframe request during this call. `bool BridgeGraph::pictureWanted(const std::string& sourceId, OutputTarget family, bool alpha) const;` — true while thumbnails are wanted and the graph is started, or any row reading the source does not pass through (`passesThrough(row config, family, alpha)` false). Both lock `mu_`.

- [ ] **Step 1: Write the failing tests**
  - `frame_test`: a `Frame` with `coded` set and empty `uyvy` → `codedOnly()` true, `valid()` false. With a valid picture too → `codedOnly()` false, `valid()` true.
  - `bridge_model_test` `"passesThrough"`: (Omt target, Omt family) true; (OmtHx, OmtHx) true; (Omt, OmtHx) false; (OmtHx, Omt) false; (Ndi, Ndi) false; (OmtHx, OmtHx, alpha source, `AlphaMode::Off`) false; same with `Auto` true.
  - `source_hub_test`: a coded-only frame stored with `format.alpha` true keeps `format.alpha` true and `latest()` returns it.
  - `bridge_graph_test`:
    - `pictureWanted`: a source with one OmtHx row → `pictureWanted(id, OmtHx, false)` false and `pictureWanted(id, Omt, false)` true; add an Ndi row → true for both; `setThumbnailsWanted(true)` on a started graph → true.
    - `publish` returns true once after a test sink's `takeKeyframeRequest()` is armed, then false.

- [ ] **Step 2: Run them to see them fail** (`scripts/test.ps1 -Filter "<name>"`). Expected: build errors.

- [ ] **Step 3: Implement** as in Interfaces.

- [ ] **Step 4: Run the touched tests, then the full suite.** Expected: green.

- [ ] **Step 5: Commit**

```bash
git add src/core/frame.h src/core/bridge_model.h src/core/bridge_model.cpp src/engine/source_hub.cpp src/engine/output_row.h src/engine/output_row.cpp src/engine/bridge_graph.h src/engine/bridge_graph.cpp tests/frame_test.cpp tests/bridge_model_test.cpp tests/source_hub_test.cpp tests/bridge_graph_test.cpp
git commit -F - <<'EOF'
feat(core): frames carry coded data; the graph says when a picture is wanted
EOF
```

### Task 3: `OmtSource` reads coded frames and decodes on demand

**Files:**
- Modify: `src/engine/omt_source.h`, `src/engine/omt_source.cpp`, `src/engine/engine.cpp` (the `OmtSource` construction in `startAdapter`, and `maybeSendThumbnails`)
- Test: `tests/engine_test.cpp` or a new `tests/omt_source_test.cpp` (add it to `CMakeLists.txt`'s test list)

**Interfaces:**
- Consumes: `FrameDecoder` (Task 1), `BridgeGraph::publish` → `bool`, `pictureWanted` (Task 2).
- Produces:
  - `OmtSource::FrameSink` becomes `std::function<bool(std::shared_ptr<Frame>)>`; true = a downstream receiver wants a keyframe → the video loop calls `receiver.requestKeyframe()`. `NdiSource` keeps its own `FrameSink`.
  - New constructor argument `PictureWanted = std::function<bool(bool hx, bool alpha)>`, after `onAudio`. Null means always.
  - `uint64_t OmtSource::framesDecoded() const` (atomic counter).
- Behaviour, `preview == false`: the receiver is `PixelFormat::Coded`. Per coded frame: `Frame.coded` = the frame (moved into a `shared_ptr<const CodedFrame>`), `Frame.format` and `timestamp` from its header. If `pictureWanted(mode == Hx, format.alpha)`: decode with one `FrameDecoder(Uyvy)`; a picture fills `uyvy`, and `format`, `premultiplied` and `timestamp` come from the decoder's `VideoFrame` (an HX decoder hands pictures back late, so its timestamp is the picture's). When decoding turns on after being off (or at the first frame), `reset()` the decoder and `requestKeyframe()`. A frame whose decode gives no picture is still published coded-only. Liveness follows published frames.
- `preview == true`: unchanged (Uyvy preview receiver, pictures only).
- `engine.cpp`: the `OmtSource` lambda returns `graph_.publish(id, …)`; the predicate is `graph_.pictureWanted(id, hx ? OutputTarget::OmtHx : OutputTarget::Omt, alpha)`. `maybeSendThumbnails` skips a frame with no picture (`!frame->valid()` already does; keep it).

- [ ] **Step 1: Write the failing tests** (real network, plain OMT so no hardware is needed; one HX variant that returns early without hardware HEVC):
  - `"an OmtSource with only pass-through demand never decodes"`: a `Mode::Full` omthx `Sender` pumping 640×360 at 30 fps; `OmtSource` with `pictureWanted` returning false; after 1 s, frames were published, every one `codedOnly()`, `framesDecoded() == 0`.
  - `"an OmtSource decodes once a picture is wanted, from a keyframe"`: as above with a flag the test flips to true after 0.5 s → within 2 s a published frame has `valid()` true and `framesDecoded() > 0`. HX variant: the first picture after the flip comes from a stream restarted at a keyframe (decoder `name()` set, no exception, picture `valid()`).
  - `"a keyframe request from downstream reaches the source"`: HX source; `FrameSink` returns true once → the sender's `stats().keyframes` grows within 1 s.

- [ ] **Step 2: Run them to see them fail.** Expected: build errors (`framesDecoded`, new constructor).

- [ ] **Step 3: Implement** as in Interfaces.

- [ ] **Step 4: Run the touched tests, then the full suite.** Expected: green.

- [ ] **Step 5: Commit**

```bash
git add src/engine/omt_source.h src/engine/omt_source.cpp src/engine/engine.cpp tests/<the test file> CMakeLists.txt
git commit -F - <<'EOF'
feat(engine): OMT sources read coded frames and decode only on demand
EOF
```

### Task 4: `OmtSink` passes coded frames through

**Files:**
- Modify: `src/engine/omt_sink.h`, `src/engine/omt_sink.cpp`
- Test: `tests/omt_sink_test.cpp`

**Interfaces:**
- Consumes: `Frame::coded`, `passesThrough` (Task 2).
- Produces:
  - Protected seam `virtual void sendCoded(const omthx::CodedFrame& frame);` (called with `mu_` held, like `send`), overridden by `TestSink`.
  - `bool takeKeyframeRequest() override;` — an `std::atomic<bool>` set true by `openLocked()` (a new output asks at once) and after each `sendCoded` whose sender reports `keyframeRequested()`.
- `deliver`: if `frame->coded` and `passesThrough(config_, family of coded->mode, coded->format.alpha)` → `sendCoded(*frame->coded)`, `frameNote_ = "passed through"`. Else if `frame->valid()` → today's path. Else drop (no picture yet). `stats()` unchanged.

- [ ] **Step 1: Write the failing tests** (`TestSink` records `sendCoded` calls):
  - an HX coded frame to an OmtHx output → one `sendCoded`, no `send`; note `"passed through"`.
  - the same frame to an Omt output → `send` (it carries a picture too), no `sendCoded`.
  - an HX coded frame with alpha to an OmtHx output with `AlphaMode::Off` → `send`, not `sendCoded`.
  - a coded-only frame to an Omt output → neither; nothing throws.
  - `takeKeyframeRequest()` is true once after `configure` (open), then false.
  - End-to-end (hardware HEVC, returns early otherwise): an HX omthx `Sender` with a `Pump` → `OmtSource` (coded, `pictureWanted` false) → its frames into a real `OmtSink` configured OmtHx → a `Uyvy` receiver on the sink's address, connected after the stream is running, gets a 640×360 picture within 3 s; the sink sender's `stats().encoder == "none"`; `OmtSource::framesDecoded() == 0`.

- [ ] **Step 2: Run them to see them fail.**

- [ ] **Step 3: Implement** as in Interfaces.

- [ ] **Step 4: Run the touched tests, then the full suite.** Expected: green.

- [ ] **Step 5: Commit, then merge into OMT-Bridge `main`**

```bash
git add src/engine/omt_sink.h src/engine/omt_sink.cpp tests/omt_sink_test.cpp
git commit -F - <<'EOF'
feat(engine): OMT outputs of the source's own family pass coded frames through
EOF
git checkout main
git merge --no-ff ob-coded-pass-through -m "merge: coded pass-through"
```
Run the full suite on `main`.
