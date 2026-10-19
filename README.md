# OMT-Bridge

A native Windows tool that takes any mix of OMT, OMT-HX and NDI sources and republishes
them as any mix of OMT, OMT-HX and NDI outputs, converting between them.

v1 is a local converter/proxy; the WAN tunnel (Host/Join) is v2.

- Design: docs/superpowers/specs/2026-10-19-omt-bridge-design.md
- Plan (M0+M1): docs/superpowers/plans/2026-10-19-omt-bridge-m0-m1-foundation.md

## Build

Needs Windows x64, Visual Studio 2022 or newer, the .NET 8 SDK and git. vcpkg is found from
the Visual Studio install.

```powershell
git clone --recurse-submodules https://github.com/bmsolutionsvn/OMT-Bridge
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test.ps1
```

## Licence

MIT.
