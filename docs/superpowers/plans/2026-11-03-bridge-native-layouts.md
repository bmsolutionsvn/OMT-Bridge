# OMT-Bridge Phase 2: Native Pixel Layouts, One Conversion per Frame — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A decoded HX picture stays NV12 (no CPU unpack to UYVY); each sink takes it in the layout it accepts; the one conversion still needed (full-range NV12 → limited NV12 for NDI and plain OMT) runs at most once per frame, shared by every sink.

**Architecture:** `Frame` gains `Layout::Nv12` (Y then interleaved UV, full-range BT.709, no alpha, as OMT-HX decodes HX). `OmtSource` decodes an HX frame without alpha with `FrameDecoder(Nv12)` and copies the planes once into the frame. The frame owns a lazily made, thread-safe `limitedNv12()` that every sink needing limited range shares. Sinks: OMT-HX → `sendVideoNv12(full range)`; plain OMT → `sendVideoNv12(limited)`; NDI → NV12 FourCC from `limitedNv12()`.

**Tech Stack:** C++20, OMT-HX submodule, NDI SDK (optional `OB_WITH_NDI`), doctest.

**Spec:** `OMT-Bridge/docs/superpowers/specs/2026-11-03-omt-bridge-data-path-design.md` (§3.4, phase 2)

**Runs after:** phase 1, `OMT-Bridge/docs/superpowers/plans/2026-11-03-bridge-coded-pass-through.md`, merged.

## Global Constraints

- NV12 in a `Frame` is full range (BT.709), as OMT-HX's decoder delivers it. UYVY stays limited range.
- Range conversion (exact, by table): luma `16 + round(y * 219 / 255)`, chroma `128 + round((c - 128) * 224 / 255)`. Endpoints: Y 0→16, 255→235; C 0→16, 128→128, 255→240.
- A source frame with alpha is decoded to UYVY (alpha kept); NV12 carries no alpha.
- NDI is limited range; plain OMT's `sendVideoNv12` takes limited range only.
- Pass-through (phase 1) is unchanged: these rules apply only to the picture lane.
- Never push. OMT-HX owner files stay untouched. Move the OMT-HX pin only from the remote `sibling`.

## Review Focus

1. **Two sinks needing limited range on one frame** (an NDI and a plain-OMT output of one HX source): one conversion, shared. Test in Task 2.
2. **A source that turns alpha on or off mid-stream:** the source switches decoder layout and restarts at a keyframe; no picture with the wrong layout or a stale plane. Test in Task 3.
3. **Levels:** an HX source's mid-grey and black must arrive at NDI and plain-OMT receivers at the same levels as today (via UYVY). Test in Task 4.
4. **Thumbnails of an NV12 source** must look like today's. Test in Task 2.
5. **The NDI sink without `OB_WITH_NDI`** must still build. Covered by building the `off` preset if present (`build/off`), else noted in the ledger.

---

### Task 1: OMT-HX `FrameDecoder(PixelFormat::Nv12)`

**Files:** Modify `OMT-HX/include/omthx/omthx.h`, `OMT-HX/src/link.cpp`; test `OMT-HX/tests/link_test.cpp`.

**Interfaces:**
- Produces: `FrameDecoder` accepts `PixelFormat::Nv12`, with the same meaning as an `Nv12` receiver: `out.nv12Y`/`out.nv12UV` point at full-range planes valid until the next `decode`, matching `out.format` (the existing `checkNv12Picture`); alpha dropped. VMX frames: decode UYVY with libvmx, then `packUyvy` to full-range NV12, as the receiver's plain-OMT path does.

- [ ] **Step 1: Failing tests:** `"FrameDecoder(Nv12) hands back the decoder's planes"` (hardware HEVC): HX source 640×360 without alpha → `nv12Y`, `nv12UV` set, `format == f`; centre Y within ±4 of an `Nv12` receiver's. `"FrameDecoder(Nv12) packs a VMX frame"` (no hardware): plain OMT source → planes set, `format.alpha == false`.
- [ ] **Step 2: Run** `scripts/test.ps1 -Filter "FrameDecoder(Nv12)*"` → fail (constructor throws `invalid_argument`).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the new tests, then the full suite → green.
- [ ] **Step 5: Commit on `omthx-frame-decoder-nv12`; merge into OMT-HX `main`** (`feat: FrameDecoder hands back NV12`, `merge: FrameDecoder NV12`). Full suite on `main`.

### Task 2: OMT-Bridge core — NV12 frames, shared limited-range conversion, thumbnails

On branch `ob-native-layouts`, first move the OMT-HX pin to `main` after Task 1 (one commit `deps: OMT-HX <short hash> with FrameDecoder NV12`, before any build).

**Files:** Modify `src/core/frame.h`, new `src/core/nv12.h` + `src/core/nv12.cpp`, `src/core/thumbnail.cpp`, `src/engine/source_hub.cpp`, `CMakeLists.txt`; tests `tests/frame_test.cpp`, new `tests/nv12_test.cpp`, `tests/thumbnail_test.cpp`, `tests/source_hub_test.cpp`.

