# OMT Bridge — Design

Date: 2026-10-19
Status: draft for review, revised 2026-10-19 after the first review (canonical frame is
UYVY/UYVA, premultiplication settled from the NDI docs, tally from downstream, audio at the
source's rate).
Upstream repository: https://github.com/bmsolutionsvn/OMT-Bridge (MIT).

This is a new product in the BM Solutions ecosystem, beside BreadStream, BreadCall,
BreadOverlay and BreadCamera. Paths below are relative to the **workspace root** (the
folder holding `OMT-Bridge`, `OMT-HX` and `BreadCamera`). The shared product record is
`PRODUCT.md`, the shared UX rules and glossary are `bm-design/docs/UX-CONTRACT.md`, and
the visual system is `DESIGN.md`. Where this file and the shared records disagree, this
file wins for OMT Bridge.

Reference: NDI Bridge (`https://ndi.video/tools/bridge/`,
`https://docs.ndi.video/all/using-ndi/ndi-tools/ndi-tools-for-windows/bridge`).

## 1. Purpose

OMT Bridge is a native Windows tool that takes any mix of **OMT**, **OMT-HX** and
**NDI** sources on a machine and republishes them as any mix of **OMT**, **OMT-HX**
and **NDI** outputs, converting between the three on the way.

It is the OMT-world analogue of NDI Bridge's *Local* mode: a local proxy and
converter. It is **not** a WAN bridge in v1 — there is no Host, no Join and no tunnel
of its own. That is v2.

The two jobs it is built for, in the words of the tool it is modelled on:

- **Transcode for a lighter link.** Full OMT or NDI in, OMT-HX out, so a signal can
  cross a link that cannot carry the full picture.
- **Repeat and cross over.** One input fanned out to several outputs, and OMT routed
  into the NDI world or NDI routed into the OMT world.

It follows the ecosystem principle *the broadcast does not fail*: the engine owns the
outputs and keeps them running when the UI is closed.

### 1.1 Success criteria

- A source and an output of **any** two of the three kinds can be connected and pass
  video and audio, in both directions, on one machine.
- Full OMT ↔ OMT-HX, NDI → OMT and NDI → OMT-HX all work end to end and are
  demonstrated by a repeatable test.
- An output keeps running, untouched, while the UI is closed and reopened.
- No output can take another output down: a slow or failed output affects only itself.

## 2. Decisions

Locked with the owner on 2026-10-19.

1. **v1 is a local converter/proxy. The WAN tunnel (Host/Join) is v2.**
2. **Full conversion matrix, Full-NDI output only.** Every source kind can feed every
   output kind. NDI output is Full NDI. NDI|HX *output* needs the paid NDI Advanced SDK
   and certification and is out of v1.
3. **N×M routing.** N sources, M outputs; every output picks its own source. There is
   no per-input ownership of outputs.
4. **Native Qt Widgets UI, no web layer.** The engine stays C++/D3D11.
5. **No scale target in v1, and no GPU colour path.** Colour work stays on the CPU. v1
   adds UYVY/UYVA entry points to `omthx` (Section 6.3) so the bridge's 4:2:2 frames reach
   the HX encoder without a colour matrix; the remaining throughput ceiling is documented,
   not engineered away. (A GPU colour path is a later milestone, not v1.) *Revised after
   review: the original decision used `omthx`'s BGRA path unchanged.*
6. **Monitoring is a thumbnail decoded from a keyframe**, per source. No live video
   preview.
7. **Alpha is preserved where the chain allows, and dropped and labelled where it does
   not.** Never silently lost.
8. **Engine process + UI process**, like BreadCamera. Closing the window does not stop
   the bridge. The bridge is a saved file the engine reloads on start.
9. **Approach: new repo, engine assembled from BreadCamera modules lifted where they
   fit**, plus new code for the bridge graph, the OMT source and the Qt UI.
10. **Standalone, English only, no login and no billing** — like BreadCamera.

## 3. Scope

### 3.1 In v1

- **Sources:** OMT (Full), OMT-HX (HEVC or H.264), NDI (Full and NDI|HX receive).
- **Outputs:** OMT (Full), OMT-HX (HEVC or H.264), NDI (Full only).
- N→M routing, each output independently choosing a source, a target and a quality.
- Alpha preserved where the chain allows, else dropped and labelled.
- One keyframe thumbnail per source.
- A saved Bridge file, and a headless engine that survives the UI closing.
- Audio passthrough with the source's channel count and sample rate.
- Tally passed upstream: a source is on air when an output it feeds is on air downstream.
- UYVY/UYVA entry points added to `omthx` (in the OMT-HX repo), additive to its BGRA API.

### 3.2 Out of v1, named so nothing is assumed

- **WAN tunnel, Host and Join** — v2.
- **NDI|HX output** — needs the NDI Advanced SDK and certification.
- **Relay / "Do Not Transcode".** v1 always decodes and re-encodes; there is no coded
  pass-through (Section 16).
- **PTZ and KVM** — NDI-only features with no OMT counterpart.
- **Custom metadata** passthrough beyond what the transports already carry.
- **Live video preview** — thumbnails only.
- **A GPU colour-conversion milestone.**
- Login, billing, Kinde, Vietnamese localisation.

## 4. How it fits

- **OMT-HX** (`OMT-HX`, the sibling repo) is the library. One `omthx::Sender` sends plain
  OMT or OMT-HX; one `omthx::Receiver` reads both, and follows a source that changes codec.
  OMT Bridge adds no protocol of its own.
- **BreadCamera** is the sibling native Windows app and the source of lifted modules
  (Section 14.2). BreadCamera is the *sender and receiver in one app*; OMT Bridge is
  the *converter between them*.
- **BreadStudio** (OBS) is where the vendored NDI SDK pattern comes from
  (`BreadStudio/breadstudio/ndi/ndi-sdk`, `ndi/ndi.cpp`).

## 5. Data model

Three objects, in the ecosystem's level-2/level-3 shape. Level 1 is absent: the tool is
standalone, like BreadCamera.

### 5.1 Bridge (level 2)

The saved thing.

```
name        the advertised prefix, e.g. "Hanoi Bridge"
sources[]   the Source list
outputs[]   the Output list
settings    default target, default quality, thumbnail interval, start-with-app
```

Persisted as one JSON file, reloaded by the engine on start (Section 6.6).

### 5.2 Source (level 3)

One input.

```
kind      OMT | OMT-HX | NDI
address   OMT: "HOSTNAME (name)"  ·  NDI: the NDI source name
state     Ready | Connecting | Live | Offline
format    width, height, fps (rational), alpha
codec     for HX: HEVC | H.264
thumb     the latest keyframe thumbnail
```

Sources are discovered (`omthx::discover()`, NDI Find) or typed in by hand.

### 5.3 Output (level 3)

One republished signal.

```
sourceId  any Source, or none (none leaves the row Ready and idle)
target    OMT | OMT-HX | NDI
codec     HEVC | H.264          (OMT-HX only)
quality   Low | Medium | High   (OMT-HX only)
alpha     Auto | Off
name      the row name; advertised as "<Bridge.name> (<Output.name>)"
state     Ready | Connecting | Live | Offline
stats     fps, bitrate, bytes, frames dropped
tally     the downstream tally read from this output's sender (Section 12.3)
```

Nothing else. No groups, no scenes, no effects, no show-wide frame rate.

## 6. Engine architecture

### 6.1 Two processes

```
OMT-Bridge.exe          Qt Widgets UI + Supervisor
   │  WebSocket, 127.0.0.1, ?token=<random>   ← JSON commands up; JSON state + binary thumbnails down
OMT-Bridge-Engine.exe   headless: owns Sources, Outputs, the bridge graph
```

The engine's control server is BreadCamera's `ControlServer` lifted as-is
(`src/engine/control_server.h`: loopback WebSocket, mandatory `?token=`, JSON text
replies, `sendBinary` for thumbnails). The UI hosts a `QWebSocket` client.

The UI's `Supervisor` keeps the engine alive with the same policy as BreadCamera
(`src/shell/supervisor.h`): 3 s heartbeat timeout, 8 s startup grace, 5 restarts inside
60 s, then one retry every 10 s. **Closing the window leaves the engine bridging.**
The engine stops only on an explicit **Stop all outputs** or on quitting.

### 6.2 No master tick

This is the one deliberate divergence from BreadCamera. BreadCamera has one show-wide
frame rate and a single master tick; a bridge has **independent sources at different
rates**, so pacing comes from each source, not a global clock. (BreadCamera removed its
own master clock in `docs/superpowers/specs/2026-10-17-no-master-clock-design.md`.)

### 6.3 Canonical frame

**The engine's one frame type is 8-bit 4:2:2 YUV: `UYVY`, or `UYVA` when the frame
carries alpha**, with `alpha` and `premultiplied` flags. BT.709 for HD, BT.601 for SD,
limited range (16–235), as NDI defines `UYVY`.

Why not BGRA: both full-bandwidth codecs are 4:2:2 YUV inside. libvmx converts BGRA to
YUV 4:2:2:4 before coding (`VMX_BGRAToYUV4224`, `libvmx/src/vmxcodec.cpp`), and NDI's
SpeedHQ is 4:2:2 (4:2:2:4 with alpha). A BGRA internal frame would make every Full OMT
and NDI hop run 4:2:2 → RGB → 4:2:2, two lossy colour passes per hop at twice the
bytes. UYVY is what both SDKs decode to natively, and OMT uses the same FourCC values
and layouts as NDI for `UYVY`, `UYVA`, `P216` and `PA16`, so a Full OMT ↔ NDI hop is a
row copy that honours each side's stride.

Every input adapter delivers this frame; every output adapter encodes from it. No other
internal format exists, which is what makes the matrix (Section 7) have no special cases.
16-bit (`P216`/`PA16`) is a later extension of the same frame, not v1.

**The `omthx` change this needs (OMT-HX repo, additive).** `omthx` today speaks BGRA
only: `Receiver` asks libomt for `OMTPreferredVideoFormat_BGRA` and `Sender` packs BGRA to
NV12 with a colour matrix. v1 adds:

- `Sender::sendVideoUyvy(format, uyvy, alpha plane or null, premultiplied, timestamp)`.
  Full mode hands libomt `OMTCodec_UYVY`/`OMTCodec_UYVA` directly. HX mode makes the NV12
  picture with **no colour matrix**: luma through a range table, chroma averaged
  vertically 4:2:2 → 4:2:0, alpha copied into the stacked rows (OMT-HX §7). Premultiplying
  straight input is done in YUV, `Y' = 16 + (Y−16)·a`, `C' = 128 + (C−128)·a`, which equals
  premultiplying in RGB because the matrix is linear; chroma uses the mean alpha of the
  pixels it covers.
- A `Receiver` option to return `UYVY`/`UYVA`. Full mode asks libomt for
  `OMTPreferredVideoFormat_UYVYorUYVA`. HX mode makes UYVY from the decoded NV12 by
  repeating chroma rows and copies the stacked alpha rows into the alpha plane.

BreadCamera and BreadStudio keep the BGRA API unchanged. The cost of the new HX path is
expected to be well under the 5 ms BGRA ↔ NV12 pass (OMT-HX `docs/MEASUREMENTS.md`) but
is **measured in M2, not assumed**.

### 6.4 SourceHub

One per Source.

- Owns the input adapter and publishes `shared_ptr<const Frame>` into a **single
  latest-frame slot** — overwritten by the next frame, never queued.
- **Reference-counted.** The adapter runs iff **≥ 1 Output references the source** *or*
  its thumbnail is wanted. Otherwise it disconnects. This demand-driven behaviour is
  what makes it affordable to discover and list everything.
- Produces the thumbnail (Section 11) and owns the tally (Section 12.3).
- **Detects opaque alpha** (Section 9.2): a source whose frames say alpha but whose alpha
  plane is all 255 is republished without alpha. One scan per source frame, shared by
  every output.

### 6.5 Output row

One worker per Output.

- Reads its source's latest frame and delivers it to its sink.
- **Decode is shared** (one per Source); **conversion and encode are per Output.**
- Owns a tiny bounded queue with **drop-oldest**. Dropping is safe: OMT-HX numbers its
  coded frames and the receiver holds the last picture; Full OMT and NDI carry whole
  frames.
- **Isolated.** A slow or failed output drops its own frames and never blocks another
  row or its SourceHub.

### 6.6 Bridge file

One JSON file, owned by the UI. On start the engine loads the last Bridge file and
re-establishes every output. The engine never edits the file; a change that fails in the
engine (a source it could not reach) is reported to the UI as state, and the UI decides
whether to save it.

## 7. Conversion matrix

Every cell is the same pipeline: **decode → UYVY/UYVA → encode.** The matrix is listed
for its usefulness, not its mechanics. Cells between Full OMT and NDI involve no colour
conversion at all; cells touching OMT-HX involve only the matrix-free 4:2:2 ↔ 4:2:0 step
in `omthx` (Section 6.3).

| Source ↓ / Output → | OMT (Full) | OMT-HX (HEVC/H.264) | NDI (Full) |
|---|---|---|---|
| **OMT (Full)** | repeater / fan-out | **headline: VMX → HEVC/H.264 for a light link** | OMT world → NDI world |
| **OMT-HX** | decompress to full | re-code (H.264 → HEVC, or new quality) | OMT-HX world → NDI world |
| **NDI** | NDI world → OMT full | **headline: NDI world → OMT-HX** | repeater / fan-out |

The four same→same cells are the *unicast repeater* case (one transmitter fanned out to
several receivers). They still transcode — Section 16.

### 7.1 Per-output settings

```
sourceId · target ∈ {OMT, OMT-HX, NDI} · codec ∈ {HEVC, H.264} · quality ∈ {Low, Medium, High} · alpha ∈ {Auto, Off} · name
```

`codec` and `quality` apply to OMT-HX only; the UI hides them for OMT and NDI targets.

`alpha: Auto` (the default) follows the source, after opaque-alpha detection (9.2).
`alpha: Off` is the operator explicitly choosing to save bandwidth; the row then shows
**"alpha dropped"**. An H.264 output above 1080p also drops alpha (9.3).

### 7.2 Keyframes and start-up

An output never waits for a source keyframe, because it re-encodes from decoded frames.
`omthx::Sender` forces a keyframe on its first frame, and **each Output owns its own
Sender**, so a new output joins cleanly. Only the thumbnail cares about keyframes
(Section 11).

## 8. I/O adapters

Four thin adapters, all mapping to and from the UYVY/UYVA frame:

| Adapter | Wraps | Notes |
|---|---|---|
| `OmtSource` | `omthx::Receiver`, UYVY option | reads Full **and** HX; `omthx::discover()` fills the source list; `omthx::probe()` reads format when not streaming; `premultiplied` from the frame |
| `OmtSink` | `omthx::Sender::sendVideoUyvy` | `Mode::{Full,Hx}` + `Quality` + `Codec::{Hevc,H264}`; passes `premultiplied` through |
| `NdiSource` | NDI Find + Recv | `NDIlib_recv_color_format_fastest` (UYVY, or UYVA with alpha), bandwidth highest; FLTP audio at the source's rate; NDI\|HX is decoded by the NDI SDK |
| `NdiSink` | NDI Send | `UYVA` when alpha, else `UYVY`; straight alpha only (9.1); **Full only** |

These are lifted from BreadCamera where they fit (`io/ndi_input.cpp`, `io/ndi_output.cpp`,
`io/omt_output.cpp`) and adapted: BreadCamera's NDI and OMT paths speak BGRA and its
`ndi_input` writes straight into a D3D11 texture, while a bridge needs a neutral UYVY
frame several sinks can share. The lifted adapters are rewired to produce and consume
that `Frame`; BreadCamera's own copies are not changed.

## 9. Alpha and premultiplication

Alpha is a per-frame flag carried on the canonical frame.

- **Full OMT** carries alpha as `UYVA` (VMX codes YUV 4:2:2:4), free.
- **OMT-HX** carries optional alpha as a stacked plane (a 1080p alpha source codes as
  1920×2176). It doubles the coded **pixels**, not the bits: `omthx` adds 25 % bitrate for
  alpha (`targetBitrate`, OMT-HX `src/pure.cpp`) because the alpha half is flat with neutral
  chroma; 1080p60 alpha measured 31.8 Mbps. The real cost is codec and CPU throughput
  (12.6 ms to send, 13.1 ms to receive per 1080p60 alpha frame on the test PC, against a
  16.7 ms frame). It is lossy. Design: OMT-HX §7.
- **NDI (Full)** carries alpha as `UYVA` (SpeedHQ 4:2:2:4).
- **NDI|HX** carries alpha the same way OMT-HX does: NDI's codec matrix describes HX2/HX3
  alpha as *YUVA 4:2:0:4, "implemented by vertically stacking the video and alpha frames
  into a single frame before compression"*, produced by NDI Bridge, TriCaster, Vectar Plus
  and Vizrt Connect. The NDI SDK decodes it; the bridge receives `UYVA`.

OMT-HX's packing is therefore already NDI|HX's packing. There is no better scheme to
copy; the optimisations are in 9.2 and 9.3.

### 9.1 Premultiplication: carry the flag, convert at one edge

The conventions differ by transport, and are documented, not assumed:

- **NDI is straight.** NDI SDK, *Frame Types*: `BGRA` and `RGBA` — *"This data is not
  pre-multiplied."* NDI documents no convention for `UYVA`; v1 treats it as straight,
  consistent with `BGRA`, and verifies it (18.1).
- **OMT-HX is premultiplied.** OMT-HX §7: *"the fill is multiplied by alpha before
  encoding"*; `omthx` reports it as `VideoFrame::premultiplied`.
- **Full OMT is either**, and says which with `OMTVideoFlags_PreMultiplied`.

The engine does **not** pick one convention. Every frame carries `premultiplied`, set by
its source adapter from its transport. Sinks that carry the flag pass it through untouched:

| Edge | Convention | Work |
|---|---|---|
| NDI in | straight | `premultiplied = false` |
| Full OMT in | per frame | read `OMTVideoFlags_PreMultiplied` |
| OMT-HX in | premultiplied | `premultiplied = true` |
| Full OMT out | per frame | set the flag from the frame, no conversion |
| OMT-HX out | either accepted | pass the flag; `omthx` premultiplies straight input itself |
| **NDI out** | **straight only** | **un-premultiply when the flag is set** |

The only conversion in the whole matrix is a premultiplied frame going to NDI. It lives in
one function, `toStraight()`, applied in YUV like 6.3's premultiply:
`Y = 16 + (Y'−16)/a`, `C = 128 + (C'−128)/a`, clamped to the legal range, with colour set to
black where `a = 0`. Dividing by a small alpha amplifies codec noise at soft edges; that
is inherent to 8-bit lossy input and accepted.

### 9.2 Opaque alpha is dropped automatically

Many senders mark frames as alpha while every pixel is opaque. Carrying that alpha would
double the HX coded picture for nothing. Under `alpha: Auto`, the SourceHub scans the alpha
plane (stopping at the first value below 255):

- any value below 255 → alpha **on**, immediately;
- all 255 for **2 s** of frames → alpha **off**.

The hysteresis is required: changing `alpha` changes the HX coded height, so `omthx`
reopens the encoder and forces a keyframe (`Sender::Impl::openEncoder`, OMT-HX
`src/link.cpp`). Alpha must not flap. A row whose alpha was dropped this way shows
**"alpha opaque"**, not "alpha dropped", because nothing was lost.

### 9.3 H.264 alpha is HD only

Copied from NDI Bridge, whose docs say H.264 alpha is HD only, while HEVC covers every
reasonable resolution with or without alpha. An OMT-HX H.264 output whose source is above
1080p with alpha drops alpha and shows **"alpha dropped — H.264 above 1080p, use HEVC"**.
HEVC outputs are bounded only by what the encoder opens; a failure to open is a system
error naming the coded size.

If a target cannot carry alpha, the frame is sent opaque (colour over black, which a
premultiplied frame already is) and the row shows *"alpha dropped"*.

## 10. Audio

- **Passthrough, no mixing.** Planar float, the source's channel count **and sample rate**.
- NDI is `FLTP` planar float; OMT is planar float (`FPA1`). The conversion is a
  channel-count copy, as BreadCamera already does (`src/io/ndi_output.cpp`,
  `src/io/omt_output.cpp`), with the source's rate passed on (`omthx::Sender::sendAudio`
  and the NDI audio frame both take a sample rate).
