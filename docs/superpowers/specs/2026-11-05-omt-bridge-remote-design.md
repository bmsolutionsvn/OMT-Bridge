# OMT Bridge — Remote Link (Host / Join / Relay) Design

Date: 2026-11-05. Owner decisions are marked **(owner)**; calls the designer made are marked
**(call)** so the owner can check them. This is the "v2 WAN tunnel" that
`2026-10-19-omt-bridge-design.md` §2.1 deferred, built on the coded lane of
`2026-11-03-omt-bridge-data-path-design.md`. Reference: NDI Bridge's Host / Join modes
(installed at `C:\Program Files\NDI\NDI 6 Tools\Bridge\Application.NDI.Bridge.UI.exe`).

## 1. Intent

The owner runs remote esports productions: 4–8 hardware cameras at a venue, captured on PCs,
and a control room (vMix) somewhere else **(owner)**. Today OMT Bridge only works on one LAN.
The remote link joins two Bridges over the internet so that:

- the venue's cameras appear in the studio as ordinary Bridge sources, and
- the studio's feeds (program, multiview, return audio) appear at the venue,

with tally going back to the cameras, **in all directions at once ("connect all")** **(owner)**.

Pass-through is the point: the cameras' own OMT-HX (H.264/HEVC) crosses the internet
byte-for-byte, with **no extra decode or encode and no added latency beyond the transport
buffer** **(owner)**.

Success: a 4–8 camera show runs venue → studio with zero unrecovered drops on a link that has
the upload for it, the delay is SRT's buffer and nothing else, and a connection that drops comes
back on its own while the studio's outputs keep running.

## 2. Goals, non-goals, anti-goals

**Goals**
1. **Evidence first:** an SRT benchmark that proves the transport settings at 10 / 20 / 40 /
   50 Mbps before the Bridge's defaults are fixed (§4) **(owner: "make sure all evident")**.
2. Host / Join between two Bridges, both directions, with tally, audio and keyframe requests (§5).
3. A relay server for shows where neither end can open a port (§6) **(owner)**.
4. Pass-through of OMT-HX by default; re-encoding only where a source cannot cross as it is (§5.4).
5. A per-output quality (and codec) setting for BreadCamera's OMT-HX outputs, so cameras fit the
   upload without any re-encode (§7).

**Non-goals (this round)**
- NAT hole-punching / SRT rendezvous **(call)**. Most venues can open a port **(owner)**; the
  others use the relay. Rendezvous can be added later on top of the relay as an introducer.
- End-to-end encryption past the relay: the relay is the owner's own server and terminates SRT's
  encryption **(call)**.
- Link bonding (several internet lines for one stream), PTZ / KVM control, NDI|HX output (needs
  the NDI Advanced SDK, not in the roadmap **(owner)**), accounts, billing, a hosted service.
- A per-source bitrate cap or warning **(owner: "no need")**.

**Anti-goals (a failure even if it works)**
- Re-encoding an OMT-HX source that could pass through.
- A link problem that stops or stutters any output that does not depend on the link.
- Shipping transport defaults that were not measured.

## 3. Constraints

- Plain OMT (Full / VMX) stays standard on the wire, for vMix.
- Windows engine + Qt UI as today; the engine keeps outputs running with the UI closed.
- SRT is the transport, through **libsrt** (vcpkg `libsrt`, the same library FFmpeg's `srt`
  feature uses), never a Go SRT. The relay is **its own program, separate from Breadstream's
  data plane** **(owner)**; it may run on the same VPS.
- The owner tested SRT and saw loss rise above ~20 Mbps **(owner)**; pass-through at 1080p60 is
  15–50 Mbps per camera. The tuning must be measured, not assumed.
- OpenCode runs only at night and never touches the VPS: anything on the VPS is run by the owner.

## 4. The SRT benchmark (do this first)

- A command-line tool `srt-bench` (Windows and Linux builds) with `send` and `recv` roles. The
  sender paces a synthetic stream that looks like video: 60 bursts per second, a keyframe burst
  4× the average size every 2 s, at a chosen average bitrate.
- Options: bitrate, duration, SRT latency (ms or "×RTT"), send / receive buffer and flow-control
  window, payload size, overhead bandwidth %, encryption on/off; and `--via <relay>` to go through
  the relay (§6).
