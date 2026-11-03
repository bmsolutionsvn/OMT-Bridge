# OMT-Bridge: pass coded frames through, keep native pixel formats — Design

Date: 2026-11-03. Status: description approved by the owner in chat (2026-11-03). Project 2 of the BreadMedia
data-path backlog (`docs/superpowers/2026-11-03-data-path-backlog.md`, item 3). Builds on project 1
(`2026-11-03-encode-on-demand-design.md`).

## 1. Why

OMT-Bridge is BreadMedia's NDI Bridge: it brings existing NDI hardware (PTZ cameras, converters, often NDI|HX) into
OMT tools (BreadCamera, BreadReplay, BreadStudio, vMix's OMT input), and sends OMT outputs back out as NDI. Its
value is compatibility and bandwidth (full NDI → OMT-HX), not picture processing.

Today every route decodes to one frame type (8-bit UYVY, `src/core/frame.h`) and re-encodes:
- `OmtSource` opens `omthx::Receiver(…, PixelFormat::Uyvy)`. An HX source is decoded on the GPU, downloaded as
  NV12, and unpacked to UYVY on the CPU.
- `OmtSink` calls `sendVideoUyvy`. For an HX output, OMT-HX packs UYVY back to NV12 on the CPU and the encoder
  uploads it again.
- So **HX → HX** costs a decode, two CPU repacks, two cross-device copies and a fresh encode, and loses quality.
  **OMT → OMT** costs a VMX decode and a VMX encode.
- Each output converts on its own. Two HX outputs of one source pack the same picture twice.

## 2. Goals, non-goals, anti-goals

Goals
- Same family in and out (OMT → OMT, HX → HX, and with the NDI Advanced SDK, NDI|HX ↔ OMT-HX): compressed frames
  pass through. No decode, no encode, no generational loss.
- Different families: decode once per source, keep the decoder's native layout, convert at most once per source per
  target layout, and share the result across all outputs.
- One network connection per source, whatever its outputs need.
- With project 1, an output nobody pulls costs nothing.

Non-goals
- Picture processing (colour, scaling, frame-rate conversion). That is BreadCamera's job.
- v2's WAN tunnel (Host/Join, two-way like NDI Bridge). This design is its foundation, because over the internet the
  tunnel should carry coded frames. It gets its own design later.
- Changing plain OMT's wire format.

Anti-goals
- A pass-through output that a new receiver cannot decode (it joins mid-GOP and never gets a keyframe).
- Breaking today's alpha handling (UYVA, the opaque-alpha gate, premultiplied flags) on decoded routes.
- Needing the NDI Advanced SDK to build or run. Without it, NDI routes decode as today.

## 3. Design

### 3.1 Two lanes per source

A source publishes into one or both lanes, depending on what its outputs and thumbnail need:

- **Coded lane:** `omthx::CodedFrame` (shared, immutable) for outputs of the same family as the source.
- **Picture lane:** decoded frames for everything else, and for the thumbnail.

`SourceHub` already starts a source only when something needs it (demand-driven). It now also tracks which lanes
are wanted.

### 3.2 OMT sources: one coded receiver, decode on demand

- `OmtSource` always opens `omthx::Receiver(address, PixelFormat::Coded)`: one connection.
- **OMT-HX change:** a public decoder, `omthx::FrameDecoder(PixelFormat)`, with
  `bool decode(const CodedFrame&, VideoFrame&)`. It wraps the existing internal `Decoder` for HX (d3d11va) and
  libvmx for VMX, so a coded receiver's frames can be decoded only when the picture lane is wanted. Its output
  layouts are `Uyvy` and `Nv12`, the same meanings as `Receiver`.
- A source with only a thumbnail wanted keeps today's path: OMT-HX's preview stream, or NDI low-bandwidth.

### 3.3 Pass-through outputs

- `OmtSink` takes the coded lane when the output's mode equals the source's (Full → Full, Hx → Hx), and calls
  `Sender::sendCoded`. An HX pass-through keeps the source's codec; the output's codec setting applies only when it
  re-encodes, and the UI says so.
- Keyframes: `Sender::keyframeRequested()` (a receiver joined, or lost a frame) is forwarded to
  `Receiver::requestKeyframe()` on the source, the existing relay mechanism. A new pass-through output asks for one
  at once.

### 3.4 Native layouts on the picture lane

- `Frame` carries `Layout::Uyvy` (today's, limited range, optional alpha plane) or `Layout::Nv12` (full-range
  BT.709, no alpha, as OMT-HX decodes HX).
- What each source decodes to:
  - HX source → NV12 (no CPU unpack).
  - VMX source → UYVY, alpha kept.
  - NDI (standard SDK) → UYVY, as today.
- What each sink takes:
  - NDI sink: UYVY/UYVA, or NV12 directly (`NDIlib_FourCC_video_type_NV12`, no conversion).
  - OMT-HX sink: NV12 via `sendVideoNv12` (no pack), or UYVY via `sendVideoUyvy`.
  - Plain-OMT sink: UYVY, or NV12 only when limited range, so a full-range NV12 picture is converted to UYVY.
- **Convert once per source:** a per-source cache keyed by target layout. The first sink that needs a conversion
  makes it, and the rest share it for that frame.

### 3.5 NDI|HX re-wrapping (optional, NDI Advanced SDK)

- Build flag `OB_WITH_NDI_ADVANCED` (off by default). The owner obtains the SDK and checks its licence terms first.
- NDI source in compressed mode (`NDIlib_recv_color_format_compressed_v…`, per the Advanced SDK's "Receiver Codec
  Support Level"): an H.264/HEVC source fills the coded lane. Packets become Annex-B access units plus parameter
  sets on keyframes, which is OMT-HX's coded format (`codec.h`: "one access unit, Annex B").
- OMT-HX → NDI|HX sink: coded frames are sent as NDI compressed packets. "The SDK will inform you when I-Frame
  insertions are required" → `requestKeyframe` on the source.
- Without the flag, these routes decode and encode as today.

### 3.6 Full NDI → OMT-HX on the GPU (OMT-HX change)

`sendVideoUyvy` on an HX sender whose encoder takes D3D11 frames: upload the UYVY (2 bytes a pixel) and convert to
NV12 on the GPU (the video processor takes YUY2/UYVY input, or a small shader), then encode in place. This removes
the CPU pack. It benefits every OMT-HX user that sends UYVY.

### 3.7 OMT-HX pin

OMT-Bridge builds OMT-HX from its submodule `third_party/OMT-HX` (currently e94ca7a). Each phase that needs new
OMT-HX starts by moving the submodule to OMT-HX `main`: fetch from the local checkout
`F:\Coding\BreadMedia\OMT-HX`, never push. Others cloning from GitHub need OMT-HX pushed first; that is the owner's
step.

## 4. Phases (each a plan)

1. **Coded lane + pass-through** (§3.1–3.3): OMT-HX `FrameDecoder`; OMT-Bridge lanes, `OmtSource` coded, `OmtSink`
   pass-through.
2. **Native layouts + shared conversions** (§3.4).
3. **GPU UYVY → NV12 in OMT-HX** (§3.6).
4. **NDI Advanced re-wrapping** (§3.5), only once the owner has the SDK and is happy with its licence.

Plans (2026-11-03): `OMT-Bridge/docs/superpowers/plans/2026-11-03-bridge-coded-pass-through.md` (1),
`…-bridge-native-layouts.md` (2), `…-omthx-gpu-uyvy.md` (3). Phase 4 waits for the SDK.

Corrections made while planning:
- NDI and plain OMT take limited-range NV12, while OMT-HX decodes to full range. So the picture lane converts
  full → limited once per frame (shared by every sink that needs it), rather than handing NV12 to NDI untouched.
- UYVY is not a DXGI format, so phase 3 writes YUY2 (a byte reorder during the upload copy) for the video
  processor, rather than a shader.

## 5. Verification

- HX → HX pass-through: the receiver's picture is bit-identical to the source's decoded picture (the same bitstream),
  and `stats().encoder` on the bridge output reports pass-through, not an encoder.
- Joining mid-stream: a receiver that connects to a pass-through output gets a decodable picture within about one
  GOP, or sooner when the source honours keyframe requests.
- Decode on demand: a source with only pass-through outputs and no thumbnail never decodes (decoder stats show no
  frames).
- Native layouts: HX → NDI sends NV12 with no CPU conversion. Two HX outputs of one UYVY source convert once per
  frame (a counter).
- Alpha: today's UYVA tests stay green.
- Measured like the BreadCamera audit: bridge process CPU, GPU and memory for 4 routes of each kind, before and
  after.

## 6. Calls made (owner to check)

- One coded connection per source, decoding on demand, rather than opening a second receiver when both lanes are
  needed.
- An HX pass-through keeps the source's codec, so the output's codec setting applies only when it re-encodes.
- NDI Advanced is optional and off by default. Without it, NDI|HX routes decode and encode.
- v2's WAN tunnel is a separate, later design built on the coded lane.