- BreadCamera ignores non-48 kHz NDI audio (`ndi_input.cpp:92`) because it feeds a 48 kHz
  mixer. The bridge has no mixer, so that rule is **not** lifted; no resampling either.
- A source and every output fed by it carry audio together; audio never gates video.

## 11. Thumbnails

- Produced by the SourceHub, which already holds the decoded UYVY frame.
- Box-downscaled and converted to a small BGRA image (the only RGB conversion in the
  engine, on a thumbnail-sized picture), refreshed **at most once every 2 s**.
- Pushed to the UI as binary frames over the control channel.
- This is also the liveness signal for the four signal states.
- For **OMT-HX** the keyframe comes for free: `omthx::Receiver` discards frames until the
  first keyframe, so its first delivered frame is a clean one. For **Full OMT** and
  **NDI** every frame is intra, so the first delivered frame is used as-is.

## 12. States, tally and errors

### 12.1 Signal states

The UX contract's four, in fixed order, one component:

**Ready** (configured, adapter not started) → **Connecting** (adapter running, no picture
yet) → **Live** (picture flowing) → **Offline** (was live, no video).

### 12.2 System errors

One separate band, never the signal band, never a toast. Validation errors appear inline
under the field. A system error is a persistent block at the top of the affected pane and
stays until resolved.