**Interfaces:**
- Produces:
  - `frame.h`: `enum class Layout { Uyvy, Nv12 };` `Frame::layout` (default `Uyvy`); `std::vector<uint8_t> nv12;` (Y `w*h` then UV `w*h/2`, full range). `valid()`: for `Nv12`, even width and height ≥ 2, `!format.alpha`, `nv12.size() == w*h*3/2`; for `Uyvy`, as today. `codedOnly()` = `coded` and no picture in either layout.
  - `std::shared_ptr<const std::vector<uint8_t>> Frame::limitedNv12() const;` — for an `Nv12` frame, made on first call (`std::call_once` on a mutable member), the same pointer afterwards; null for `Uyvy`. Copying a `Frame` (NDI's `scratch_`) must still compile: hold the once-state in a `shared_ptr` member, or give `Frame` a copy that resets it.
  - `nv12.h`: `void nv12FullToLimited(std::span<const uint8_t> full, int width, int height, std::vector<uint8_t>& out);` (the table in Global Constraints).
  - `makeThumbnail` reads an `Nv12` frame through `limitedNv12()`, with the same box average and BT.709 maths.
  - `SourceHub::storeFrame` runs the opaque-alpha gate only for `Uyvy` frames with alpha.

- [ ] **Step 1: Failing tests:**
  - `nv12_test`: endpoints from Global Constraints; a 4×2 picture converts plane by plane.
  - `frame_test`: an `Nv12` frame of the right size is `valid()`; a wrong size, an odd size, or `format.alpha` true is not. `limitedNv12()` returns the same pointer twice, also from two threads at once (both threads get one pointer).
  - `thumbnail_test`: a mid-grey `Nv12` frame (Y 128, UV 128 full range) and the same grey as limited `Uyvy` give thumbnails within ±2 per channel.
- [ ] **Step 2: Run** them (`scripts/test.ps1 -Filter …`) → build errors.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the touched tests, then the full suite → green.
- [ ] **Step 5: Commit** `feat(core): NV12 frames with a shared limited-range conversion`.

### Task 3: `OmtSource` decodes HX without alpha to NV12

**Files:** Modify `src/engine/omt_source.cpp` (+ `.h` if a member is added); test the phase-1 `OmtSource` test file.

**Interfaces:**
- Consumes: `FrameDecoder(Nv12)` (Task 1), `Frame::layout`/`nv12` (Task 2).
- Behaviour: an HX coded frame with `format.alpha == false` → `FrameDecoder(Nv12)`, planes copied once into `Frame::nv12`, `layout = Nv12`. With alpha, or VMX → `FrameDecoder(Uyvy)` as in phase 1. When the needed layout changes, make the other decoder (or reset it) and `requestKeyframe()`.

- [ ] **Step 1: Failing tests** (hardware HEVC, early return otherwise): an HX source without alpha with the picture wanted → published frames are `Nv12` and `valid()`. A source that switches alpha on (restart its `Sender` with an alpha format under the same name, or a second sender as the test finds simpler) → frames become `Uyvy` with alpha, every published picture `valid()`.
- [ ] **Step 2: Run** → fail (frames are `Uyvy`).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** touched tests, then the full suite.
- [ ] **Step 5: Commit** `feat(engine): HX sources stay NV12 on the picture lane`.

### Task 4: Sinks take NV12

**Files:** Modify `src/engine/omt_sink.h/.cpp`, `src/engine/ndi_sink.h/.cpp`; tests `tests/omt_sink_test.cpp`, `tests/ndi_sink_test.cpp`.

**Interfaces:**
- `OmtSink`: new seam `virtual void sendNv12(const VideoFormat& f, const uint8_t* y, const uint8_t* uv, bool fullRange, int64_t ts);` (with `mu_` held). An `Nv12` frame (not passed through) → HX target: `sendNv12(frame planes, fullRange = true)`; Omt target: `sendNv12(limitedNv12() planes, fullRange = false)`. Real implementation: `sendVideoNv12(vf, y, w, uv, w, fullRange, ts)`.
- `NdiSink`: an `Nv12` frame → `NDIlib_FourCC_video_type_NV12`, `p_data` = `limitedNv12()->data()`, `line_stride_in_bytes = width`. Alpha settings do not apply (no alpha).

- [ ] **Step 1: Failing tests:**
  - `omt_sink_test`: an `Nv12` frame to an OmtHx output → `sendNv12` with `fullRange == true` and the frame's own Y pointer; to an Omt output → `fullRange == false` and Y pointer equal to `limitedNv12()->data()`.
  - The shared-conversion check: one `Nv12` frame delivered to an Omt `TestSink` and an NDI test sink → both saw the same `limitedNv12()` pointer.
  - Levels, end-to-end (hardware HEVC): an HX source sending limited-range UYVY black (Y 16) and mid-grey (Y 126) → through the bridge's source and a plain-OMT `OmtSink` → a `Uyvy` omthx receiver reads Y within ±2 of 16 and 126.
- [ ] **Step 2: Run** → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** touched tests, then the full suite.
- [ ] **Step 5: Commit, merge into `main`** (`feat(engine): sinks take NV12 as it comes`, `merge: native layouts`). Full suite on `main`.
