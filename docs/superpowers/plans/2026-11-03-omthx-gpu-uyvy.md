# OMT-HX: UYVY to NV12 on the GPU for HX Senders — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `sendVideoUyvy` on an HX sender (no alpha) makes its NV12 picture on the GPU instead of with `packUyvy` on the CPU. The CPU work per frame drops from two passes (pack, then the encoder's own upload) to one (a byte reorder written straight into a mapped texture).

**Architecture:** A new `UyvyToNv12` in `src/d3d11.cpp`, next to `BgraToNv12`, owns a D3D11 device (the default adapter, video support). Each frame it writes the UYVY rows into a DYNAMIC `DXGI_FORMAT_YUY2` texture with `Map(WRITE_DISCARD)`, reordering U Y0 V Y1 → Y0 U Y1 V on the way. The video processor turns limited-range YUY2 into a full-range NV12 frame from an FFmpeg D3D11 pool, exactly as `BgraToNv12` does for BGRA. The sender then encodes it through the existing `sendVideo(VideoFrame)` GPU path. Any failure falls back to `packUyvy` for good, like `gpuRefused`.

**Tech Stack:** C++20, D3D11 video processor, FFmpeg hwcontext_d3d11va, doctest.

**Spec:** `OMT-Bridge/docs/superpowers/specs/2026-11-03-omt-bridge-data-path-design.md` (§3.6, phase 3). Spec deviation, recorded here: UYVY is not a DXGI format, so the processor reads YUY2 and the reorder rides the upload copy, rather than a shader.

**Runs after:** the bridge native-layouts plan is merged (it is the last user of the old pin).

## Global Constraints

- Only `Mode::Hx`, without alpha, on Windows. With alpha, in Full mode, or elsewhere, `sendVideoUyvy` is unchanged.
- The NV12 picture matches `packUyvy`'s: full range, BT.709. Levels within ±2 of the CPU path.
- Kill switch: environment `OMTHX_NO_GPU_UYVY=1` keeps the CPU path (read once per sender).
- A failure (no device, no video processor, no YUY2 input support, an encoder that refuses D3D11 frames) logs nothing new, sets a refusal flag, and sends that frame and every later one through `packUyvy`. A frame is never lost to a refusal.
- Never push; OMT-HX owner files stay untouched.

## Review Focus

1. **Levels and colour:** black, white, mid-grey and a saturated red through the GPU path match `packUyvy` within ±2 (Y, U, V). Test in Task 1.
2. **Stride larger than `width * 2`** (rows padded): the picture is not skewed. Test in Task 1.
3. **A size change** mid-stream: the converter and the encoder reopen; no stale texture size. Test in Task 2.
4. **A PC whose default adapter cannot encode** (an encoder on another GPU): the fallback keeps frames flowing. Test in Task 2 by forcing the refusal through a test hook.
5. **Throughput:** the CPU path must not get slower for alpha frames or Full mode (unchanged code). Covered by the existing suite.

---

### Task 1: `UyvyToNv12`

**Files:** Modify `src/codec.h`, `src/d3d11.cpp`; test `tests/codec_test.cpp`.

**Interfaces:**
- Produces:
  ```cpp
  // Limited-range UYVY in memory to a full-range NV12 frame in video memory (BT.709), on this object's own D3D11
  // device. Windows only; elsewhere, or when the device cannot do it, convert throws std::runtime_error.
  class UyvyToNv12 {
   public:
    UyvyToNv12();
    ~UyvyToNv12();
    void convert(const uint8_t* uyvy, int stride, int width, int height, GpuPicture& out);
   private:
    struct Impl;
    std::unique_ptr<Impl> d_;
  };
  ```
  Device: `D3D11CreateDevice(nullptr, HARDWARE, …, D3D11_CREATE_DEVICE_VIDEO_SUPPORT)`, multithread-protected. Stream colour space `DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709` (or the old API's `Nominal_Range 16_235`, `YCbCr_Matrix 1`), output `YCBCR_FULL_G22_LEFT_P709`. Check `D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT` for `DXGI_FORMAT_YUY2`. Share the processor and pool setup with `BgraToNv12` where it is the same code, rather than copying it.

- [ ] **Step 1: Failing tests** (skip with a `MESSAGE` when `convert` throws `runtime_error` on this PC):
  - `"UyvyToNv12 matches packUyvy"`: 64×16 UYVY with four vertical bars (black Y16/U128/V128, white Y235, grey Y126, red Y63/U102/V240) → download the NV12 frame (`downloadNv12`) and compare with `packUyvy` of the same input: every Y, U, V within ±2.
  - `"UyvyToNv12 reads padded rows"`: the same picture with `stride = width * 2 + 64` → same result.
- [ ] **Step 2: Run** `scripts/test.ps1 -Filter "UyvyToNv12*"` → build error.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the new tests, then the full suite → green.
- [ ] **Step 5: Commit** on `omthx-gpu-uyvy`: `feat: UYVY to NV12 on the GPU`.

### Task 2: `sendVideoUyvy` uses it

**Files:** Modify `src/link.cpp`; test `tests/link_test.cpp`.

**Interfaces:**
- Consumes: `UyvyToNv12` (Task 1); the existing `sendVideo(const VideoFrame&)` GPU branch.
- Behaviour: in `Sender::sendVideoUyvy`, Hx mode, `!fmt.alpha`, not refused, kill switch off → make (once) a `UyvyToNv12`, `convert` into a new `GpuPicture`, then `sendVideo(frame)`. On `std::runtime_error` from `convert`, or when that `sendVideo` falls back to memory because the encoder took no D3D11 frames (`gpuRefused`), set `uyvyRefused` and use `packUyvy` from then on. The encode-on-demand idle gate stays first.
- Test hook: `Sender`'s constructor reads `OMTHX_NO_GPU_UYVY` into `Impl`; tests set it with `_putenv_s` before making a sender, and clear it afterwards.

- [ ] **Step 1: Failing tests** (hardware HEVC; early return otherwise):
  - `"sendVideoUyvy on an HX sender encodes from video memory"`: a 1280×720 UYVY grey picture pumped for 1 s with a receiver connected → `stats().encoder` ends with `" (d3d11)"`; the receiver's centre Y within ±4 of the input's full-range value.
  - `"sendVideoUyvy keeps working after a size change"`: 640×360 then 1280×720 → both sizes arrive.
  - `"OMTHX_NO_GPU_UYVY keeps the CPU path"`: with the variable set, `stats().encoder` has no `" (d3d11)"`, and pictures still arrive.
  - CPU time: 300 frames at 1920×1080 with a receiver, process CPU time (`GetProcessTimes`) with the GPU path vs with `OMTHX_NO_GPU_UYVY=1` → `MESSAGE` both numbers; `CHECK(gpu < cpu)`.
- [ ] **Step 2: Run** them → the first fails (no `" (d3d11)"`).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the new tests, then the full suite → green.
- [ ] **Step 5: Commit, merge into OMT-HX `main`** (`feat: HX senders take UYVY through the GPU`, `merge: UYVY on the GPU`). Full suite on `main`.

### Task 3: OMT-Bridge takes it

On OMT-Bridge branch `ob-omthx-gpu-uyvy`: move the OMT-HX pin from `sibling` to the new `main` (commit `deps: OMT-HX <short hash>, UYVY to NV12 on the GPU`, before building), run `scripts/test.ps1` → green, merge into `main` (`merge: OMT-HX with GPU UYVY`).