- Every second it logs SRT's own stats (`srt_bstats`): sent, retransmitted, lost, dropped
  (unrecovered), RTT, send / receive buffer fill, estimated link bandwidth; at the end a summary
  row. Output is CSV plus one Markdown table.
- The run the owner does: from the venue-type PC to the VPS and back, and through the relay, at
  10 / 20 / 40 / 50 Mbps, defaults versus tuned, 10 minutes each. **Pass bar:** zero unrecovered
  drops at 40 Mbps for 10 minutes, tuned, on both paths. The winning settings become the Bridge's
  defaults, and the table is committed next to this design.
- If 40 Mbps cannot pass on the real path with any setting, this design is reviewed before the
  Bridge work starts (the fallback is §7's lower camera quality, not re-encoding).

## 5. Host / Join

### 5.1 Roles

- **Host** (usually the studio): listens on one UDP port, has a **Bridge name** and a **key**. It
  may accept several Joins at once **(call)**.
- **Join** (usually the venue): enters the Host's address, port and key; only dials out.
- The Host's UDP port needs a Windows Firewall rule: the Bridge adds it when the operator turns
  Host on (one UAC prompt), and says so if it cannot **(call)**.
- Either side may be either role; what flows does not depend on who listens.
- The key is the SRT passphrase (AES-256) for a direct link, and proves the pairing in the
  control stream on the relay (§6).

### 5.2 Streams on the link

- **One SRT connection per shared source in each direction** (live mode), all on the one port,
  told apart by SRT's stream ID: `bmb/<session>/<from-bridge>/<source-id>`.
- **One control connection** (SRT message mode, stream ID `bmb/<session>/<bridge>/control`):
  hello and key proof, the shared-source list, subscribe / unsubscribe, tally, keyframe requests,
  link stats, small thumbnails and Peek requests (§5.3).
- A source's stream carries `omthx::CodedFrame`s as they are (codec, timestamps, keyframe flag,
  data), plus its audio as received (OMT PCM) and its metadata. One SRT message per frame part;
  the receiver rebuilds frames and timestamps.

### 5.3 Remote sources in the bridge graph

- Each side picks which of its sources it **shares** (a checkbox per source in a Remote panel).
- A shared source appears on the other side as a normal source named
  `<Bridge name> (<source name>)` with a link badge, routable to any OMT / OMT-HX / NDI output as
  today.
- **The receiving Bridge advertises every shared source on its own network by itself**
  **(owner)**: an OMT sender of the same mode (OMT-HX stays OMT-HX, passed through again), named
  `<Bridge name> (<source name>)`, published by DNS-SD like any OMT sender, so vMix and other
  receivers there see it without anyone routing it by hand. Until a receiver connects it sends
  nothing.
- **Only names cross until someone receives** **(owner)**. The sharing side learns its sources
  by DNS-SD (names only, no connection, so cameras stay idle) and sends the list over the
  control stream. No video and no thumbnail of an unused source crosses.
- **Demand-driven, like `SourceHub`:** a remote source is streamed only while the other side has
  a consumer: a receiver connected to its advertised sender, or an output routed to it. The first
  consumer sends a subscribe; the sharing side then connects to the source (the camera starts its
  encode) and asks it for a keyframe. When the last consumer leaves, the receiving side waits 5 s
  (a quick reconnect does not restart the stream) and unsubscribes; the sharing side disconnects
  and the camera goes idle **(call: the 5 s)**. Note: vMix stays connected to every input in its
  project, so every camera added to vMix streams all the time.
- **Thumbnails never wake a camera** **(owner)**, following NDI, whose every preview is a real
  connection (its low-bandwidth "proxy" stream) and whose Bridge sends names only:
  - a source already streaming at the sharing side (consumed there or over the link) sends a
    small JPEG every 2 s from frames it already has (one hardware keyframe decode for OMT-HX),
    and only while the other side has its Remote panel open;
  - an idle source shows its name and "idle", with a **Peek** button: Peek connects for about
    3 s, takes one picture (plain OMT uses libomt's `OMTReceiveFlags_Preview`, the 1/8 VMX
    preview), disconnects, and the camera goes idle again **(call: the 3 s)**.
- **Tally** follows the existing rule (a source is on air when an output it feeds is on air) and
  crosses the link back to the sharing side, so the camera's tally lights at the venue. Program
  and preview both cross, as OMT carries both **(call)**.
- Two Joins with the same Bridge name on one Host: the second is refused with that reason
  **(call)**.