### 12.3 Tally, passed upstream

A source is on air when something downstream has it on air, not when the bridge relays
it. Setting `program` whenever an output is live would light the camera's red tally for as
long as the bridge runs.

- Each output reads its **downstream** tally from its sender: `omthx::Sender::tally()` for
  OMT and OMT-HX, `NDIlib_send_get_tally` for NDI.
- The SourceHub ORs `program` and `preview` across every output fed by its source.
- It sets the result upstream: `omthx::Receiver::setTally` for OMT and OMT-HX sources,
  `NDIlib_recv_set_tally` for NDI sources.
- Polled with the stats, not per frame.

## 13. UI (Qt Widgets)

BreadCamera's chrome, not a web shell: menu bar, toolbar, main area, status bar; Settings
is a dialog. English only. Window title `OMT Bridge`.

### 13.1 Main window

- **Sources** pane: one row per Source — thumbnail, name, kind badge (`OMT` / `OMT-HX` /
  `NDI`), format (`1920×1080 · 60p`, mono, tabular), codec for HX, and the state pill.
  Toolbar: **Add source** (discovery list, or type an address).
- **Outputs** pane: one row per Output — name, a **source picker**, target + codec +
  quality, the state pill, and live `fps / Mbps` in mono. Toolbar: **Add output**.
