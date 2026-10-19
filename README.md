# OMT-Bridge

A native Windows tool that takes any mix of OMT, OMT-HX and NDI sources and republishes
them as any mix of OMT, OMT-HX and NDI outputs, converting between them.

v1 is a local converter/proxy; the WAN tunnel (Host/Join) is v2.

- Design: docs/superpowers/specs/2026-10-19-omt-bridge-design.md
- Plan (M0+M1): docs/superpowers/plans/2026-10-19-omt-bridge-m0-m1-foundation.md

## Status

M0 and M1 are done: the build foundation (CMake, vcpkg, CI, the submodule), a UI-free core
(frame type, signal states, a drop-oldest queue, the Bridge model and its JSON store), and the
engine skeleton. The engine has a refcounted source hub, isolated per-output rows, a reconciling
graph, and a token-authenticated loopback control server with five commands; it runs headless
and keeps running when the UI closes.

Run it with `omt-bridge-engine.exe --port <n> [--token <s>] [--bridge <file>]`. It prints
`READY <token>` and pushes a `state` reply when an authorised client connects; pass the token as
`?token=` on the WebSocket URL.

The Qt UI is a status-bar stub. Nothing bridges frames yet: no OMT, OMT-HX or NDI adapter exists.
NDI is optional at build time (`OB_WITH_NDI`, auto-detected; off in CI). The supervisor and
process-launcher code is present but not wired in until M2.

## Build

Needs Windows x64, Visual Studio 2022 or newer, the .NET 8 SDK and git. vcpkg is found from
the Visual Studio install.

```powershell
git clone --recurse-submodules https://github.com/bmsolutionsvn/OMT-Bridge
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test.ps1
```

## Licence

MIT.