- **Keyframes:** when the far side joins mid-stream, or a frame is lost beyond recovery, its
  `Sender::keyframeRequested()` / decoder error becomes a control-stream request, which the
  sharing side forwards to `Receiver::requestKeyframe()` on the source, as on the LAN. The picture
  recovers within one keyframe.

### 5.4 Pass-through and the few re-encodes

| Source on the sharing side | Over the link |
|---|---|
| OMT-HX (H.264 / HEVC) | pass-through, always |
| Full OMT (VMX) | re-encoded to HEVC at a bitrate set on that share, labelled `re-encoded` |
| NDI (Full or HX) | re-encoded (the standard NDI SDK hands over decoded pictures), labelled |

On the receiving side, an output of the same family passes through again (OMT-HX out). An output
that only speaks Full OMT — vMix — needs one decode + VMX encode on the studio Bridge, exactly as
on the LAN today; the UI already says so per output.

### 5.5 Connection life

- States shown in the Remote panel: connecting, connected (direct or via relay), reconnecting,
  failed (with the reason: wrong key, no route, port closed).
- Per link: RTT, upload and download Mbps, and per stream retransmits and unrecovered drops.
  Upload is shown against the link bandwidth SRT estimates, as information.
- A dropped link reconnects on its own (backoff 1, 2, 4… up to 10 s). Remote sources go offline
  the way any lost source does (outputs keep running and show what they show for a lost source);
  when the link returns, each consumed source asks for a keyframe.
- SRT latency defaults to the benchmark's rule (expected: about 4 × RTT, never below 120 ms),
  editable per link. Nothing else adds delay.

## 6. The relay

- A small Linux program `bmb-relay` (C++, libsrt), shipped as a Docker image and compose file
  the owner runs on a VPS (beside `deploy/dp-sgp-1`, or anywhere) **(call: C++ so it shares the
  Bridge's stream-ID and framing code)**.
- One UDP port, an SRT passphrase and a **relay token** in its environment. A Bridge in relay
  mode dials out to it with the token in its stream ID; the relay pairs the two sides of a
  session and forwards each stream's packets to the other side without parsing the media.
- The relay never needs the session key; the Bridges prove the key to each other through the
  control stream **(call)**.
- Bandwidth: a session costs the VPS its bitrate twice (in and out). The relay logs per-session
  throughput and refuses a new session above a configured total (default unlimited).
- Each Bridge chooses **Direct** or **Relay** per link **(call: no automatic fallback in v1;
  the operator knows which venues can open a port)**.

## 7. Making cameras fit the upload without re-encoding

OMT-HX's bitrate follows its quality level. At 1080p60:

| Camera encoder | Low | Medium | High |
|---|---|---|---|
| HEVC | 15 Mbps | 30 Mbps | 50 Mbps |
| H.264 | 21 Mbps | 42 Mbps | 70 Mbps |

8 cameras at HEVC Medium need about 300 Mbps of upload with SRT's headroom; at HEVC Low about
150 Mbps. BreadCamera, the venue sender this is built and tested with **(call: the owner has no
venue sender yet and will test with the finished stack)**, fixes every OMT-HX output at Medium
today. This design adds to BreadCamera's OMT-HX output a **quality** (Low / Medium / High) and a
**codec** (HEVC / H.264) setting, saved with the output; the encoder is the one it already runs,
so it adds no delay.

## 8. Left to the builder

- How to split into plans. A sensible order: (1) `srt-bench` + the relay's pairing core (so the
  owner can run §4); (2) BreadCamera's quality / codec setting (independent, any time);
  (3) Host / Join direct mode with remote sources, tally and keyframes; (4) relay mode and the
  Remote panel's stats. Plans (3)–(4) wait for the §4 numbers.
- The exact control-message format (JSON over SRT message mode is fine), framing of frame parts,
  and how audio is chunked.
- The Remote panel's layout, within the Bridge's existing Qt style.
- Tests: two Bridge engines on one PC linked over loopback (direct and through a local relay),
  OMT-HX in → remote → OMT-HX out byte-identical frames; tally round trip; keyframe recovery after
  a forced drop; reconnect after killing the link; demand-driven start / stop; wrong key refused;
  a shared source is advertised on the far side and a receiver connecting to it starts the
  stream; an idle source's thumbnail and the list never connect to the source; Peek connects
  once and lets go.