- **Matrix view** (a toggle): rows = Sources, columns = Outputs, a filled cell where an
  Output takes that Source. Read-mostly; assignments are edited in the Outputs pane.
- **Status bar:** engine state (running / restarting / stopped) and total in/out bitrate.
  The screen's **one primary action** is top-right: **Start**, or **Stop all outputs**
  once running (through the confirm dialog in 13.4).

### 13.2 Settings dialog

Default target and quality for new outputs, thumbnail refresh interval, and
"start engine with the app".

### 13.3 Design language

`@bmsolutionsvn/design` is a React/MUI package and cannot be imported by Qt. The repo
carries a small `tokens.h` + `tokens.qss` derived from `DESIGN.md`: blueprint cyan on
plate black, three surface steps, hairline borders, radii 4/6/999px, Inter for text,
JetBrains Mono with tabular figures for every number. The **signal pill** is the one
custom widget — 999px, mono uppercase label, state-text colour on a 15% wash with a
solid dot; Live pulses and goes static under reduced motion.

### 13.4 UX contract

- Exactly **one primary action** per screen, top-right.
- Validation inline under the field; system errors a persistent block at the top of the
  affected pane; toasts only for brief success.
- Destructive actions (remove source, stop all outputs) go through a confirm dialog whose
  primary button **names the verb** ("Remove source", "Stop all outputs"), secondary left,
  primary right.
