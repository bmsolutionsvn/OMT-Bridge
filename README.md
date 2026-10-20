# OMT-Bridge

A native Windows tool that takes any mix of OMT, OMT-HX and NDI sources and republishes
them as any mix of OMT, OMT-HX and NDI outputs, converting between them.

v1 is a local converter/proxy; the WAN tunnel (Host/Join) is v2.

- Design: docs/superpowers/specs/2026-10-19-omt-bridge-design.md
- Plan (M0+M1): docs/superpowers/plans/2026-10-19-omt-bridge-m0-m1-foundation.md

## Status

M0–M4 are done. The engine bridges OMT, OMT-HX and NDI in both directions with audio, alpha and
tally. The Qt shell drives it: Sources and Outputs panes, a matrix view, live state and fps/Mbps,
settings, confirm dialogs and the Bridge file.

Run the shell from `build\release`:

```powershell
.\omt-bridge.exe
```

It launches `omt-bridge-engine.exe` when needed, or adopts an engine left running by the previous
session through `%LOCALAPPDATA%\OMTBridge\engine.json`, and saves the bridge to
`%LOCALAPPDATA%\OMTBridge\bridge.json`. The engine stays headless:
`omt-bridge-engine.exe --port <n> [--token <s>] [--bridge <file>] [--discovery <file>]`. It
prints `READY <token>` and answers the WebSocket control channel with `?token=`.

The control commands are `state`, `discover`, `set-running`, `add-source`, `set-source`,
`remove-source`, `add-output`, `set-output`, `set-output-source`, `remove-output` and
`set-settings`.

M5 is next: thumbnails, tally on screen, persistent error blocks and a soak run. NDI remains
optional at build time (`OB_WITH_NDI`, auto-detected; off in CI).

## Build

Needs Windows x64, Visual Studio 2022 or newer, the .NET 8 SDK and git. vcpkg is found from
the Visual Studio install.

```powershell
git clone --recurse-submodules https://github.com/bmsolutionsvn/OMT-Bridge
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test.ps1
```

## Licence

MIT.