- Errors never use On-Air Red. The signal band and the system band stay separate.

## 14. Repository, build and dependencies

### 14.1 Repository

- `github.com/bmsolutionsvn/OMT-Bridge`, MIT (as `LICENSE` already states).
- Windows x64, Visual Studio 2022 or newer, C++20 — the same toolchain as OMT-HX and
  BreadCamera.
- CMake + `CMakePresets.json` + `vcpkg.json`, matching the two sibling repos.

### 14.2 Lifted from BreadCamera

`io/ndi_input`, `io/ndi_output`, `io/omt_output`, `io/queued_output`,
`gpu/{device,converter,texture_pool}` (the GPU converter comes along for the later GPU
milestone even though v1 does not use it), `core/{signal_state,bounded_queue,rate}`,
`engine/control_server`, `engine/command_router` (pattern), `shell/{supervisor,process_launcher}`,
and the `show_store` config pattern (becomes `bridge_store`).

**Not** lifted: the show/camera/effect/calibration model, matte backends, AJA, DeckLink,
virtual camera, preview service.

### 14.3 Dependencies

| Dependency | How | Notes |
|---|---|---|
| OMT-HX | git submodule; `add_subdirectory`; `target_link_libraries(... omthx)`; `omthx_copy_runtime(...)` | brings `libomt.dll`, `libvmx.dll`; pinned at or after the commit that adds the UYVY/UYVA API (6.3) |
| FFmpeg (`avcodec`, `swscale`, `nvcodec`, `amf`) | vcpkg | required by OMT-HX |
| NDI SDK | vendored like `BreadStudio/breadstudio/ndi/ndi-sdk` | header + import lib + runtime DLL; guarded by `OB_WITH_NDI`, mirroring `BC_WITH_NDI` |
| Qt 6 (`qtbase`, Widgets) | vcpkg | **LGPLv3 — dynamically linked; Qt DLLs shipped, never statically linked** |
| ixwebsocket | vcpkg, as BreadCamera | engine control server |
| Catch2 | vcpkg, as BreadCamera/OMT-HX | tests |

### 14.4 CI

GitHub Actions on a Windows runner: configure, build, run tests — mirroring
`OMT-HX/.github/workflows/build.yml`. The NDI SDK and the Qt dependency are the two items
`18` must settle before CI is green.

## 15. Testing and verification

### 15.1 Unit tests (Catch2, co-located)

- The data model and the Bridge JSON round-trip.
- The bridge graph: reference counting (adapter runs iff referenced or thumbnail-wanted),
  drop-oldest behaviour under a slow sink, isolation between output rows.
- The alpha/`premultiplied` table of 9.1, one case per row, and `toStraight()` against
  known values (`a = 0`, `a = 255`, a mid value, clamping).
- Opaque-alpha detection (9.2): on at the first non-255 value, off only after 2 s of
  opaque frames, no flapping on alternating input.
- The H.264-above-1080p alpha rule (9.3) and its row label.
- Tally (12.3): the OR across outputs, and that a live output with no downstream tally
  sets nothing upstream.
- The command router's input validation (mirroring BreadCamera's
  `engine/command_router.cpp` message style).

In the OMT-HX repo, alongside its existing tests:

- `sendVideoUyvy` / UYVY receive round trip in both modes, with and without alpha.
- YUV-domain premultiply equals RGB premultiply within ±1 on a test pattern.
- The YUV range OMT's `UYVY` uses (black at 16 or 0), checked against libvmx, so 6.3's
  "limited range" is confirmed rather than assumed.

### 15.2 End-to-end, on loopback (the real test)

The far ends are OMT-HX's **own tools**, so the test proves interop with the shipped
library rather than with itself:

- **OMT-HX in → OMT-HX out:** `omthx-send.exe` → OMT Bridge → `omthx-recv.exe --dump
  frame.bmp`; assert a picture, assert resolution and fps.
- **OMT-HX in → OMT out, OMT in → OMT-HX out:** same shape, different targets.
- **NDI self-loop:** an NDI output fed back into an NDI source; assert video and audio.
- **Alpha round trip:** an OMT-HX alpha source through an `alpha: Auto` output; assert the
  alpha plane survives, and that `alpha: Off` composites over black and labels the row.
- **Premultiplied to NDI:** an OMT-HX alpha source into an NDI output, received back by
  an NDI source; assert the colour of a 50 % alpha patch is the straight value.
- **Opaque alpha:** an alpha source with all-255 alpha into an OMT-HX output; assert the
  coded height drops to the display height after 2 s and the row shows "alpha opaque".

### 15.3 Manual checks

- **NDI alpha smoke test** (18.1): a keyed picture over a checkerboard in NDI Studio
  Monitor, sent as `UYVA` with a soft edge; no dark or bright fringe. Confirms the
  undocumented `UYVA` convention in a real receiver.
- **Tally**: an OMT output taken to program in a downstream receiver lights the upstream
  camera's tally; taking it off clears it.
- HEVC decoder availability on a clean Windows install (18.3).
- A UI-close test: outputs survive the window closing and reopening.

## 16. Limits of v1

- **No relay.** `omthx::Receiver` returns decoded pictures only, so an OMT-HX → OMT-HX hop
  with identical settings still decodes and re-encodes. NDI Bridge's *Do Not Transcode* has
  no equivalent here. This is a future milestone, not a defect.
- **CPU work per HX hop.** The UYVY path removes `omthx`'s BGRA ↔ NV12 colour matrix (about
  5 ms per 1080p frame each way), but each HX output still does a CPU 4:2:2 → 4:2:0 pass
  and a copy to the encoder, and each HX source a copy back from the D3D11 decoder. There is
  **no scale target in v1** and no GPU colour path; the per-output cost is measured in M2
  and documented, and the GPU milestone is deferred.
- **HX → HX alpha at 1080p60 is the measured ceiling case.** The bridge decodes and
  encodes in one process, which is OMT-HX's "sender and receiver on one PC" run: a median
  of 57 fps received at 1080p60 alpha (OMT-HX `docs/MEASUREMENTS.md`, BGRA path). 720p60
  and 1080p30 alpha hold. M2 re-measures this on the UYVY path.
- **No NDI|HX output** (NDI Advanced SDK).
- **NDI|HX input alpha is present only when the HX sender stacks it** (NDI Bridge,
  TriCaster, Vectar Plus, Vizrt Connect); most HX cameras send none. When present, the NDI
  SDK decodes it and the bridge treats it like any `UYVA` source. Our NDI output is Full
  only.
- **NDI `UYVA` premultiplication is undocumented**; v1 treats it as straight like `BGRA`
  and confirms it in the smoke test (18.1).
- **No tally into NDI**, no PTZ, no KVM, no live preview, no custom metadata.

## 17. Risks

| Risk | Mitigation |
|---|---|
| NDI `UYVA` turns out premultiplied → fringing on keyed NDI | `BGRA` is documented straight; `UYVA` is the one assumption, isolated in `toStraight()` and the 9.1 table, checked in the smoke test before M3 ships (18.1) |
| The `omthx` UYVY API breaks BreadCamera or BreadStudio | Additive only: the BGRA API and its behaviour are unchanged, and OMT-HX's existing tests must still pass |
| CPU work per HX hop caps usable outputs at a handful | UYVY path removes the colour matrix; remaining cost measured in M2 and documented; GPU converter already lifted into the repo for the next milestone |
| Alpha flapping on a source that is opaque some of the time | 2 s hysteresis before dropping alpha (9.2) |
| Qt LGPL and NDI SDK redistribution terms | Qt dynamically linked only; NDI runtime shipped per SDK terms, or required as a prerequisite if terms do not allow it (18.2) |
| HEVC decode unavailable on some Windows installs | Detect and report a system error naming the missing decoder (18.3) |
| Lifting BreadCamera modules drags in the camera model | Explicit "not lifted" list (14.2); review each lift at the boundary |

## 18. Open decisions for the owner

1. **NDI premultiplication convention — mostly settled.** `BGRA`/`RGBA` are straight per
   the NDI SDK's *Frame Types* ("not pre-multiplied"). `UYVA` is undocumented and treated as
   straight; the NDI Studio Monitor smoke test (15.3) confirms it before the NDI output
   milestone ships. If it proves premultiplied, only the 9.1 table's NDI rows change.
2. **NDI runtime distribution.** Ship the NDI runtime DLL with the installer, or require
   NDI Tools / the NDI Runtime as a prerequisite? Depends on the SDK redistribution terms.
3. **HEVC decoder on Windows.** HEVC decode may need the Microsoft Store extension on a
   clean install. Ship a check and a named error, or require it as a prerequisite?
4. **Qt acquisition.** vcpkg `qtbase` (matches the vcpkg habit) or the official Qt
   installer (smaller build, easier deployment)? Needs a licence decision either way.
5. **NDI SDK version to pin** for the vendored copy.
6. **Bridge file name and location.** Extension (suggested `.ombridge.json`) and default
   folder (`%APPDATA%\BMSolutionsVN\OMT-Bridge\`).
7. **Output name uniqueness.** Every output is advertised as
   `<Bridge.name> (<Output.name>)` (5.3). Decide whether the UI forces that advertised
   string to be unique across the machine (discovery needs unique names), or allows
   duplicates and lets the operator resolve a collision.
8. **A relay mode later** — worth a v1.x milestone if bandwidth matters more than CPU.

## 19. Alternatives considered

- **A local converter inside BreadCamera** instead of a new app. Rejected: BreadCamera's
  show/camera/effect model does not fit a router, and it would conflate two products.
- **A single-process app.** Rejected: a UI crash or a mis-click would kill live outputs,
  against *the broadcast does not fail*.
- **A web UI (WebView2/React)**, reusing `@bmsolutionsvn/design` directly. Rejected by the
  owner (decision 4); the cost is a native re-implementation of the design tokens.
- **A pure Win32/Direct2D UI.** Rejected in favour of Qt Widgets: maturity for a
  table-and-dialog-heavy tool, and Qt is already proven in `BreadStudio`.
- **A headless CLI only.** Would cover the transcoding but not the source/output
  management the tool exists for.
- **A shared `bm-media` library across BreadCamera and OMT Bridge.** Rejected as premature:
  it refactors a working app to serve a product that does not exist yet. Revisit if a third
  app needs it.
- **Full NDI Bridge parity in v1 (Host/Join).** Rejected: the owner scoped the WAN tunnel
  to v2.

## 20. Milestones

| # | Milestone | Done when |
|---|---|---|
| M0 | Repo skeleton: CMake + vcpkg + presets, OMT-HX submodule, NDI SDK vendored, empty engine + empty Qt window, CI builds | CI green; `OMT-Bridge.exe` opens and launches `OMT-Bridge-Engine.exe` |
| M1 | Engine core: canonical Frame, SourceHub, Output row, bridge graph, reference counting, control server, Supervisor | unit tests for the graph pass; outputs survive a control-channel disconnect |
| M2 | `omthx` UYVY/UYVA API (OMT-HX repo, 6.3); OMT / OMT-HX in and out: `OmtSource`, `OmtSink`, opaque-alpha detection, H.264 alpha rule | OMT-HX's tests (old and new) pass; the `omthx-send` → Bridge → `omthx-recv --dump` test passes; the per-HX-output cost and the 1080p60 alpha HX → HX rate are measured and written into Section 16 |
| M3 | NDI in and out: `NdiSource`, `NdiSink`, `toStraight()`, tally upstream | NDI self-loop and premultiplied-to-NDI tests pass; NDI alpha smoke test passes (18.1) |
| M4 | UI: Sources, Outputs, Matrix, states, settings, Bridge file, confirms | a full manual pass; Bridge file reloads on restart |
| M5 | Hardening: thumbnails, tally, error blocks, alpha labelling, soak | the success criteria in 1.1 are demonstrated |