# OMT Bridge M0+M1 (Foundation and Engine Core) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the `OMT-Bridge` repository and its headless engine core — two processes, a token-authenticated loopback control channel, N sources × M outputs wired through reference-counted source hubs and drop-oldest output rows, with a CI build — so that M2 onwards can add real OMT/OMT-HX/NDI I/O to a working engine.

**Architecture:** `OMT-Bridge-Engine.exe` is headless and owns the bridge graph; `OMT-Bridge.exe` is a Qt Widgets UI that supervises it and talks to it over a loopback WebSocket carrying a random token. The engine has one `SourceHub` per Source and one `OutputRow` per Output; an OutputRow holds a drop-oldest queue and a worker thread. There is no master tick — pacing belongs to each source. The canonical frame is 8-bit 4:2:2 `UYVY`/`UYVA`.

**Tech Stack:** C++20, CMake 3.28+, Ninja, vcpkg (`qtbase`, `ffmpeg`, `ixwebsocket`, `nlohmann-json`, `doctest`), OMT-HX as a git submodule, NDI 6 SDK (optional, off in CI), doctest.

**Spec:** `docs/superpowers/specs/2026-10-19-omt-bridge-design.md`. Workspace-relative sibling checkouts: `OMT-HX`, `BreadCamera` (the source of lifted modules).

> **Prerequisite for every task (amended 2026-10-19):** run every build and test through `scripts/test.ps1` (or `scripts/build.ps1`). Those scripts activate the MSVC developer shell and set `VCPKG_ROOT` themselves, because this machine has **no developer shell on PATH** by default — `cmake`, `ninja` and `VCPKG_ROOT` are only present inside one. The `cmake --preset release` lines shown inside each task are exactly what those scripts run; **run the script, not the raw lines**. All paths are relative to the `OMT-Bridge` repository root unless they start with a sibling repo name (for example `..\BreadCamera`).
>
> **Every file you create or edit must end with a single trailing newline** (`.gitattributes` sets `* text=auto eol=lf`). Reviewers flag its absence and it has recurred across tasks. For a **newly added** file, check with `git show <sha> | Select-String "No newline"` returning nothing. (On a commit that *adds* a missing newline, the marker appears on the removed side of the diff and is expected — verify such a file's last byte instead.)
>
> **doctest stringifies every `CHECK` operand, and that has bitten three times.** Before writing an assertion, ask what doctest will try to stream:
> - **An enum with an `ob::toString` overload.** `CHECK(*back == k)` on two `ob::SourceKind` values makes ADL pick `ob::toString(SourceKind)`, which returns `std::string_view`; `string_view + const char*` then fails to compile. Compare `static_cast<int>(...)`, or wrap in `std::string(...)` when comparing to a literal.
> - **`std::shared_ptr<T>`.** doctest streams it through the standard `operator<<`, whose body does `os << p.get()` — so it needs an `operator<<` for `T*`/`T`, which a plain struct like `ob::Frame` does not have. Write `CHECK_FALSE(static_cast<bool>(ptr))`, `REQUIRE(ptr.get() != nullptr)`, and compare through the pointee (`ptr->field`) rather than the pointer itself.
> - **A `std::string_view` compared to a string literal** — wrap the left side in `std::string(...)`.
>
> When in doubt, assert on a `bool`, an `int`, or a `std::string`, never on a struct, an enum, or a smart pointer.

---

## File structure

| Path | Responsibility |
|---|---|
| `CMakeLists.txt` | Project, optional SDK discovery, targets: `ob_core`, `ob_engine`, `ob_shell`, `ob_ui`, `omt-bridge-engine`, `omt-bridge`, `ob_tests` |
| `CMakePresets.json` | `release` / `debug` presets, Ninja + vcpkg toolchain |
| `vcpkg.json` | Manifest dependencies |
| `cmake/FindNDI.cmake` | Finds the installed NDI 6 SDK (lifted from BreadCamera) |
| `src/core/frame.h` | The canonical `Frame` and `VideoFormat` |
| `src/core/signal_state.h/.cpp` | Four signal states + `ErrorInfo` (lifted) |
| `src/core/bounded_queue.h` | Drop-oldest queue (lifted) |
| `src/core/bridge_model.h/.cpp` | `Bridge`, `Source`, `Output` data model |
| `src/core/bridge_store.h/.cpp` | Bridge JSON save/load |
| `src/engine/source_hub.h/.cpp` | One source: latest frame + reference counting |
| `src/engine/output_row.h/.cpp` | One output: queue + worker + drop accounting |
| `src/engine/bridge_graph.h/.cpp` | Reconciles the model into hubs and rows |
| `src/engine/control_server.h/.cpp` | Loopback WebSocket control API (lifted) |
| `src/engine/command_router.h/.cpp` | JSON command validation + dispatch |
| `src/engine/engine.h/.cpp` | Composes graph, store, control server |
| `src/engine/engine_main.cpp` | `omt-bridge-engine.exe` entry point |
| `src/shell/supervisor.h/.cpp` | Engine restart policy (lifted) |
| `src/shell/process_launcher.h/.cpp` | Launches the engine (lifted) |
| `src/ui/main.cpp`, `src/ui/main_window.h/.cpp` | Qt Widgets window |
| `tests/main.cpp` | doctest main |
| `tests/*_test.cpp` | Unit tests, co-located by subject |
| `.github/workflows/build.yml` | CI: configure, build, test |

**Namespace:** everything is in `namespace ob` (OMT Bridge). Lifted files change only their namespace from `bc` to `ob`.

---

## Task 1: Repository skeleton

**Files:**
- Create: `.gitignore`, `.gitattributes`, `README.md`

- [ ] **Step 1: Write `.gitignore`**

```gitignore
build/
out/
.vs/
*.user
vcpkg_installed/
```

- [ ] **Step 2: Write `.gitattributes`**

```gitattributes
* text=auto eol=lf
*.png binary
*.ico binary
```

- [ ] **Step 3: Write `README.md`**

```markdown
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
```

- [ ] **Step 4: Write `scripts/build.ps1`**

Adapted from `OMT-HX/scripts/build.ps1`. It exists so a build never depends on the caller having started a developer shell.

```powershell
param([ValidateSet('release', 'debug')][string]$Preset = 'release')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
git -C $root submodule update --init
if ($LASTEXITCODE) { exit $LASTEXITCODE }
# The developer shell calls vswhere.exe by name, so its folder must be on PATH.
$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$env:PATH = "$installer;$env:PATH"
$vs = & "$installer\vswhere.exe" -latest -products * -property installationPath
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = "$vs\VC\vcpkg" }
Set-Location $root
cmake --preset $Preset
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build --preset $Preset
exit $LASTEXITCODE
```

- [ ] **Step 5: Write `scripts/test.ps1`**

```powershell
param(
  [ValidateSet('release', 'debug')][string]$Preset = 'release',
  [string]$Filter = ''
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1') -Preset $Preset
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Set-Location (Join-Path $PSScriptRoot "..\build\$Preset")
if ($Filter) { & .\ob_tests.exe "--test-case=$Filter" } else { & .\ob_tests.exe }
exit $LASTEXITCODE
```

- [ ] **Step 6: Commit**

```bash
git add .gitignore .gitattributes README.md scripts
git commit -m "chore: repository skeleton and build scripts"
```

---

## Task 2: Build system and the first test

**Files:**
- Create: `CMakePresets.json`, `vcpkg.json`, `CMakeLists.txt`, `src/core/frame.h`, `tests/main.cpp`, `tests/frame_test.cpp`

- [ ] **Step 1: Write `vcpkg.json`**

```json
{
  "name": "omt-bridge",
  "version-string": "0.1.0",
  "dependencies": [
    {
      "name": "ffmpeg",
      "default-features": false,
      "features": ["avcodec", "avformat", "swresample", "swscale", "srt", "nvcodec", "amf"]
    },
    "qtbase",
    "ixwebsocket",
    "nlohmann-json",
    "doctest"
  ],
  "builtin-baseline": "2750401336fb7c95f6619657a46a7e798661341c"
}
```

- [ ] **Step 2: Write `CMakePresets.json`**

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "base",
      "hidden": true,
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "toolchainFile": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": { "VCPKG_TARGET_TRIPLET": "x64-windows" }
    },
    { "name": "release", "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo" } },
    { "name": "debug", "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } }
  ],
  "buildPresets": [
    { "name": "release", "configurePreset": "release" },
    { "name": "debug", "configurePreset": "debug" }
  ]
}
```

- [ ] **Step 3: Write the failing test `tests/frame_test.cpp`**

```cpp
#include <doctest/doctest.h>

#include "core/frame.h"

TEST_CASE("a frame reports its plane sizes") {
  ob::Frame f;
  f.format.width = 1920;
  f.format.height = 1080;
  f.format.alpha = false;
  CHECK(f.lumaBytes() == 1920u * 2u * 1080u);
  CHECK(f.alphaBytes() == 0u);

  f.format.alpha = true;
  CHECK(f.alphaBytes() == 1920u * 1080u);
}

TEST_CASE("alphaOffset points past the luma plane and is null when there is no alpha") {
  ob::Frame f;
  f.format = ob::VideoFormat{4, 2, 60, 1, true};
  CHECK(f.alphaOffset() == 4u * 2u * 2u);
  f.format.alpha = false;
  CHECK(f.alphaOffset() == ob::Frame::kNoAlpha);
}

TEST_CASE("valid() accepts an UYVY picture of the right size and rejects others") {
  ob::Frame f;
  f.format = ob::VideoFormat{4, 2, 60, 1, false};
  f.uyvy.assign(f.lumaBytes(), 0x10);
  CHECK(f.valid());

  f.uyvy.assign(f.lumaBytes() - 1, 0x10);
  CHECK_FALSE(f.valid());

  f.format.width = 3;  // odd width is not UYVY
  f.uyvy.assign(4096, 0x10);
  CHECK_FALSE(f.valid());
}
```

- [ ] **Step 4: Write `tests/main.cpp`**

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```

- [ ] **Step 5: Write `src/core/frame.h`**

```cpp
#pragma once
// The engine's one frame type: 8-bit 4:2:2 YUV, UYVY, or UYVA when alpha is present.
// Design 6.3. Luma and chroma are limited range (16-235); the alpha plane is full range.
// The layout matches OMT's and NDI's UYVA exactly: the UYVY plane, then the alpha plane.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ob {

struct VideoFormat {
  int width = 0;
  int height = 0;
  int fpsN = 60;
  int fpsD = 1;
  bool alpha = false;
  bool operator==(const VideoFormat&) const = default;
};

struct Frame {
  VideoFormat format;
  bool premultiplied = false;
  int64_t timestamp = 0;      // OMT units: 10,000,000 per second
  std::vector<uint8_t> uyvy;  // luma plane, then the alpha plane when format.alpha

  static constexpr size_t kNoAlpha = static_cast<size_t>(-1);

  size_t lumaBytes() const {
    return static_cast<size_t>(format.width) * 2u * static_cast<size_t>(format.height);
  }
  size_t alphaBytes() const {
    return format.alpha ? static_cast<size_t>(format.width) * static_cast<size_t>(format.height) : 0u;
  }
  size_t alphaOffset() const { return format.alpha ? lumaBytes() : kNoAlpha; }

  // A UYVY picture needs an even width, both dimensions at least 2, and exactly the right bytes.
  bool valid() const {
    if (format.width < 2 || format.height < 2 || format.width % 2 != 0) return false;
    return uyvy.size() == lumaBytes() + alphaBytes();
  }
};

}  // namespace ob
```

- [ ] **Step 6: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.28)
project(OMTBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(/W4 /permissive- /utf-8 /EHsc)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

find_package(doctest CONFIG REQUIRED)

# ---- tests ----
# frame.h is header-only, so nothing links yet; ob_core is created in Task 6.
add_executable(ob_tests
  tests/main.cpp
  tests/frame_test.cpp
)
target_include_directories(ob_tests PRIVATE tests src)
target_link_libraries(ob_tests PRIVATE doctest::doctest)
```

- [ ] **Step 7: Build and run the test to verify it passes**

Run:
```powershell
cmake --preset release
cmake --build --preset release
.\build\release\ob_tests.exe
```
Expected: `[doctest] Status: SUCCESS!`, all three test cases pass.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt CMakePresets.json vcpkg.json src/core/frame.h tests/main.cpp tests/frame_test.cpp
git commit -m "build: CMake, vcpkg and the canonical Frame"
```

---

## Task 3: Link OMT-HX as a submodule

**Files:**
- Create: `.gitmodules` (via `git submodule add`), `cmake/omt-runtime.cmake`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add the submodule pinned at the reviewed commit**

Run:
```powershell
git submodule add https://github.com/bmsolutionsvn/OMT-HX third_party/OMT-HX
git -C third_party/OMT-HX fetch --all
git -C third_party/OMT-HX checkout ae96aa1
git -C third_party/OMT-HX submodule update --init --recursive
```
Expected: `third_party/OMT-HX/include/omthx/omthx.h` exists **and** it declares `void sendVideoUyvy(` and `enum class PixelFormat { Bgra, Uyvy, Gpu, Coded };`. If either is missing, the wrong commit was checked out.

- [ ] **Step 2: Verify the pinned commit is at or after `ae96aa1`**

Run:
```powershell
git -C third_party/OMT-HX merge-base --is-ancestor ae96aa1 HEAD; echo $LASTEXITCODE
```
Expected: `0` (ae96aa1 is an ancestor of HEAD).

- [ ] **Step 3: Write `cmake/omt-runtime.cmake`**

```cmake
# OMT-HX is a submodule under third_party/; OMTHX_ROOT lets a developer point at a sibling
# checkout instead, exactly like BreadCamera does.
include_guard(GLOBAL)
set(OMTHX_ROOT "${CMAKE_SOURCE_DIR}/third_party/OMT-HX" CACHE PATH "OMT-HX checkout")
if(EXISTS "${OMTHX_ROOT}/include/omthx/omthx.h" AND EXISTS "${OMTHX_ROOT}/omt/libomt/libomt.h")
  add_subdirectory("${OMTHX_ROOT}" omthx-build EXCLUDE_FROM_ALL)
  set(OB_WITH_OMT ON)
else()
  set(OB_WITH_OMT OFF)
  message(FATAL_ERROR "OMT-HX is missing at ${OMTHX_ROOT}. Run: git submodule update --init --recursive")
endif()
```

- [ ] **Step 4: Add the runtime copy function and the FFmpeg/omthx dependency to `CMakeLists.txt`**

Insert after `find_package(doctest CONFIG REQUIRED)`:

```cmake
# FFmpeg comes from vcpkg; its ffmpeg package provides FindFFMPEG.cmake, so no module is vendored.
find_package(FFMPEG REQUIRED)
include(cmake/omt-runtime.cmake)  # defines the omthx target and omthx_copy_runtime()

# Copies the DLLs vcpkg does not manage (libomt, libvmx, and NDI when enabled) beside an exe.
function(ob_copy_runtime target)
  if(OB_WITH_OMT)
    omthx_copy_runtime(${target})
    # omthx_copy_runtime's POST_BUILD copy reads libomt.dll (built by the libomt_build custom
    # target) and libvmx.dll. A target linking omthx inherits those dependencies; one that does
    # not (e.g. a stub exe) would race them on a clean build and fail the copy. Depend on them
    # explicitly. The TARGET guards keep this valid if OMT-HX is absent or renames its targets.
    if(TARGET libomt_build)
      add_dependencies(${target} libomt_build)
    endif()
    if(TARGET libvmx)
      add_dependencies(${target} libvmx)
    endif()
  endif()
endfunction()
```

- [ ] **Step 5: Prove the link works with a compile-time check**

Add to `tests/frame_test.cpp`:

```cpp
#include "omthx/omthx.h"

TEST_CASE("the linked omthx is the UYVY build") {
  // sendVideoUyvy and PixelFormat::Coded only exist at OMT-HX ae96aa1 or later (spec 6.3, 18.8).
  static_assert(static_cast<int>(omthx::PixelFormat::Coded) >= 0);
}
```

Add to the `ob_tests` target in `CMakeLists.txt`:

```cmake
target_link_libraries(ob_tests PRIVATE omthx)
ob_copy_runtime(ob_tests)
```

- [ ] **Step 6: Build and run**

Run:
```powershell
cmake --preset release
cmake --build --preset release
.\build\release\ob_tests.exe
```
Expected: SUCCESS. `build/release/libomt.dll` and `build/release/libvmx.dll` exist beside `ob_tests.exe`.

- [ ] **Step 7: Commit**

```bash
git add .gitmodules third_party/OMT-HX cmake/omt-runtime.cmake CMakeLists.txt tests/frame_test.cpp
git commit -m "build: link OMT-HX at ae96aa1 and copy its runtime DLLs"
```

---

## Task 4: NDI 6 SDK discovery, off by default in CI

**Files:**
- Create: `cmake/FindNDI.cmake`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Copy `FindNDI.cmake` from BreadCamera**

Run:
```powershell
Copy-Item ..\BreadCamera\cmake\FindNDI.cmake cmake\FindNDI.cmake
```

- [ ] **Step 2: Confirm it targets the installed SDK, not a vendored copy**

Run:
```powershell
Select-String -Path cmake\FindNDI.cmake -Pattern "NDI_SDK_DIR|NDI 6 SDK|Processing.NDI.Lib.x64"
```
Expected: three matches. If the file instead includes a vendored `ndi-sdk` header directory, you copied the wrong pattern — BreadStudio's copy is headers only and cannot be linked (spec 14.3).

- [ ] **Step 3: Add the option to `CMakeLists.txt`**

Insert before the `# ---- tests` block:

```cmake
# Optional NDI 6 SDK (spec 14.3). Absent in CI, so OB_WITH_NDI defaults to whatever is found.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(NDI)
option(OB_WITH_NDI "Build the NDI adapters" ${NDI_FOUND})
if(OB_WITH_NDI AND NOT NDI_FOUND)
  message(WARNING "OB_WITH_NDI=ON but the NDI 6 SDK was not found; turning it off")
  set(OB_WITH_NDI OFF)
endif()
message(STATUS "OB_WITH_NDI=${OB_WITH_NDI}")
```

- [ ] **Step 4: Extend `ob_copy_runtime` for NDI**

```cmake
function(ob_copy_runtime target)
  if(OB_WITH_OMT)
    omthx_copy_runtime(${target})
    # See Task 3: without this, a target that does not link omthx races libomt.dll's build.
    if(TARGET libomt_build)
      add_dependencies(${target} libomt_build)
    endif()
    if(TARGET libvmx)
      add_dependencies(${target} libvmx)
    endif()
  endif()
  if(OB_WITH_NDI)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NDI_DLL}" "$<TARGET_FILE_DIR:${target}>")
  endif()
endfunction()
```

- [ ] **Step 5: Configure both ways and confirm**

Run:
```powershell
cmake --preset release
```
Expected: the configure output contains `OB_WITH_NDI=ON` when the NDI 6 SDK is installed, `OB_WITH_NDI=OFF` when it is not. Both must configure cleanly.

Run:
```powershell
cmake --preset release -DOB_WITH_NDI=OFF
```
Expected: configures and builds with NDI off.

- [ ] **Step 6: Commit**

```bash
git add cmake/FindNDI.cmake CMakeLists.txt
git commit -m "build: optional NDI 6 SDK, off when not installed"
```

---

## Task 5: Two executables and CI

**Files:**
- Create: `src/engine/engine_main.cpp`, `src/ui/main.cpp`, `src/ui/main_window.h`, `src/ui/main_window.cpp`, `.github/workflows/build.yml`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write `src/engine/engine_main.cpp` (a placeholder engine)**

```cpp
// OMT-Bridge-Engine.exe: headless. Task 15 replaces this body with the control server and graph.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
  int port = 0;
  std::string token;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    if (k == "--port") port = std::atoi(argv[i + 1]);
    else if (k == "--token") token = argv[i + 1];
  }
  if (port == 0 || token.empty()) {
    std::fprintf(stderr, "usage: omt-bridge-engine --port <n> --token <s>\n");
    return 2;
  }
  std::printf("OMT Bridge engine listening on 127.0.0.1:%d\n", port);
  std::fflush(stdout);
  // Placeholder: exit cleanly so the shell's supervisor can be exercised.
  for (;;) {
    std::string line;
    if (!std::getline(std::cin, line)) break;
    if (line == "quit") break;
  }
  return 0;
}
```

- [ ] **Step 2: Write `src/ui/main_window.h`**

```cpp
#pragma once
#include <QMainWindow>
#include <QLabel>

namespace ob {
class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget* parent = nullptr);
 private:
  QLabel* engine_state_ = nullptr;
};
}  // namespace ob
```

- [ ] **Step 3: Write `src/ui/main_window.cpp`**

```cpp
#include "ui/main_window.h"

#include <QStatusBar>

namespace ob {
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("OMT Bridge"));
  engine_state_ = new QLabel(QStringLiteral("Engine: stopped"), this);
  statusBar()->addWidget(engine_state_);
  resize(900, 560);
}
}  // namespace ob
```

- [ ] **Step 4: Write `src/ui/main.cpp`**

```cpp
#include <QApplication>
#include "ui/main_window.h"

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  ob::MainWindow w;
  w.show();
  return app.exec();
}
```

- [ ] **Step 5: Add the Qt and executable targets to `CMakeLists.txt`**

Append:

```cmake
# ---- shells ----
add_executable(omt-bridge-engine src/engine/engine_main.cpp)
ob_copy_runtime(omt-bridge-engine)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
set(CMAKE_AUTOMOC ON)
add_executable(omt-bridge WIN32 src/ui/main.cpp src/ui/main_window.cpp src/ui/main_window.h)
target_include_directories(omt-bridge PRIVATE src)
target_link_libraries(omt-bridge PRIVATE Qt6::Widgets)

# Deploy Qt's platform plugin beside the executable. Without it Qt6Core falls back to the plugin
# path baked into this vcpkg build, so the UI starts only on the build machine; copied anywhere
# else it dies with "This application failed to start because no Qt platform plugin could be
# initialized" (found in testing, after the merge). vcpkg's Qt does not export a plugin target, so
# the file is located from Qt6_DIR: <prefix>/share/Qt6 -> <prefix>/Qt6/plugins/platforms.
get_filename_component(_ob_qt_prefix "${Qt6_DIR}/../.." ABSOLUTE)
set(_ob_qwindows "${_ob_qt_prefix}/Qt6/plugins/platforms/qwindows.dll")
if(EXISTS "${_ob_qwindows}")
  add_custom_command(TARGET omt-bridge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:omt-bridge>/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_ob_qwindows}"
            "$<TARGET_FILE_DIR:omt-bridge>/platforms")
else()
  message(WARNING "qwindows.dll not found under ${_ob_qt_prefix}: the UI will only start where Qt is installed")
endif()
```

`target_include_directories(omt-bridge PRIVATE src)` is required: both `src/ui/main.cpp` and `main_window.cpp` `#include "ui/main_window.h"`, a project-root-relative path MSVC cannot resolve from `src/ui/` alone. It matches what `ob_tests` already does. It may become redundant once Task 6 links `ob_core` (whose `src` include is `PUBLIC`); leaving it is harmless.

- [ ] **Step 6: Write `.github/workflows/build.yml`**

```yaml
# Configure, build and test OMT Bridge on every push and pull request.
# NDI is not installed on the runner, so OB_WITH_NDI resolves to OFF (spec 14.4).
name: Build

on:
  push:
    branches: [main]
  pull_request:
  workflow_dispatch:

permissions:
  contents: read

jobs:
  windows-x64:
    runs-on: windows-2025
    timeout-minutes: 120  # a cold vcpkg cache builds ffmpeg and qtbase from source
    env:
      VCPKG_DEFAULT_BINARY_CACHE: ${{ github.workspace }}\..\vcpkg-cache
    steps:
      # recursive: OMT-HX has its own submodules (omt/libomt, omt/libvmx, omt/libomtnet);
      # plain `true` is `git submodule update --init` without --recursive and leaves them empty.
      - name: Check out OMT-Bridge and its submodules
        uses: actions/checkout@v4
        with:
          submodules: recursive

      - uses: actions/setup-dotnet@v4
        with:
          dotnet-version: 8.0.x

      - uses: ilammy/msvc-dev-cmd@0b201ec74fa43914dc39ae48a89fd1d8cb592756 # v1.13.0
        with:
          arch: x64

      - name: Point at the runner's vcpkg
        shell: pwsh
        run: |
          New-Item -ItemType Directory -Force $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
          git -C $env:VCPKG_INSTALLATION_ROOT fetch -q
          "VCPKG_ROOT=$env:VCPKG_INSTALLATION_ROOT" >> $env:GITHUB_ENV

      - name: Cache vcpkg packages
        uses: actions/cache@v4
        with:
          path: ${{ env.VCPKG_DEFAULT_BINARY_CACHE }}
          key: vcpkg-${{ runner.os }}-x64-windows-${{ hashFiles('vcpkg.json') }}
          restore-keys: vcpkg-${{ runner.os }}-x64-windows-

      - name: Configure
        run: cmake --preset release -DOB_WITH_NDI=OFF

      - name: Build
        run: cmake --build --preset release

      - name: Test
        working-directory: build/release
        run: .\ob_tests.exe
```

- [ ] **Step 7: Build locally and confirm both executables run**

Run:
```powershell
cmake --preset release
cmake --build --preset release
.\build\release\omt-bridge-engine.exe --port 5990 --token t
```
Expected: prints `OMT Bridge engine listening on 127.0.0.1:5990`, then exits on EOF/`quit`.

Run:
```powershell
.\build\release\omt-bridge.exe
```
Expected: a window titled `OMT Bridge` with a status bar reading `Engine: stopped`. Close it.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt src/engine/engine_main.cpp src/ui .github/workflows/build.yml
git commit -m "build: engine and UI executables, CI"
```

**M0 is complete when:** CI is green, `omt-bridge.exe` opens, and `omt-bridge-engine.exe --port N --token T` runs.

---

## Task 6: Signal state and the lift pattern

**Files:**
- Create: `src/core/signal_state.h`, `src/core/signal_state.cpp`, `tests/signal_state_test.cpp`
- Modify: `CMakeLists.txt`, `tests/` list

- [ ] **Step 1: Copy the header from BreadCamera**

Run:
```powershell
Copy-Item ..\BreadCamera\src\core\signal_state.h src\core\signal_state.h
Copy-Item ..\BreadCamera\src\core\signal_state.cpp src\core\signal_state.cpp
```

- [ ] **Step 2: Change the namespace `bc` to `ob`**

Run:
```powershell
(Get-Content src\core\signal_state.h)  -replace '\bbc\b','ob' | Set-Content src\core\signal_state.h
(Get-Content src\core\signal_state.cpp) -replace '\bbc\b','ob' | Set-Content src\core\signal_state.cpp
```

- [ ] **Step 3: Write the failing test `tests/signal_state_test.cpp`**

Note two things, both verified against the lifted source and BreadCamera's control protocol:
- The wire literal is **lowercase** (`"ready"`, `"connecting"`, `"live"`, `"offline"`) — BreadCamera's real contract (`src/engine/camera.cpp` sets `p + "/state" = "ready"`; its e2e tests assert `...["state"] == "live"`). The design spec's `Ready | Connecting | Live | Offline` is prose enumerating the enum members, exactly as it writes `kind` as `OMT | OMT-HX | NDI` and `quality` as `Low | Medium | High`. Do **not** capitalise the strings.
- Wrap `toString(...)` in `std::string(...)`. Comparing a `std::string_view` to a string literal makes doctest's expression decomposition produce `string_view + const char*`, which does not compile (BreadCamera's `aja_test.cpp` / `decklink_test.cpp` carry a `DOCTEST_CONFIG_DOUBLE_STRINGIFY` workaround for it). Converting to `std::string` sidesteps it.

```cpp
#include <doctest/doctest.h>

#include <string>

#include "core/signal_state.h"

TEST_CASE("the four signal states serialise to their lowercase wire names") {
  CHECK(std::string(ob::toString(ob::SignalState::Ready)) == "ready");
  CHECK(std::string(ob::toString(ob::SignalState::Connecting)) == "connecting");
  CHECK(std::string(ob::toString(ob::SignalState::Live)) == "live");
  CHECK(std::string(ob::toString(ob::SignalState::Offline)) == "offline");
}

TEST_CASE("an absent error serialises to JSON null and a present one to an object") {
  CHECK(ob::errorToJson(std::nullopt).is_null());
  const auto j = ob::errorToJson(ob::ErrorInfo{"ndi-failed", "no runtime"});
  CHECK(j["code"] == "ndi-failed");
  CHECK(j["message"] == "no runtime");
}
```

- [ ] **Step 4: Create the `ob_core` library and wire the test into CMake**

Task 2 left `ob_core` uncreated (its comment said "created in Task 6" — this is that moment; `frame.h` was header-only, so nothing linked). Two edits:

First, `nlohmann_json::nlohmann_json` is an IMPORTED target that does not exist until it is found. Listing `nlohmann-json` in `vcpkg.json` installs the port but defines nothing, so add this line beside the existing `find_package(doctest CONFIG REQUIRED)`:
```cmake
find_package(nlohmann_json CONFIG REQUIRED)
```

Second, add the library just above the `# ---- tests ----` marker, *replacing that stale comment*:

```cmake
# ---- core ----
# The shared, UI-free core library. Every later task adds its .cpp to this list.
add_library(ob_core STATIC
  src/core/signal_state.cpp
)
target_include_directories(ob_core PUBLIC src)
# PUBLIC: signal_state.h includes <nlohmann/json.hpp>, and every later core header that
# does the same must pass that include path on to whoever links ob_core.
target_link_libraries(ob_core PUBLIC nlohmann_json::nlohmann_json)
```

Then, in the `ob_tests` block: add `tests/signal_state_test.cpp` to its sources and add `target_link_libraries(ob_tests PRIVATE ob_core)` (keep the existing `doctest::doctest`, `omthx` and `ob_copy_runtime` lines). `ob_tests` keeps `PRIVATE tests src` — harmless, and `ob_core` also supplies `src` publicly.

Run:
```powershell
cmake --preset release; cmake --build --preset release; .\build\release\ob_tests.exe
```
Expected: `[doctest] Status: SUCCESS!` — the two new cases pass alongside the existing ones.

- [ ] **Step 5: Commit**

```bash
git add src/core/signal_state.h src/core/signal_state.cpp tests/signal_state_test.cpp CMakeLists.txt
git commit -m "feat(core): signal states, lifted from BreadCamera"
```

---

## Task 7: Drop-oldest queue

**Files:**
- Create: `src/core/bounded_queue.h`, `tests/bounded_queue_test.cpp`

- [ ] **Step 1: Copy and rename**

Run:
```powershell
Copy-Item ..\BreadCamera\src\core\bounded_queue.h src\core\bounded_queue.h
(Get-Content src\core\bounded_queue.h) -replace '\bbc\b','ob' | Set-Content src\core\bounded_queue.h
```
Expected: the file is header-only, is a template, drops the oldest item when full, and `push()` never blocks.

- [ ] **Step 2: Write the failing test `tests/bounded_queue_test.cpp`**

```cpp
#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include "core/bounded_queue.h"

using namespace std::chrono_literals;

TEST_CASE("push never blocks and drops the oldest when full") {
  ob::BoundedQueue<int> q(2);
  CHECK_FALSE(q.push(1));
  CHECK_FALSE(q.push(2));
  CHECK(q.push(3));  // dropped 1
  CHECK(q.dropped() == 1);
  CHECK(q.pop(0ms) == 2);
  CHECK(q.pop(0ms) == 3);
  CHECK_FALSE(q.pop(0ms).has_value());
}

TEST_CASE("close unblocks a waiting pop and reopen resumes") {
  ob::BoundedQueue<int> q(2);
  CHECK_FALSE(q.closed());
  q.close();
  CHECK(q.closed());
  CHECK_FALSE(q.push(1));
  CHECK_FALSE(q.pop(0ms).has_value());
  q.reopen();
  CHECK_FALSE(q.closed());
  CHECK_FALSE(q.push(1));
  CHECK(q.pop(0ms) == 1);
}

// The case above proves the *state*, not the *wakeup*: it closes before it pops, so nothing
// is ever parked. OutputRow::stop() (Task 11) relies on close() releasing a worker already
// blocked inside pop(), so that interleaving needs its own case with a real thread.
TEST_CASE("close releases a thread parked in pop") {
  ob::BoundedQueue<int> q(2);
  std::atomic<bool> returned = false;
  std::optional<int> got;
  std::thread t([&] {
    got = q.pop(5000ms);
    returned = true;
  });
  std::this_thread::sleep_for(50ms);
  CHECK_FALSE(returned.load());  // still parked: nothing pushed and not yet closed

  const auto start = std::chrono::steady_clock::now();
  q.close();
  t.join();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  CHECK(returned.load());
  CHECK_FALSE(got.has_value());
  CHECK(q.closed());
  // pop's timeout is 5000 ms, so this passes only if close() *woke* the parked pop rather
  // than letting the timeout expire. Without it, a close() that forgot to notify would
  // still pass — 5 s slower. This is the assertion that makes the test about the wakeup.
  CHECK(elapsed < 1000ms);
}

TEST_CASE("push wakes a thread parked in pop") {
  ob::BoundedQueue<int> q(2);
  std::optional<int> got;
  std::thread t([&] { got = q.pop(5000ms); });
  std::this_thread::sleep_for(20ms);
  CHECK_FALSE(q.push(7));
  t.join();
  CHECK(got == 7);
}
```

- [ ] **Step 3: Guard the capacity, then add `closed()`, to the queue**

Two edits to `src/core/bounded_queue.h`:

**(a) Guard `capacity == 0`.** With `capacity_ == 0` the first `push` finds `q_.size() (0) >= capacity_ (0)` true and calls `q_.pop_front()` on an **empty** deque — undefined behaviour, not a harmless no-op. Add `assert`, and add `#include <cassert>` beside the header's other includes:

```cpp
  explicit BoundedQueue(size_t capacity) : capacity_(capacity) {
    assert(capacity >= 1);
  }
```

**(b) `closed()`.** `OutputRow`'s worker must tell a timeout apart from a close. Insert into `BoundedQueue`'s public section:

```cpp
  bool closed() const {
    std::lock_guard l(m_);
    return closed_;
  }
```

`closed()` is the **only thing this file gains over BreadCamera's copy** (BreadCamera's `bounded_queue.h` has no such accessor); `OutputRow`'s worker needs it to tell a pop timeout apart from a close. Because it is added code, Step 2's test asserts it (hence the `closed()` checks above). It is `const` and locks the `mutable` mutex, so it is race-free against `close()`/`reopen()`. Both (a) and (b) keep the rest of the file byte-identical to BreadCamera's.

- [ ] **Step 4: Add to `CMakeLists.txt` and run**

`bounded_queue.h` is a header-only template, so `ob_core` gains **no** source here — the only change is adding `tests/bounded_queue_test.cpp` to the `ob_tests` sources.

Run:
```powershell
cmake --preset release; cmake --build --preset release; .\build\release\ob_tests.exe
```
Expected: `[doctest] Status: SUCCESS!` — the two new cases pass alongside the existing seven.

- [ ] **Step 5: Commit**

```bash
git add src/core/bounded_queue.h tests/bounded_queue_test.cpp CMakeLists.txt
git commit -m "feat(core): drop-oldest queue, lifted from BreadCamera"
```

---

## Task 8: The Bridge data model

**Files:**
- Create: `src/core/bridge_model.h`, `src/core/bridge_model.cpp`, `tests/bridge_model_test.cpp`

- [ ] **Step 1: Write the failing test `tests/bridge_model_test.cpp`**

```cpp
#include <doctest/doctest.h>

#include <string>

#include "core/bridge_model.h"

TEST_CASE("a new bridge has a default name and no sources or outputs") {
  ob::Bridge b;
  CHECK(b.name.empty());
  CHECK(b.sources.empty());
  CHECK(b.outputs.empty());
}

TEST_CASE("findSource and findOutput return nullptr for an unknown id") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC (Cam)"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt});
  CHECK(b.findSource("s1") != nullptr);
  CHECK(b.findSource("nope") == nullptr);
  CHECK(b.findOutput("o1") != nullptr);
  CHECK(b.findOutput("nope") == nullptr);
}

TEST_CASE("outputsFor counts every output that reads one source (N x M)") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Ndi, .address = "CAM"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::OmtHx});
  b.outputs.push_back(ob::Output{.id = "o2", .sourceId = "s1", .target = ob::OutputTarget::Ndi});
  b.outputs.push_back(ob::Output{.id = "o3", .sourceId = "s2", .target = ob::OutputTarget::Omt});
  CHECK(b.outputsFor("s1") == 2u);
  CHECK(b.outputsFor("s2") == 1u);
  CHECK(b.outputsFor("none") == 0u);
}

TEST_CASE("the advertised name joins the bridge and row names") {
  ob::Bridge b;
  b.name = "Hanoi Bridge";
  ob::Output o{.id = "o1", .sourceId = "", .target = ob::OutputTarget::Omt, .name = "Cam 1"};
  CHECK(b.advertisedName(o) == "Hanoi Bridge (Cam 1)");
}

TEST_CASE("advertisedName falls back to the row name when the bridge is unnamed") {
  ob::Bridge b;
  ob::Output o{.id = "o1", .sourceId = "", .target = ob::OutputTarget::Omt, .name = "Cam 1"};
  CHECK(b.advertisedName(o) == "Cam 1");
}

// Pins every wire name. `toString` returning a string_view compared to a literal is the same
// doctest decomposition trap Task 6 hit, hence the std::string(...) wrappers.
TEST_CASE("the enums serialise to their wire names") {
  CHECK(std::string(ob::toString(ob::SourceKind::Omt)) == "OMT");
  CHECK(std::string(ob::toString(ob::SourceKind::OmtHx)) == "OMT-HX");
  CHECK(std::string(ob::toString(ob::SourceKind::Ndi)) == "NDI");
  CHECK(std::string(ob::toString(ob::OutputTarget::Omt)) == "OMT");
  CHECK(std::string(ob::toString(ob::OutputTarget::OmtHx)) == "OMT-HX");
  CHECK(std::string(ob::toString(ob::OutputTarget::Ndi)) == "NDI");
  CHECK(std::string(ob::toString(ob::HxCodec::Hevc)) == "HEVC");
  CHECK(std::string(ob::toString(ob::HxCodec::H264)) == "H.264");
  CHECK(std::string(ob::toString(ob::Quality::Low)) == "Low");
  CHECK(std::string(ob::toString(ob::Quality::Medium)) == "Medium");
  CHECK(std::string(ob::toString(ob::Quality::High)) == "High");
  CHECK(std::string(ob::toString(ob::AlphaMode::Auto)) == "Auto");
  CHECK(std::string(ob::toString(ob::AlphaMode::Off)) == "Off");
}

// The writers and readers must agree, or the control protocol is asymmetric (a state reply
// writes "OMT" while add-source rejects it). This is the test that keeps them one table.
//
// The comparisons cast to int deliberately. `CHECK(*back == k)` with two enum operands makes
// doctest stringify each via unqualified `toString`; ADL then picks ob::toString(SourceKind),
// which returns std::string_view, and `string_view + const char*` does not compile — the same
// trap Task 6 hit. Casting to int sidesteps the stringification entirely.
TEST_CASE("every wire name parses back to its enum") {
  for (auto k : {ob::SourceKind::Omt, ob::SourceKind::OmtHx, ob::SourceKind::Ndi}) {
    const auto back = ob::sourceKindFromString(ob::toString(k));
    REQUIRE(back.has_value());
    CHECK(static_cast<int>(*back) == static_cast<int>(k));
  }
  for (auto t : {ob::OutputTarget::Omt, ob::OutputTarget::OmtHx, ob::OutputTarget::Ndi}) {
    const auto back = ob::outputTargetFromString(ob::toString(t));
    REQUIRE(back.has_value());
    CHECK(static_cast<int>(*back) == static_cast<int>(t));
  }
  for (auto c : {ob::HxCodec::Hevc, ob::HxCodec::H264}) {
    const auto back = ob::hxCodecFromString(ob::toString(c));
    REQUIRE(back.has_value());
    CHECK(static_cast<int>(*back) == static_cast<int>(c));
  }
  for (auto q : {ob::Quality::Low, ob::Quality::Medium, ob::Quality::High}) {
    const auto back = ob::qualityFromString(ob::toString(q));
    REQUIRE(back.has_value());
    CHECK(static_cast<int>(*back) == static_cast<int>(q));
  }
  for (auto a : {ob::AlphaMode::Auto, ob::AlphaMode::Off}) {
    const auto back = ob::alphaModeFromString(ob::toString(a));
    REQUIRE(back.has_value());
    CHECK(static_cast<int>(*back) == static_cast<int>(a));
  }
  CHECK_FALSE(ob::sourceKindFromString("Telepathy").has_value());
  CHECK_FALSE(ob::hxCodecFromString("omt").has_value());  // the wire name is case-sensitive
}

TEST_CASE("equality ignores liveness, so a Live source still equals its saved form") {
  ob::Source a{.id = "s1", .kind = ob::SourceKind::Ndi, .address = "CAM"};
  ob::Source b = a;
  b.state = ob::SignalState::Live;
  b.format = ob::VideoFormatView{1920, 1080, 60, 1, false, true};
  CHECK(a == b);
  b.address = "OTHER";
  CHECK_FALSE(a == b);
}
```

- [ ] **Step 2: Write `src/core/bridge_model.h`**

```cpp
#pragma once
// The three objects of the bridge (design 5). Bridge is level 2; Source and Output are level 3.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/signal_state.h"

namespace ob {

enum class SourceKind { Omt, OmtHx, Ndi };
enum class OutputTarget { Omt, OmtHx, Ndi };
enum class HxCodec { Hevc, H264 };
enum class Quality { Low, Medium, High };
enum class AlphaMode { Auto, Off };

std::string_view toString(SourceKind k);
std::string_view toString(OutputTarget t);
std::string_view toString(HxCodec c);
std::string_view toString(Quality q);
std::string_view toString(AlphaMode a);

// The wire names, and their one home. kind/target/codec take their proper spelling ("OMT",
// "OMT-HX", "NDI", "HEVC", "H.264"); quality and alpha follow design 5.2/5.3's field listing
// ("Low"/"Medium"/"High", "Auto"/"Off"). These differ in case from `state` ("live"), which is
// lowercase only because it is lifted verbatim from BreadCamera's shipped wire contract
// (Task 6) and must stay diffable.
//
// The parsers below compare against toString(...) rather than re-listing the literals, so each
// name exists exactly once. The JSON store (Task 9) and the command router (Task 13) MUST use
// these — never their own name tables, which is how the protocol would otherwise end up writing
// "OMT" in a state reply while rejecting it in an add-source.
std::optional<SourceKind> sourceKindFromString(std::string_view s);
std::optional<OutputTarget> outputTargetFromString(std::string_view s);
std::optional<HxCodec> hxCodecFromString(std::string_view s);
std::optional<Quality> qualityFromString(std::string_view s);
std::optional<AlphaMode> alphaModeFromString(std::string_view s);

// What a source is sending, once known. Kept separate from core/frame.h's VideoFormat so
// core/ stays free of the omthx headers; the adapters copy one into the other.
struct VideoFormatView {
  int width = 0;
  int height = 0;
  int fpsN = 60;
  int fpsD = 1;
  bool alpha = false;
  bool known = false;
  bool operator==(const VideoFormatView&) const = default;
};

struct Source {
  std::string id;
  SourceKind kind = SourceKind::Omt;
  std::string address;  // OMT: "HOSTNAME (name)"  ·  NDI: the NDI source name
  SignalState state = SignalState::Ready;  // runtime liveness: NOT persisted (Task 9)
  VideoFormatView format;                  // runtime: NOT persisted
  std::optional<HxCodec> codec;  // OMT-HX only

  // Equality is CONFIGURATION equality: `state` and `format` are live values, not saved config,
  // so a source that goes Live still equals its on-disk form. The JSON store round-trip (Task 9)
  // compares exactly this, and no future diff-based apply() may read a liveness change as an
  // edit and restart an adapter.
  bool operator==(const Source& o) const {
    return id == o.id && kind == o.kind && address == o.address && codec == o.codec;
  }
};

struct Output {
  std::string id;
  std::string sourceId;  // any Source, or empty
  OutputTarget target = OutputTarget::Omt;
  HxCodec codec = HxCodec::Hevc;
  Quality quality = Quality::Medium;
  AlphaMode alpha = AlphaMode::Auto;
  std::string name;
  SignalState state = SignalState::Ready;  // runtime liveness: NOT persisted

  // Configuration equality (see Source): `state` is live liveness, not saved config.
  bool operator==(const Output& o) const {
    return id == o.id && sourceId == o.sourceId && target == o.target && codec == o.codec &&
           quality == o.quality && alpha == o.alpha && name == o.name;
  }
};

struct BridgeSettings {
  OutputTarget defaultTarget = OutputTarget::OmtHx;
  Quality defaultQuality = Quality::Medium;
  int thumbnailIntervalMs = 2000;
  bool startEngineWithApp = false;
  bool operator==(const BridgeSettings&) const = default;
};

struct Bridge {
  std::string name;
  std::vector<Source> sources;
  std::vector<Output> outputs;
  BridgeSettings settings;

  Source* findSource(const std::string& id);
  const Source* findSource(const std::string& id) const;
  Output* findOutput(const std::string& id);
  const Output* findOutput(const std::string& id) const;

  // N x M: how many outputs read this source.
  size_t outputsFor(const std::string& sourceId) const;

  // "<Bridge.name> (<Output.name>)", the name a remote app sees (design 5.3).
  std::string advertisedName(const Output& o) const;
};

}  // namespace ob
```

- [ ] **Step 3: Write `src/core/bridge_model.cpp`**

```cpp
#include "core/bridge_model.h"

#include <algorithm>
#include <utility>

namespace ob {

std::string_view toString(SourceKind k) {
  switch (k) {
    case SourceKind::Omt: return "OMT";
    case SourceKind::OmtHx: return "OMT-HX";
    case SourceKind::Ndi: return "NDI";
  }
  return "OMT";
}

std::string_view toString(OutputTarget t) {
  switch (t) {
    case OutputTarget::Omt: return "OMT";
    case OutputTarget::OmtHx: return "OMT-HX";
    case OutputTarget::Ndi: return "NDI";
  }
  return "OMT";
}

std::string_view toString(HxCodec c) { return c == HxCodec::Hevc ? "HEVC" : "H.264"; }
std::string_view toString(Quality q) {
  switch (q) {
    case Quality::Low: return "Low";
    case Quality::Medium: return "Medium";
    case Quality::High: return "High";
  }
  return "Medium";
}
std::string_view toString(AlphaMode a) { return a == AlphaMode::Auto ? "Auto" : "Off"; }

// Each parser compares against toString(...), so the literal exists in exactly one place and a
// rename cannot desync the reader from the writer.
std::optional<SourceKind> sourceKindFromString(std::string_view s) {
  if (s == toString(SourceKind::Omt)) return SourceKind::Omt;
  if (s == toString(SourceKind::OmtHx)) return SourceKind::OmtHx;
  if (s == toString(SourceKind::Ndi)) return SourceKind::Ndi;
  return std::nullopt;
}
std::optional<OutputTarget> outputTargetFromString(std::string_view s) {
  if (s == toString(OutputTarget::Omt)) return OutputTarget::Omt;
  if (s == toString(OutputTarget::OmtHx)) return OutputTarget::OmtHx;
  if (s == toString(OutputTarget::Ndi)) return OutputTarget::Ndi;
  return std::nullopt;
}
std::optional<HxCodec> hxCodecFromString(std::string_view s) {
  if (s == toString(HxCodec::Hevc)) return HxCodec::Hevc;
  if (s == toString(HxCodec::H264)) return HxCodec::H264;
  return std::nullopt;
}
std::optional<Quality> qualityFromString(std::string_view s) {
  if (s == toString(Quality::Low)) return Quality::Low;
  if (s == toString(Quality::Medium)) return Quality::Medium;
  if (s == toString(Quality::High)) return Quality::High;
  return std::nullopt;
}
std::optional<AlphaMode> alphaModeFromString(std::string_view s) {
  if (s == toString(AlphaMode::Auto)) return AlphaMode::Auto;
  if (s == toString(AlphaMode::Off)) return AlphaMode::Off;
  return std::nullopt;
}

const Source* Bridge::findSource(const std::string& id) const {
  const auto it = std::find_if(sources.begin(), sources.end(), [&](const Source& s) { return s.id == id; });
  return it == sources.end() ? nullptr : &*it;
}
Source* Bridge::findSource(const std::string& id) {
  // The const overload does the lookup; this one only removes constness from the result.
  return const_cast<Source*>(std::as_const(*this).findSource(id));
}
const Output* Bridge::findOutput(const std::string& id) const {
  const auto it = std::find_if(outputs.begin(), outputs.end(), [&](const Output& o) { return o.id == id; });
  return it == outputs.end() ? nullptr : &*it;
}
Output* Bridge::findOutput(const std::string& id) {
  return const_cast<Output*>(std::as_const(*this).findOutput(id));
}

size_t Bridge::outputsFor(const std::string& sourceId) const {
  return static_cast<size_t>(std::count_if(outputs.begin(), outputs.end(),
                                           [&](const Output& o) { return o.sourceId == sourceId; }));
}

std::string Bridge::advertisedName(const Output& o) const {
  if (name.empty()) return o.name;
  return name + " (" + o.name + ")";
}

}  // namespace ob
```

- [ ] **Step 4: Wire into `CMakeLists.txt` and run**

Two changes: add `src/core/bridge_model.cpp` to the **`ob_core`** source list, and `tests/bridge_model_test.cpp` to `ob_tests`. (Adding only the test would leave `Bridge::findSource` et al. undefined and fail the link.)

Run:
```powershell
cmake --preset release; cmake --build --preset release; .\build\release\ob_tests.exe
```
Expected: `[doctest] Status: SUCCESS!` — the new cases pass alongside the existing eleven.

- [ ] **Step 5: Commit**

```bash
git add src/core/bridge_model.h src/core/bridge_model.cpp tests/bridge_model_test.cpp CMakeLists.txt
git commit -m "feat(core): Bridge, Source and Output model"
```

---

## Task 9: Bridge JSON persistence

**Files:**
- Create: `src/core/bridge_store.h`, `src/core/bridge_store.cpp`, `tests/bridge_store_test.cpp`

- [ ] **Step 1: Write the failing test `tests/bridge_store_test.cpp`**

```cpp
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "core/bridge_store.h"

namespace fs = std::filesystem;

static fs::path tempFile(const char* name) {
  // Unique per process: two concurrent test binaries (another checkout, ctest -j) must not
  // collide, and the absent-file case must not see a previous run's stale residue.
  static const std::string unique = std::to_string(std::random_device{}());
  const fs::path dir = fs::temp_directory_path() / ("ob-bridge-tests-" + unique);
  fs::create_directories(dir);
  return dir / name;
}

TEST_CASE("a bridge round-trips through JSON unchanged") {
  ob::Bridge b;
  b.name = "Hanoi Bridge";
  b.settings.defaultTarget = ob::OutputTarget::Ndi;
  b.settings.thumbnailIntervalMs = 1500;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::OmtHx, .address = "PC (Cam)",
                                 .codec = ob::HxCodec::H264});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Ndi,
                                 .quality = ob::Quality::High, .alpha = ob::AlphaMode::Off,
                                 .name = "Cam 1"});

  const fs::path p = tempFile("round-trip.ombridge.json");
  ob::saveBridge(b, p);

  const auto loaded = ob::loadBridge(p);
  REQUIRE(loaded.has_value());
  CHECK(loaded->name == b.name);
  CHECK(loaded->sources == b.sources);
  CHECK(loaded->outputs == b.outputs);
  CHECK(loaded->settings == b.settings);
}

TEST_CASE("loading an absent file returns no value") {
  CHECK_FALSE(ob::loadBridge(tempFile("does-not-exist.ombridge.json")).has_value());
}

TEST_CASE("loading a file that is not JSON reports an error and returns no value") {
  const fs::path p = tempFile("garbage.ombridge.json");
  std::ofstream(p) << "this is not json";
  CHECK_FALSE(ob::loadBridge(p).has_value());
}

TEST_CASE("an unknown enum string in the file is rejected, not silently defaulted") {
  const fs::path p = tempFile("bad-enum.ombridge.json");
  std::ofstream(p) << R"({"name":"x","sources":[{"id":"s1","kind":"Whatever"}],"outputs":[]})";
  CHECK_FALSE(ob::loadBridge(p).has_value());
}

TEST_CASE("a file with no settings block loads with the struct defaults") {
  const fs::path p = tempFile("no-settings.ombridge.json");
  std::ofstream(p) << R"({"name":"x","sources":[],"outputs":[]})";
  const auto loaded = ob::loadBridge(p);
  REQUIRE(loaded.has_value());
  CHECK(loaded->settings == ob::BridgeSettings{});
}
```

- [ ] **Step 2: Write `src/core/bridge_store.h`**

```cpp
#pragma once
// Bridge persistence (design 5.1, 6.6). One JSON file, owned by the UI.
//
// loadBridge: a load that fails returns no value; it never invents a default bridge. It cannot
// distinguish a missing file from a corrupt one (both are no value) — Task 12 must decide
// deliberately whether that is acceptable at engine start.
//
// saveBridge: writes atomically (sibling .tmp, then one rename) and THROWS std::runtime_error if
// the write or the rename fails, so a failed save can never silently replace a good file.
#include <filesystem>
#include <optional>

#include "core/bridge_model.h"

namespace ob {

void saveBridge(const Bridge& bridge, const std::filesystem::path& path);
std::optional<Bridge> loadBridge(const std::filesystem::path& path);

}  // namespace ob
```

- [ ] **Step 3: Write `src/core/bridge_store.cpp`**

```cpp
#include "core/bridge_store.h"

#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace ob {

// No name tables here. The wire names live once, in core/bridge_model: toString() writes and the
// *FromString() parsers read, so the store cannot drift from the enum contract (and from the
// command router, which uses the same functions).
void saveBridge(const Bridge& b, const std::filesystem::path& path) {
  json j;
  j["name"] = b.name;
  j["settings"] = {{"defaultTarget", std::string(toString(b.settings.defaultTarget))},
                   {"defaultQuality", std::string(toString(b.settings.defaultQuality))},
                   {"thumbnailIntervalMs", b.settings.thumbnailIntervalMs},
                   {"startEngineWithApp", b.settings.startEngineWithApp}};

  j["sources"] = json::array();
  for (const Source& s : b.sources) {
    json js{{"id", s.id}, {"kind", std::string(toString(s.kind))}, {"address", s.address}};
    if (s.codec.has_value()) js["codec"] = std::string(toString(*s.codec));
    j["sources"].push_back(std::move(js));
  }

  j["outputs"] = json::array();
  for (const Output& o : b.outputs) {
    j["outputs"].push_back(json{{"id", o.id},
                                {"sourceId", o.sourceId},
                                {"target", std::string(toString(o.target))},
                                {"codec", std::string(toString(o.codec))},
                                {"quality", std::string(toString(o.quality))},
                                {"alpha", std::string(toString(o.alpha))},
                                {"name", o.name}});
  }

  // Write to a sibling temp file and replace it in one rename, so a failure can never truncate
  // the good file. The write is CHECKED: otherwise a full disk or an unwritable directory would
  // leave a truncated or empty file renamed over the user's configuration. `path += ".tmp"`
  // appends to the filename with no narrow-encoding round-trip through path.string().
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  out << j.dump(2);
  out.flush();
  if (!out) {
    out.close();
    std::error_code ignored;
    std::filesystem::remove(tmp, ignored);  // never leave a stray .tmp behind
    throw std::runtime_error("bridge save failed: could not write " + tmp.string());
  }
  out.close();
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) throw std::runtime_error("bridge save failed: " + ec.message());
}

std::optional<Bridge> loadBridge(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;

  json j;
  try {
    j = json::parse(in);
  } catch (const json::exception&) {
    return std::nullopt;
  }

  try {
    Bridge b;
    b.name = j.value("name", std::string{});

    if (j.contains("settings")) {
      const auto& s = j["settings"];
      // Defaults come from BridgeSettings itself, so no enum wire-name literal is re-spelled
      // here: the names live only in bridge_model. An absent key keeps the struct default; a key
      // present but unparseable rejects the whole file.
      b.settings = BridgeSettings{};
      if (s.contains("defaultTarget")) {
        auto v = outputTargetFromString(s.value("defaultTarget", std::string{}));
        if (!v) return std::nullopt;
        b.settings.defaultTarget = *v;
      }
      if (s.contains("defaultQuality")) {
        auto v = qualityFromString(s.value("defaultQuality", std::string{}));
        if (!v) return std::nullopt;
        b.settings.defaultQuality = *v;
      }
      b.settings.thumbnailIntervalMs = s.value("thumbnailIntervalMs", b.settings.thumbnailIntervalMs);
      b.settings.startEngineWithApp = s.value("startEngineWithApp", b.settings.startEngineWithApp);
    }

    for (const auto& js : j.value("sources", json::array())) {
      Source s;
      s.id = js.value("id", std::string{});
      auto kind = sourceKindFromString(js.value("kind", std::string{}));
      if (!kind) return std::nullopt;
      s.kind = *kind;
      s.address = js.value("address", std::string{});
      if (js.contains("codec")) {
        auto c = hxCodecFromString(js.value("codec", std::string{}));
        if (!c) return std::nullopt;
        s.codec = *c;
      }
      b.sources.push_back(std::move(s));
    }

    for (const auto& jo : j.value("outputs", json::array())) {
      Output o;
      o.id = jo.value("id", std::string{});
      o.sourceId = jo.value("sourceId", std::string{});
      auto target = outputTargetFromString(jo.value("target", std::string{}));
      if (!target) return std::nullopt;
      o.target = *target;
      // Absent optional keys keep Output's own defaults (again, no wire-name literals here).
      if (jo.contains("codec")) {
        auto codec = hxCodecFromString(jo.value("codec", std::string{}));
        if (!codec) return std::nullopt;
        o.codec = *codec;
      }
      if (jo.contains("quality")) {
        auto quality = qualityFromString(jo.value("quality", std::string{}));
        if (!quality) return std::nullopt;
        o.quality = *quality;
      }
      if (jo.contains("alpha")) {
        auto alpha = alphaModeFromString(jo.value("alpha", std::string{}));
        if (!alpha) return std::nullopt;
        o.alpha = *alpha;
      }
      o.name = jo.value("name", std::string{});
      b.outputs.push_back(std::move(o));
    }

    return b;
  } catch (const json::exception&) {
    return std::nullopt;
  }
}

}  // namespace ob
```

- [ ] **Step 4: Wire into `CMakeLists.txt` and run**

Two changes: add `src/core/bridge_store.cpp` to the **`ob_core`** source list, and `tests/bridge_store_test.cpp` to `ob_tests`. (Test-only wiring would leave `saveBridge`/`loadBridge` undefined and fail the link.)

Run:
```powershell
cmake --preset release; cmake --build --preset release; .\build\release\ob_tests.exe
```
Expected: `[doctest] Status: SUCCESS!` — the four new cases pass alongside the existing nineteen.

- [ ] **Step 5: Commit**

```bash
git add src/core/bridge_store.h src/core/bridge_store.cpp tests/bridge_store_test.cpp CMakeLists.txt
git commit -m "feat(core): Bridge JSON save and load"
```

---

## Task 10: SourceHub and reference counting (demand-driven)

**Files:**
- Create: `src/engine/source_hub.h`, `src/engine/source_hub.cpp`, `tests/source_hub_test.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write the failing test `tests/source_hub_test.cpp`**

```cpp
#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "engine/source_hub.h"

static std::shared_ptr<const ob::Frame> makeFrame(int w, int h) {
  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{w, h, 60, 1, false};
  f->uyvy.assign(f->lumaBytes(), 0x10);
  return f;
}

TEST_CASE("a hub runs when referenced or when its thumbnail is wanted, and stops otherwise") {
  ob::SourceHub hub("s1");
  CHECK_FALSE(hub.running());

  hub.addRef();
  CHECK(hub.running());
  hub.addRef();
  hub.release();
  CHECK(hub.running());  // one output left
  hub.release();
  CHECK_FALSE(hub.running());

  hub.setThumbnailWanted(true);
  CHECK(hub.running());
  hub.setThumbnailWanted(false);
  CHECK_FALSE(hub.running());
}

TEST_CASE("running changes are reported once per transition") {
  ob::SourceHub hub("s1");
  std::atomic<int> starts{0}, stops{0};
  hub.setOnRunningChanged([&](bool on) { (on ? starts : stops).fetch_add(1); });

  hub.addRef();
  hub.addRef();  // no second start
  hub.release();
  hub.release();

  CHECK(starts.load() == 1);
  CHECK(stops.load() == 1);
}

TEST_CASE("the latest frame is the newest published one") {
  ob::SourceHub hub("s1");
  // Assert on bools or through the pointee: doctest cannot stream a shared_ptr<const ob::Frame>
  // (it would need an operator<< for ob::Frame*, which does not exist — see the plan's rules).
  CHECK_FALSE(static_cast<bool>(hub.latest()));
  hub.publish(makeFrame(4, 2));
  hub.publish(makeFrame(8, 2));
  REQUIRE(hub.latest().get() != nullptr);
  CHECK(hub.latest()->format.width == 8);
}

// The class is the engine's synchronisation point, so its invariant gets a real interleaving:
// every false->true transition is matched by a true->false one, so starts must equal stops and
// the hub must end idle. This is the runnable check for the once-per-transition claim.
TEST_CASE("hammering addRef and release from several threads keeps starts and stops balanced") {
  ob::SourceHub hub("s1");
  std::atomic<int> starts{0}, stops{0};
  hub.setOnRunningChanged([&](bool on) { (on ? starts : stops).fetch_add(1); });

  constexpr int kThreads = 4;
  constexpr int kIters = 500;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < kIters; ++i) {
        hub.addRef();
        hub.release();
      }
    });
  }
  for (auto& th : threads) th.join();

  CHECK(hub.refs() == 0);
  CHECK_FALSE(hub.running());
  CHECK(starts.load() == stops.load());
}
```

- [ ] **Step 2: Write `src/engine/source_hub.h`**

```cpp
#pragma once
// One source (design 6.4). Publishes the newest decoded frame into a single slot; the
// adapter is meant to run only while the hub is running, which is exactly when at least one
// output references the source or its thumbnail is wanted (demand-driven).
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "core/frame.h"

namespace ob {

class SourceHub {
 public:
  using RunningChanged = std::function<void(bool running)>;

  explicit SourceHub(std::string id) : id_(std::move(id)) {}

  const std::string& id() const { return id_; }

  void addRef();
  void release();
  int refs() const;

  void setThumbnailWanted(bool wanted);
  bool thumbnailWanted() const;

  bool running() const;

  // Called once per running transition. The engine starts or stops the input adapter here.
  // Install this BEFORE the first demand change: the setter does not replay the current state,
  // so a callback installed after the hub is already running waits for the next transition.
  void setOnRunningChanged(RunningChanged cb);

  // Overwrites the slot; never queues. A frame shared by N outputs is decoded once (design 6.5).
  void publish(std::shared_ptr<const Frame> frame);
  std::shared_ptr<const Frame> latest() const;

  uint64_t framesPublished() const;

 private:
  bool computeRunning() const;  // caller holds mu_
  void maybeTransition();       // fires the callback once per transition, outside the lock

  std::string id_;
  mutable std::mutex mu_;
  int refs_ = 0;
  bool thumbnailWanted_ = false;
  bool running_ = false;
  RunningChanged onRunningChanged_;

  std::shared_ptr<const Frame> latest_;
  uint64_t published_ = 0;
};

}  // namespace ob
```

- [ ] **Step 3: Write `src/engine/source_hub.cpp`**

```cpp
#include "engine/source_hub.h"

#include <utility>

namespace ob {

bool SourceHub::computeRunning() const { return refs_ > 0 || thumbnailWanted_; }

void SourceHub::maybeTransition() {
  RunningChanged cb;
  bool next = false;
  {
    std::lock_guard l(mu_);
    next = computeRunning();
    if (next == running_) return;
    running_ = next;
    // Copy the callback under the lock: a concurrent setOnRunningChanged must not reassign the
    // std::function while we are invoking it (that would be a data race, and can tear).
    cb = onRunningChanged_;
  }
  // Outside the lock: the callback starts or stops an adapter, which may call back in.
  if (cb) cb(next);
}

void SourceHub::addRef() {
  {
    std::lock_guard l(mu_);
    ++refs_;
  }
  maybeTransition();
}

void SourceHub::release() {
  {
    std::lock_guard l(mu_);
    if (refs_ > 0) --refs_;
  }
  maybeTransition();
}

int SourceHub::refs() const {
  std::lock_guard l(mu_);
  return refs_;
}

void SourceHub::setThumbnailWanted(bool wanted) {
  {
    std::lock_guard l(mu_);
    thumbnailWanted_ = wanted;
  }
  maybeTransition();
}

bool SourceHub::thumbnailWanted() const {
  std::lock_guard l(mu_);
  return thumbnailWanted_;
}

bool SourceHub::running() const {
  std::lock_guard l(mu_);
  return running_;
}

void SourceHub::setOnRunningChanged(RunningChanged cb) {
  std::lock_guard l(mu_);
  onRunningChanged_ = std::move(cb);
}

void SourceHub::publish(std::shared_ptr<const Frame> frame) {
  std::shared_ptr<const Frame> previous;
  {
    std::lock_guard l(mu_);
    previous = std::move(latest_);
    latest_ = std::move(frame);
    ++published_;
  }
  // `previous` is released here, outside the lock: if this hub held the last reference, dropping
  // it frees a full frame, and that must not happen while the mutex is held.
}

std::shared_ptr<const Frame> SourceHub::latest() const {
  std::lock_guard l(mu_);
  return latest_;
}

uint64_t SourceHub::framesPublished() const {
  std::lock_guard l(mu_);
  return published_;
}

}  // namespace ob
```

- [ ] **Step 4: Add `ob_engine` to `CMakeLists.txt`**

`ixwebsocket::ixwebsocket` is an IMPORTED target, so first add its `find_package` beside the others at the top of `CMakeLists.txt` (vcpkg installs the port; only `find_package` defines the target):
```cmake
find_package(ixwebsocket CONFIG REQUIRED)
```

Then create the engine library:

```cmake
# ---- engine ----
add_library(ob_engine STATIC
  src/engine/source_hub.cpp   # Tasks 11-15 append output_row, bridge_graph, command_router, control_server, engine
)
target_include_directories(ob_engine PUBLIC src)
target_link_libraries(ob_engine PUBLIC ob_core ixwebsocket::ixwebsocket)
# bcrypt is a Windows SDK library (bcrypt.lib, in the VS environment's default lib paths), not a
# vcpkg port — exactly as BreadCamera's bc_engine links it. control_server uses it in Task 13.
target_link_libraries(ob_engine PUBLIC bcrypt)
```

Finally, wire the test: add `tests/source_hub_test.cpp` to `ob_tests` **and** link `ob_tests` to `ob_engine` — without that link the test's `ob::SourceHub` symbols are undefined:

```cmake
target_link_libraries(ob_tests PRIVATE ob_engine)
```

(`ixwebsocket` reaches `ob_tests` transitively through `ob_engine`'s `PUBLIC` usage requirement, so it needs no separate line.)

- [ ] **Step 5: Run the tests**

Run:
```powershell
cmake --preset release; cmake --build --preset release; .\build\release\ob_tests.exe
```
Expected: SUCCESS, including `running changes are reported once per transition`.

- [ ] **Step 6: Commit**

```bash
git add src/engine/source_hub.h src/engine/source_hub.cpp tests/source_hub_test.cpp CMakeLists.txt
git commit -m "feat(engine): reference-counted SourceHub"
```

---

## Task 11: OutputRow with drop-oldest and isolation

**Files:**
- Create: `src/engine/output_row.h`, `src/engine/output_row.cpp`, `tests/output_row_test.cpp`

- [ ] **Step 1: Write the failing test `tests/output_row_test.cpp`**

```cpp
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "engine/output_row.h"

using namespace std::chrono_literals;

static std::shared_ptr<const ob::Frame> frame(int64_t ts) {
  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{4, 2, 60, 1, false};
  f->uyvy.assign(f->lumaBytes(), 0x10);
  f->timestamp = ts;
  return f;
}

TEST_CASE("a row delivers what it is offered, in order") {
  std::mutex m;
  std::vector<int64_t> got;
  ob::OutputRow row("o1", "s1", [&](const std::shared_ptr<const ob::Frame>& f) {
    std::lock_guard l(m);
    got.push_back(f->timestamp);
  });
  row.start();
  for (int64_t i = 0; i < 3; ++i) row.offer(frame(i));

  // stop() closes the queue and clears anything not yet delivered, so wait for delivery first.
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (row.delivered() < 3 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();

  CHECK(got == std::vector<int64_t>{0, 1, 2});
}

TEST_CASE("a slow sink drops the oldest frames and never blocks the producer") {
  std::atomic<bool> gate{false};
  ob::OutputRow row("o1", "s1",
                    [&](const std::shared_ptr<const ob::Frame>&) {
                      while (!gate.load()) std::this_thread::sleep_for(1ms);
                    }, /*capacity=*/2u);
  row.start();

  // The worker takes one and blocks; two more fill the queue; the next two drop the oldest.
  for (int64_t i = 0; i < 5; ++i) row.offer(frame(i));
  gate.store(true);
  row.stop();

  CHECK(row.dropped() >= 2u);
}

TEST_CASE("stopping a row unblocks its worker") {
  ob::OutputRow row("o1", "s1", [](const std::shared_ptr<const ob::Frame>&) {});
  row.start();
  row.stop();  // must not hang
  CHECK(true);
}

TEST_CASE("a sink that throws stops the row instead of terminating the process") {
  ob::OutputRow row("o1", "s1", [](const std::shared_ptr<const ob::Frame>&) {
    throw std::runtime_error("sink broke");
  });
  row.start();
  row.offer(frame(0));

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!row.faulted() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();  // must not hang on a worker that already returned

  CHECK(row.faulted());
  CHECK(row.delivered() == 0u);  // the frame was consumed by the throw, not delivered
}

TEST_CASE("a row can be started again after a stop") {
  ob::OutputRow row("o1", "s1", [](const std::shared_ptr<const ob::Frame>&) {});
  row.start();
  row.offer(frame(0));
  auto deadline = std::chrono::steady_clock::now() + 2s;
  while (row.delivered() < 1 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();
  CHECK(row.delivered() == 1u);

  // Restart: start() must reopen the closed queue, or the worker exits at once and delivers
  // nothing (this is the "Stop all outputs, then Start" cycle the UI offers).
  row.start();
  row.offer(frame(1));
  deadline = std::chrono::steady_clock::now() + 2s;
  while (row.delivered() < 2 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  row.stop();
  CHECK(row.delivered() == 2u);
}
```

- [ ] **Step 2: Write `src/engine/output_row.h`**

```cpp
#pragma once
// One output (design 6.5). Reads its source's newest frame and hands it to a sink. Its own
// drop-oldest queue means a slow or failed sink can never block another row or its SourceHub.
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "core/bounded_queue.h"
#include "core/frame.h"

namespace ob {

class OutputRow {
 public:
  using Deliver = std::function<void(const std::shared_ptr<const Frame>&)>;

  OutputRow(std::string id, std::string sourceId, Deliver deliver, size_t capacity = 4);
  ~OutputRow();
  OutputRow(const OutputRow&) = delete;
  OutputRow& operator=(const OutputRow&) = delete;

  // Lifecycle and setSourceId have a single writer: BridgeGraph serialises every row call under
  // its own mutex. Two threads calling start() at once could assign over a live std::thread.
  void start();
  void stop();

  void offer(std::shared_ptr<const Frame> frame);

  const std::string& id() const { return id_; }
  const std::string& sourceId() const { return sourceId_; }
  void setSourceId(std::string id) { sourceId_ = std::move(id); }

  uint64_t delivered() const { return delivered_.load(); }  // atomic: incremented on the worker thread
  uint64_t dropped() const { return queue_.dropped(); }
  // True once the sink threw. The worker then stops delivering (it does not spin retrying); the
  // engine reads this to mark this output Offline, so one failed sink affects only its own row.
  bool faulted() const { return faulted_.load(); }

 private:
  void run();

  std::string id_;
  std::string sourceId_;
  Deliver deliver_;
  BoundedQueue<std::shared_ptr<const Frame>> queue_;
  std::thread thread_;
  // Read by the test (and later the state snapshot) on a different thread from run(), which
  // does the increment: it must be atomic, not a plain uint64_t.
  std::atomic<uint64_t> delivered_ = 0;
  std::atomic<bool> faulted_ = false;
};

}  // namespace ob
```

- [ ] **Step 3: Write `src/engine/output_row.cpp`**

```cpp
#include "engine/output_row.h"

#include <chrono>
#include <exception>

namespace ob {
namespace {
constexpr auto kPopTimeout = std::chrono::milliseconds(100);
}

OutputRow::OutputRow(std::string id, std::string sourceId, Deliver deliver, size_t capacity)
    : id_(std::move(id)), sourceId_(std::move(sourceId)), deliver_(std::move(deliver)),
      queue_(capacity) {}

OutputRow::~OutputRow() { stop(); }

void OutputRow::start() {
  if (thread_.joinable()) return;
  // A deliberate restart retries: clear a previous fault, or a row that now works would still be
  // reported Offline by the engine. `delivered_` is NOT reset — it is a cumulative stat.
  faulted_.store(false);
  // A row can be started again after stop() ("Stop all outputs", then resume): stop() closed the
  // queue, so reopen it before the worker starts — otherwise its first pop returns nullopt, sees
  // `closed()`, and the worker exits immediately, delivering nothing.
  queue_.reopen();
  thread_ = std::thread([this] { run(); });
}

void OutputRow::stop() {
  queue_.close();
  if (thread_.joinable()) thread_.join();
}

void OutputRow::offer(std::shared_ptr<const Frame> frame) { queue_.push(std::move(frame)); }

void OutputRow::run() {
  for (;;) {
    auto f = queue_.pop(kPopTimeout);
    if (!f.has_value()) {
      if (queue_.closed()) return;  // closed by stop(); a timeout just keeps waiting
      continue;
    }
    // A sink that throws must not take the process down — an exception escaping a thread body
    // calls std::terminate — and must not spin retrying either. Mark the row faulted and stop
    // delivering; the engine turns that into Offline for this output alone, so a failed sink
    // never affects another row or its SourceHub (the header's promise).
    try {
      deliver_(*f);
    } catch (...) {
      faulted_.store(true);
      return;
    }
    ++delivered_;
  }
}

}  // namespace ob
```

> `BoundedQueue::closed()` is added in Task 7, Step 3.

- [ ] **Step 4: Wire into `CMakeLists.txt` and run**

Two changes: add `src/engine/output_row.cpp` to the **`ob_engine`** source list, and `tests/output_row_test.cpp` to `ob_tests` (`ob_tests` already links `ob_engine`, so no extra link line).

Run:
```powershell
cmake --preset release; cmake --build --preset release; .\build\release\ob_tests.exe
```
Expected: `[doctest] Status: SUCCESS!` — the five new cases pass alongside the existing twenty-eight.

- [ ] **Step 5: Commit**

```bash
git add src/engine/output_row.h src/engine/output_row.cpp tests/output_row_test.cpp CMakeLists.txt
git commit -m "feat(engine): OutputRow with drop-oldest isolation"
```

---

## Task 12: BridgeGraph reconciles the model

**Files:**
- Create: `src/engine/bridge_graph.h`, `src/engine/bridge_graph.cpp`, `tests/bridge_graph_test.cpp`

- [ ] **Step 1: Write the failing test `tests/bridge_graph_test.cpp`**

```cpp
#include <doctest/doctest.h>

#include <atomic>
#include <memory>

#include "engine/bridge_graph.h"

TEST_CASE("one source feeding two outputs counts two refs on one hub (N x M)") {
  ob::Bridge b;
  b.name = "B";
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});
  b.outputs.push_back(ob::Output{.id = "o2", .sourceId = "s1", .target = ob::OutputTarget::OmtHx, .name = "B"});

  ob::BridgeGraph graph([](const std::shared_ptr<const ob::Frame>&) {});
  graph.apply(b);

  CHECK(graph.hubCount() == 1u);
  CHECK(graph.rowCount() == 2u);
  REQUIRE(graph.hub("s1") != nullptr);
  CHECK(graph.hub("s1")->refs() == 2);
  CHECK(graph.hub("s1")->running());
}

TEST_CASE("removing an output releases its ref, and an unreferenced source goes idle") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});

  ob::BridgeGraph graph([](const std::shared_ptr<const ob::Frame>&) {});
  graph.apply(b);
  REQUIRE(graph.hub("s1")->refs() == 1);

  b.outputs.clear();
  graph.apply(b);

  CHECK(graph.rowCount() == 0u);
  CHECK(graph.hub("s1")->refs() == 0);
  CHECK_FALSE(graph.hub("s1")->running());
}

TEST_CASE("an output with no source is kept as an idle row") {
  ob::Bridge b;
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "", .target = ob::OutputTarget::Omt, .name = "A"});

  ob::BridgeGraph graph([](const std::shared_ptr<const ob::Frame>&) {});
  graph.apply(b);

  CHECK(graph.rowCount() == 1u);
  CHECK(graph.hubCount() == 0u);
}

TEST_CASE("fan-out offers each published frame to every row that reads the source") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});
  b.outputs.push_back(ob::Output{.id = "o2", .sourceId = "s1", .target = ob::OutputTarget::Ndi, .name = "B"});

  ob::BridgeGraph graph([](const std::shared_ptr<const ob::Frame>&) {});
  graph.apply(b);
  graph.start();

  auto f = std::make_shared<ob::Frame>();
  f->format = ob::VideoFormat{4, 2, 60, 1, false};
  f->uyvy.assign(f->lumaBytes(), 0x10);
  graph.publish("s1", f);
  graph.stop();

  // publish() hands the frame to each row's queue; both rows were offered it.
  CHECK(graph.offeredToRow("o1") == 1u);
  CHECK(graph.offeredToRow("o2") == 1u);
}

TEST_CASE("publishing to a source that has no rows is a no-op") {
  ob::BridgeGraph graph([](const std::shared_ptr<const ob::Frame>&) {});
  auto f = std::make_shared<ob::Frame>();
  graph.publish("gone", f);  // must not throw
  CHECK(true);
}

// Re-applying an unchanged model must not churn demand: a reset-to-zero-and-re-add would fire a
// stop-then-start on every source adapter for any unrelated edit (Task 13 calls apply() per edit).
TEST_CASE("re-applying an unchanged model fires no running transitions") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});
  b.outputs.push_back(ob::Output{.id = "o2", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "B"});

  ob::BridgeGraph graph([](const std::shared_ptr<const ob::Frame>&) {});
  graph.apply(b);

  std::atomic<int> transitions{0};
  graph.hub("s1")->setOnRunningChanged([&](bool) { transitions.fetch_add(1); });

  graph.apply(b);  // the identical model

  CHECK(transitions.load() == 0);
  CHECK(graph.hub("s1")->refs() == 2);
}

// The subtle refcount case: one row leaves one source and joins another.
TEST_CASE("an output that moves to another source moves its ref") {
  ob::Bridge b;
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.sources.push_back(ob::Source{.id = "s2", .kind = ob::SourceKind::Omt, .address = "PC2"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});

  ob::BridgeGraph graph([](const std::shared_ptr<const ob::Frame>&) {});
  graph.apply(b);
  REQUIRE(graph.hub("s1")->refs() == 1);

  b.outputs[0].sourceId = "s2";
  graph.apply(b);

  CHECK(graph.hub("s1")->refs() == 0);
  CHECK_FALSE(graph.hub("s1")->running());
  CHECK(graph.hub("s2")->refs() == 1);
  CHECK(graph.hub("s2")->running());
}
```

- [ ] **Step 2: Write `src/engine/bridge_graph.h`**

```cpp
#pragma once
// Reconciles a Bridge model into running hubs and rows (design 6.4, 6.5). apply() may be
// called with a whole new model; it diffs against what it holds. No master tick: publish()
// is called by each SourceHub's adapter, and fan-out comes from the ref counts.
//
// THREADING: apply(), start() and stop() are owned by ONE thread (the engine's control loop);
// only publish() may be called from other threads (the source adapters). That is what lets the
// first three hold raw row/hub pointers across a deliberate unlock, while publish() locks for its
// read. Never call apply() from an adapter.
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/bridge_model.h"
#include "engine/output_row.h"
#include "engine/source_hub.h"

namespace ob {

class BridgeGraph {
 public:
  using Deliver = OutputRow::Deliver;

  explicit BridgeGraph(Deliver deliver);
  BridgeGraph(const BridgeGraph&) = delete;
  BridgeGraph& operator=(const BridgeGraph&) = delete;
  ~BridgeGraph();

  // Creates, updates and removes hubs and rows so they match `bridge`. Idempotent.
  void apply(const Bridge& bridge);

  void start();
  void stop();

  // Hands a frame to every row whose output reads this source.
  void publish(const std::string& sourceId, std::shared_ptr<const Frame> frame);

  // Locks; for callers outside the graph.
  // READ-ONLY uses (latest(), running(), framesPublished()) are fine from any thread. Demand
  // mutation is NOT: addRef/release/setThumbnailWanted have a single writer — the thread that
  // calls apply() — so two transitions cannot interleave and the running callback's delivery
  // order always matches the last state change. Toggle thumbnail demand by changing the model and
  // calling apply(), not by reaching a hub from another thread.
  SourceHub* hub(const std::string& sourceId) const;
  size_t hubCount() const;
  size_t rowCount() const;
  uint64_t offeredToRow(const std::string& outputId) const;

 private:
  // Caller holds mu_. apply() and publish() use these, never the locked public ones.
  SourceHub* hubUnlocked(const std::string& sourceId) const;

  struct Row {
    std::unique_ptr<OutputRow> row;
    uint64_t offered = 0;
  };

  mutable std::mutex mu_;
  Deliver deliver_;
  std::map<std::string, std::unique_ptr<SourceHub>> hubs_;
  std::map<std::string, Row> rows_;  // by output id
  bool started_ = false;
};

}  // namespace ob
```

- [ ] **Step 3: Write `src/engine/bridge_graph.cpp`**

```cpp
#include "engine/bridge_graph.h"

#include <vector>

namespace ob {

BridgeGraph::BridgeGraph(Deliver deliver) : deliver_(std::move(deliver)) {}

BridgeGraph::~BridgeGraph() { stop(); }

void BridgeGraph::apply(const Bridge& bridge) {
  // Two phases. Under mu_ we only restructure (hubs and rows). Demand is deferred to phase 2,
  // because SourceHub::addRef()/release() fire the running callback SYNCHRONOUSLY, and that
  // callback starts or stops an input adapter — which may publish back into this graph. Firing it
  // while mu_ is held would stall on, or self-deadlock with, a non-recursive mutex.
  std::map<std::string, int> wanted;                // sourceId -> refs the new model implies
  std::vector<std::unique_ptr<OutputRow>> retired;  // destroyed after mu_ is released
  {
    std::lock_guard l(mu_);

    for (const Source& s : bridge.sources) {
      if (hubs_.find(s.id) == hubs_.end()) hubs_.emplace(s.id, std::make_unique<SourceHub>(s.id));
    }
    for (auto it = hubs_.begin(); it != hubs_.end();) {
      if (bridge.findSource(it->first) == nullptr)
        it = hubs_.erase(it);
      else
        ++it;
    }

    // Remove rows whose output is gone. Every surviving hub starts from zero demand, so a
    // re-apply is exact and a hub left with no outputs is released to idle. Retired rows are moved
    // out here and destroyed AFTER mu_ is released: OutputRow's destructor joins its worker, which
    // may be inside deliver_, and holding mu_ across that join would stall publish() for every
    // other row (design 6.5: one slow sink must not block the others).
    for (auto it = rows_.begin(); it != rows_.end();) {
      if (bridge.findOutput(it->first) == nullptr) {
        retired.push_back(std::move(it->second.row));
        it = rows_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto& [id, hub] : hubs_) wanted[id] = 0;

    for (const Output& o : bridge.outputs) {
      auto found = rows_.find(o.id);
      if (found == rows_.end()) {
        Row r;
        r.row = std::make_unique<OutputRow>(o.id, o.sourceId, deliver_);
        found = rows_.emplace(o.id, std::move(r)).first;
      }
      found->second.row->setSourceId(o.sourceId);
      if (started_) found->second.row->start();
      if (hubUnlocked(o.sourceId) != nullptr) ++wanted[o.sourceId];
    }
  }
  retired.clear();  // joins each retired row's worker with mu_ released

  // Phase 2, with mu_ released: move each hub to exactly the demand the model implies, by DELTA.
  // Resetting to zero and re-adding would fire a stop-then-start transition on every apply(), so an
  // unrelated edit (renaming an output) would bounce every running source adapter. Only the
  // difference moves, so an unchanged model produces no transition at all.
  for (auto& [id, want] : wanted) {
    SourceHub* h = hub(id);  // the public accessor locks mu_ only for the lookup
    if (h == nullptr) continue;
    int cur = h->refs();
    for (; cur > want; --cur) h->release();
    for (; cur < want; ++cur) h->addRef();
  }
}

void BridgeGraph::start() {
  std::vector<OutputRow*> rows;
  {
    std::lock_guard l(mu_);
    started_ = true;
    for (auto& [id, r] : rows_) rows.push_back(r.row.get());
  }
  for (OutputRow* r : rows) r->start();  // outside mu_: start() spawns a worker
}

void BridgeGraph::stop() {
  std::vector<OutputRow*> rows;
  {
    std::lock_guard l(mu_);
    started_ = false;
    for (auto& [id, r] : rows_) rows.push_back(r.row.get());
  }
  // Join outside mu_: a worker that is inside deliver_ must not hold the lock publish() needs.
  for (OutputRow* r : rows) r->stop();
}

void BridgeGraph::publish(const std::string& sourceId, std::shared_ptr<const Frame> frame) {
  std::lock_guard l(mu_);
  SourceHub* h = hubUnlocked(sourceId);
  if (h == nullptr) return;
  h->publish(frame);
  for (auto& [id, r] : rows_) {
    if (r.row->sourceId() == sourceId) {
      r.row->offer(frame);
      ++r.offered;
    }
  }
}

SourceHub* BridgeGraph::hubUnlocked(const std::string& sourceId) const {
  const auto it = hubs_.find(sourceId);
  return it == hubs_.end() ? nullptr : it->second.get();
}

SourceHub* BridgeGraph::hub(const std::string& sourceId) const {
  std::lock_guard l(mu_);
  return hubUnlocked(sourceId);
}

size_t BridgeGraph::hubCount() const {
  std::lock_guard l(mu_);
  return hubs_.size();
}

size_t BridgeGraph::rowCount() const {
  std::lock_guard l(mu_);
  return rows_.size();
}

uint64_t BridgeGraph::offeredToRow(const std::string& outputId) const {
  std::lock_guard l(mu_);
  const auto it = rows_.find(outputId);
  return it == rows_.end() ? 0u : it->second.offered;
}

}  // namespace ob
```

> The public accessors all lock `mu_`; `apply()` and `publish()` call `hubUnlocked()` because they already hold it. Never call a public accessor from inside the lock — it would deadlock on a non-recursive `std::mutex`.

- [ ] **Step 4: Wire into `CMakeLists.txt` and run**

Two changes: add `src/engine/bridge_graph.cpp` to the **`ob_engine`** source list, and `tests/bridge_graph_test.cpp` to `ob_tests` (`ob_tests` already links `ob_engine`, so no extra link line).

Run:
```powershell
cmake --preset release; cmake --build --preset release; .\build\release\ob_tests.exe
```
Expected: `[doctest] Status: SUCCESS!` — the seven new cases pass alongside the existing thirty-four.

- [ ] **Step 5: Commit**

```bash
git add src/engine/bridge_graph.h src/engine/bridge_graph.cpp tests/bridge_graph_test.cpp CMakeLists.txt
git commit -m "feat(engine): BridgeGraph reconciles the model"
```

---

## Task 13: Control server and command router

**Files:**
- Create: `src/engine/control_server.h`, `src/engine/control_server.cpp`, `src/engine/command_router.h`, `src/engine/command_router.cpp`, `tests/command_router_test.cpp`
- Test: `tests/command_router_test.cpp`

- [ ] **Step 1: Lift the control server**

Run:
```powershell
Copy-Item ..\BreadCamera\src\engine\control_server.h  src\engine\control_server.h
Copy-Item ..\BreadCamera\src\engine\control_server.cpp src\engine\control_server.cpp
(Get-Content src\engine\control_server.h)  -replace '\bbc\b','ob' | Set-Content src\engine\control_server.h
(Get-Content src\engine\control_server.cpp) -replace '\bbc\b','ob' | Set-Content src\engine\control_server.cpp
```
Expected: it brings `tokenMatches`, `tokenFromUri`, `randomToken`, and a `ControlServer` with `send`, `sendBinary`, `broadcast` and a `Handlers` struct. Keep all of it — the token check is the security boundary (spec 6.1).

- [ ] **Step 2: Write the failing test `tests/command_router_test.cpp`**

```cpp
#include <doctest/doctest.h>

#include "engine/command_router.h"

TEST_CASE("an unknown command is refused with a named error") {
  ob::CommandRouter router;
  const auto reply = router.handle(ob::ClientId{"c1"}, R"({"cmd":"nope"})");
  CHECK(reply.find("\"ok\":false") != std::string::npos);
  CHECK(reply.find("unknown command") != std::string::npos);
}

TEST_CASE("input that is not JSON is refused, not crashed on") {
  ob::CommandRouter router;
  const auto reply = router.handle(ob::ClientId{"c1"}, "not json at all");
  CHECK(reply.find("\"ok\":false") != std::string::npos);
}

TEST_CASE("add-source rejects an address that is not text") {
  ob::CommandRouter router;
  const auto reply = router.handle(ob::ClientId{"c1"},
                                   R"({"cmd":"add-source","kind":"OMT","address":42})");
  CHECK(reply.find("\"ok\":false") != std::string::npos);
  CHECK(reply.find("address") != std::string::npos);
}

TEST_CASE("add-source rejects an unknown kind") {
  ob::CommandRouter router;
  const auto reply = router.handle(ob::ClientId{"c1"},
                                   R"({"cmd":"add-source","kind":"Telepathy","address":"x"})");
  CHECK(reply.find("\"ok\":false") != std::string::npos);
  CHECK(reply.find("kind") != std::string::npos);
}

TEST_CASE("a valid add-source returns ok and the new id") {
  ob::CommandRouter router;
  const auto reply = router.handle(ob::ClientId{"c1"},
                                   R"({"cmd":"add-source","kind":"NDI","address":"CAM 1"})");
  CHECK(reply.find("\"ok\":true") != std::string::npos);
  CHECK(reply.find("\"id\"") != std::string::npos);
}

TEST_CASE("state reports the model") {
  ob::CommandRouter router;
  router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-source","kind":"NDI","address":"CAM 1"})");
  const auto reply = router.handle(ob::ClientId{"c1"}, R"({"cmd":"state"})");
  CHECK(reply.find("\"ok\":true") != std::string::npos);
  CHECK(reply.find("CAM 1") != std::string::npos);
  // The symmetry that matters: what the reply writes is exactly what add-source accepts.
  CHECK(reply.find("\"kind\":\"NDI\"") != std::string::npos);
}

// A wrong-typed field must be refused, not throw: an exception here would escape a control-server
// thread and call std::terminate.
TEST_CASE("a wrong-typed field is refused rather than thrown on") {
  ob::CommandRouter router;
  const auto a = router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-output","sourceId":42})");
  CHECK(a.find("\"ok\":false") != std::string::npos);
  const auto b = router.handle(ob::ClientId{"c1"},
                               R"({"cmd":"set-output-source","id":"o1","sourceId":42})");
  CHECK(b.find("\"ok\":false") != std::string::npos);
  const auto c = router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-output","name":true})");
  CHECK(c.find("\"ok\":false") != std::string::npos);
}

TEST_CASE("add-output and set-output-source edit the model, and remove-source detaches dependents") {
  ob::CommandRouter router;
  router.handle(ob::ClientId{"c1"}, R"({"cmd":"add-source","kind":"OMT","address":"PC"})");
  const auto made = router.handle(ob::ClientId{"c1"},
                                  R"({"cmd":"add-output","sourceId":"s1","name":"A"})");
  CHECK(made.find("\"ok\":true") != std::string::npos);

  const auto moved = router.handle(ob::ClientId{"c1"},
                                   R"({"cmd":"set-output-source","id":"o1","sourceId":""})");
  CHECK(moved.find("\"ok\":true") != std::string::npos);

  const auto removed = router.handle(ob::ClientId{"c1"}, R"({"cmd":"remove-source","id":"s1"})");
  CHECK(removed.find("\"ok\":true") != std::string::npos);
  CHECK(router.bridge().sources.empty());
  CHECK(router.bridge().outputs.size() == 1u);         // the row is kept...
  CHECK(router.bridge().outputs[0].sourceId.empty());  // ...with its source detached
}

TEST_CASE("setBridge seeds the id counter past a loaded model") {
  ob::CommandRouter router;
  ob::Bridge loaded;
  loaded.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  router.setBridge(loaded);

  const auto reply = router.handle(ob::ClientId{"c1"},
                                   R"({"cmd":"add-source","kind":"NDI","address":"CAM"})");
  CHECK(reply.find("\"id\":\"s2\"") != std::string::npos);  // not a duplicate "s1"
}
```

- [ ] **Step 3: Write `src/engine/command_router.h`**

```cpp
#pragma once
// Validates the JSON commands the UI sends over the control channel and answers each with a
// JSON string (design 6.1). Validation failures are named and inline; the router never
// throws to its caller.
#include <string>

#include "core/bridge_model.h"
#include "engine/control_server.h"

namespace ob {

class CommandRouter {
 public:
  // Wholly synchronous: no engine threads, so the tests can call it directly.
  std::string handle(const ClientId& client, const std::string& text);

  const Bridge& bridge() const { return bridge_; }
  // Loads a saved model. Seeds the id counter past every id already present, so a later
  // add-source/add-output cannot mint a duplicate id (which would make findSource/remove-source
  // act on the wrong row, and make a state reply list two rows with one id).
  void setBridge(Bridge b);

 private:
  Bridge bridge_;
  // Sources and outputs number independently (s1, s2, … and o1, o2, …): the prefixes exist
  // precisely to tell the two kinds apart, so a shared counter would mint "o2" for a first output.
  uint64_t nextSourceId_ = 1;
  uint64_t nextOutputId_ = 1;
};

}  // namespace ob
```

- [ ] **Step 4: Write `src/engine/command_router.cpp`**

```cpp
#include "engine/command_router.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace ob {
namespace {

std::string refuse(const std::string& message) {
  return json{{"ok", false}, {"error", message}}.dump();
}

}  // namespace

void CommandRouter::setBridge(Bridge b) {
  bridge_ = std::move(b);
  nextSourceId_ = 1;
  nextOutputId_ = 1;
  // Seed each counter past its own ids in the loaded model: a duplicate id would make
  // findSource/remove-source act on the wrong row, and make a state reply list two rows under one.
  const auto seed = [](const std::string& id, char prefix, uint64_t& next) {
    if (id.size() < 2 || id[0] != prefix) return;
    try {
      const uint64_t n = std::stoull(id.substr(1));
      if (n >= next) next = n + 1;
    } catch (const std::exception&) {
      // Not a generated id (e.g. hand-edited); leave the counter alone.
    }
  };
  for (const Source& s : bridge_.sources) seed(s.id, 's', nextSourceId_);
  for (const Output& o : bridge_.outputs) seed(o.id, 'o', nextOutputId_);
}

std::string CommandRouter::handle(const ClientId&, const std::string& text) {
  // ONE try around the whole body. Every field read below (j.value(...), j["id"], ...) throws
  // nlohmann's type_error.302 on a wrong-typed value — `{"cmd":"add-output","sourceId":42}` — and
  // this runs on a control-server thread, where an escaping exception would call std::terminate.
  // The router's contract is that it never throws to its caller; this is what makes that true for
  // malformed as well as unparseable input.
  try {
    json j = json::parse(text);
    if (!j.is_object() || !j.contains("cmd") || !j["cmd"].is_string())
      return refuse("every command needs a text \"cmd\"");

    const std::string cmd = j["cmd"];

    if (cmd == "state") {
      json sources = json::array();
      for (const Source& s : bridge_.sources)
        sources.push_back({{"id", s.id}, {"kind", std::string(toString(s.kind))}, {"address", s.address}});
      json outputs = json::array();
      for (const Output& o : bridge_.outputs)
        outputs.push_back({{"id", o.id},
                           {"sourceId", o.sourceId},
                           {"target", std::string(toString(o.target))},
                           {"name", o.name},
                           {"advertised", bridge_.advertisedName(o)}});
      return json{{"ok", true}, {"name", bridge_.name}, {"sources", sources}, {"outputs", outputs}}.dump();
    }

    if (cmd == "add-source") {
      if (!j.contains("address") || !j["address"].is_string())
        return refuse("add-source needs a text \"address\"");
      if (!j.contains("kind") || !j["kind"].is_string())
        return refuse("add-source needs a text \"kind\"");
      // Wire names come from core/bridge_model — the same table a state reply writes — so an
      // unknown string is an error rather than a silent default.
      const auto kind = sourceKindFromString(j["kind"]);
      if (!kind) return refuse("add-source: unknown \"kind\": " + j["kind"].get<std::string>());

      Source s;
      s.id = "s" + std::to_string(nextSourceId_++);
      s.kind = *kind;
      s.address = j["address"];
      bridge_.sources.push_back(std::move(s));
      return json{{"ok", true}, {"id", bridge_.sources.back().id}}.dump();
    }

    if (cmd == "remove-source") {
      if (!j.contains("id") || !j["id"].is_string()) return refuse("remove-source needs a text \"id\"");
      const std::string id = j["id"];
      if (bridge_.findSource(id) == nullptr) return refuse("remove-source: no such source: " + id);
      for (Output& o : bridge_.outputs)
        if (o.sourceId == id) o.sourceId.clear();
      bridge_.sources.erase(
          std::remove_if(bridge_.sources.begin(), bridge_.sources.end(),
                         [&](const Source& s) { return s.id == id; }),
          bridge_.sources.end());
      return json{{"ok", true}}.dump();
    }

    if (cmd == "add-output") {
      if (j.contains("sourceId") && !j["sourceId"].is_string())
        return refuse("add-output: \"sourceId\" must be text");
      if (j.contains("name") && !j["name"].is_string())
        return refuse("add-output: \"name\" must be text");
      Output o;
      o.id = "o" + std::to_string(nextOutputId_++);
      o.sourceId = j.value("sourceId", std::string{});
      o.name = j.value("name", std::string{"Output " + o.id});
      bridge_.outputs.push_back(std::move(o));
      return json{{"ok", true}, {"id", bridge_.outputs.back().id}}.dump();
    }

    if (cmd == "set-output-source") {
      if (!j.contains("id") || !j["id"].is_string()) return refuse("set-output-source needs a text \"id\"");
      if (j.contains("sourceId") && !j["sourceId"].is_string())
        return refuse("set-output-source: \"sourceId\" must be text");
      Output* o = bridge_.findOutput(j["id"]);
      if (o == nullptr) return refuse("set-output-source: no such output: " + j["id"].get<std::string>());
      const std::string sid = j.value("sourceId", std::string{});
      if (!sid.empty() && bridge_.findSource(sid) == nullptr)
        return refuse("set-output-source: no such source: " + sid);
      o->sourceId = sid;
      return json{{"ok", true}}.dump();
    }

    return refuse("unknown command: " + cmd);
  } catch (const json::exception&) {
    return refuse("malformed command");
  }
}

}  // namespace ob
```

- [ ] **Step 5: Wire into `CMakeLists.txt` and run**

Three changes: add `src/engine/control_server.cpp` and `src/engine/command_router.cpp` to **`ob_engine`**, and `tests/command_router_test.cpp` to `ob_tests`. `ob_engine` already links `ixwebsocket::ixwebsocket` (the control server's transport) and `bcrypt` (its token generator), so no link change is needed.

Run:
```powershell
cmake --preset release; cmake --build --preset release; .\build\release\ob_tests.exe
```
Expected: `[doctest] Status: SUCCESS!` — the nine router cases pass alongside the existing forty-one.

- [ ] **Step 6: Commit**

```bash
git add src/engine/control_server.h src/engine/control_server.cpp src/engine/command_router.h src/engine/command_router.cpp tests/command_router_test.cpp CMakeLists.txt
git commit -m "feat(engine): control server and command router"
```

---

## Task 14: Supervisor and process launcher

**Files:**
- Create: `src/shell/supervisor.h`, `src/shell/supervisor.cpp`, `src/shell/process_launcher.h`, `src/shell/process_launcher.cpp`, `tests/supervisor_test.cpp`

- [ ] **Step 1: Lift both pairs from BreadCamera**

Run:
```powershell
Copy-Item ..\BreadCamera\src\shell\supervisor.h src\shell\supervisor.h
Copy-Item ..\BreadCamera\src\shell\supervisor.cpp src\shell\supervisor.cpp
Copy-Item ..\BreadCamera\src\shell\process_launcher.h src\shell\process_launcher.h
Copy-Item ..\BreadCamera\src\shell\process_launcher.cpp src\shell\process_launcher.cpp
Get-ChildItem src\shell\*.h, src\shell\*.cpp | ForEach-Object {
  (Get-Content $_) -replace '\bbc\b','ob' | Set-Content $_
}
```

- [ ] **Step 2: Write the failing test `tests/supervisor_test.cpp`**

```cpp
#include <doctest/doctest.h>

#include "shell/supervisor.h"

using Clock = ob::Supervisor::Clock;

struct FakeLauncher : ob::EngineLauncher {
  bool running = false;
  int launched = 0, killed = 0;
  bool isRunning() override { return running; }
  void launch() override { ++launched; running = true; }
  void kill() override { ++killed; running = false; }
};

TEST_CASE("the supervisor launches a dead engine and relaunches it while it stays dead") {
  FakeLauncher l;  // not running
  ob::Supervisor sup(l);
  const auto t0 = Clock::now();

  sup.poll(t0);
  CHECK(l.launched == 1);

  // launch() sets running = true, so clear it: the engine has since exited and must read as dead.
  l.running = false;
  sup.poll(t0 + std::chrono::seconds(1));  // a dead engine is relaunched at once
  CHECK(l.launched == 2);
}

TEST_CASE("the startup grace protects a slow-starting engine from the heartbeat timeout") {
  FakeLauncher l;
  l.running = true;  // launched, but it has not sent a heartbeat yet
  ob::Supervisor sup(l);
  const auto t0 = Clock::now();

  sup.poll(t0);                             // sets the baseline
  sup.poll(t0 + std::chrono::seconds(5));   // 5 s > the 3 s heartbeat timeout, but < the 8 s grace
  CHECK(l.killed == 0);

  sup.poll(t0 + std::chrono::seconds(9));   // past the grace, heartbeat still stale
  CHECK(l.killed == 1);
}

TEST_CASE("a fresh heartbeat keeps a running engine from being killed") {
  FakeLauncher l;
  l.running = true;
  ob::Supervisor sup(l);
  const auto t0 = Clock::now();

  sup.poll(t0);
  sup.heartbeat(t0 + std::chrono::seconds(8));
  sup.poll(t0 + std::chrono::seconds(9));  // past the grace, but the beat is only 1 s old
  CHECK(l.killed == 0);
}

// The policy's headline behaviour: five quick restarts inside the window, then one per 10 s.
TEST_CASE("rapid relaunches are throttled to the slow interval once the budget is spent") {
  FakeLauncher l;  // dead throughout
  ob::Supervisor sup(l);
  const auto t0 = Clock::now();

  for (int i = 0; i < 5; ++i) {
    sup.poll(t0 + std::chrono::seconds(i));  // 1 s apart, inside 60 s
    l.running = false;                       // launch() marks it running; it dies again at once
  }
  CHECK(l.launched == 5);
  CHECK(sup.crashLooping(t0 + std::chrono::seconds(5)));

  sup.poll(t0 + std::chrono::seconds(6));  // inside the window and the slow interval: throttled
  CHECK(l.launched == 5);

  sup.poll(t0 + std::chrono::seconds(15));  // 11 s since the last attempt: one more is allowed
  CHECK(l.launched == 6);
}
```

> These three cases encode the lifted `Supervisor`'s actual policy, which the earlier draft of this
> task got wrong. Two things to keep straight: (a) the **startup grace protects a *running*
> engine** from the stale-heartbeat kill — it does **not** delay relaunching a *dead* one, which is
> relaunched on the next `poll()`; and (b) `heartbeat()` only refreshes `lastBeat_`, so it does
> **not** reset the restart budget (the `launchTimes_` window). If the committed code disagrees with
> any assertion here, the code is the authority — adapt the test to it and say so in your report,
> rather than changing the lifted policy.

- [ ] **Step 3: Add `ob_shell` to `CMakeLists.txt` and run**

```cmake
# ---- shell ----
add_library(ob_shell STATIC src/shell/supervisor.cpp src/shell/process_launcher.cpp)
target_include_directories(ob_shell PUBLIC src)
target_link_libraries(ob_shell PUBLIC ob_core)
```

Add `tests/supervisor_test.cpp` to `ob_tests` and add `target_link_libraries(ob_tests PRIVATE ob_shell)`. (`process_launcher.cpp` uses nlohmann, which reaches it transitively: `ob_core` links `nlohmann_json::nlohmann_json` as `PUBLIC`. No other library is needed — BreadCamera's `bc_shell` links only `bc_core`.)

Run:
```powershell
cmake --preset release; cmake --build --preset release; .\build\release\ob_tests.exe
```
Expected: `[doctest] Status: SUCCESS!` — the four supervisor cases pass alongside the existing fifty.

- [ ] **Step 4: Commit**

```bash
git add src/shell CMakeLists.txt tests/supervisor_test.cpp
git commit -m "feat(shell): supervisor and process launcher, lifted from BreadCamera"
```

---

## Task 15: Engine main, and the UI-close guarantee

**Files:**
- Create: `src/engine/engine.h`, `src/engine/engine.cpp`, `tests/engine_test.cpp`
- Modify: `src/engine/engine_main.cpp`

- [ ] **Step 1: Write the failing test `tests/engine_test.cpp`**

```cpp
#include <doctest/doctest.h>

#include <chrono>
#include <thread>

#include "engine/engine.h"

using namespace std::chrono_literals;

static ob::Bridge twoOutputsOneSource() {
  ob::Bridge b;
  b.name = "Test";
  b.sources.push_back(ob::Source{.id = "s1", .kind = ob::SourceKind::Omt, .address = "PC"});
  b.outputs.push_back(ob::Output{.id = "o1", .sourceId = "s1", .target = ob::OutputTarget::Omt, .name = "A"});
  b.outputs.push_back(ob::Output{.id = "o2", .sourceId = "s1", .target = ob::OutputTarget::OmtHx, .name = "B"});
  return b;
}

TEST_CASE("applying a bridge starts rows and refs the hub") {
  ob::Engine engine(0, "token");  // port 0: do not bind, tests drive apply() directly
  engine.apply(twoOutputsOneSource());
  REQUIRE(engine.graph().hub("s1") != nullptr);
  CHECK(engine.graph().hub("s1")->refs() == 2);
  CHECK(engine.graph().rowCount() == 2u);
}

TEST_CASE("outputs survive a control-channel disconnect (the UI-close guarantee)") {
  ob::Engine engine(0, "token");
  engine.apply(twoOutputsOneSource());
  engine.start();

  engine.onControlClientDisconnected(ob::ClientId{"c1"});
  std::this_thread::sleep_for(10ms);

  // The graph is owned by the engine, not by the connection, so a disconnect must leave demand and
  // rows exactly as they were. (framesPublished cannot move here — M1 has no adapter publishing —
  // so assert the invariant that can actually fail if a disconnect tore the graph down.)
  CHECK(engine.graph().hub("s1")->running());
  CHECK(engine.graph().hub("s1")->refs() == 2);
  CHECK(engine.graph().rowCount() == 2u);
  engine.stop();
}
```

- [ ] **Step 2: Write `src/engine/engine.h`**

```cpp
#pragma once
// The headless engine (design 6.1, 6.6): owns the graph, the bridge file and the control
// server. Closing the UI drops a control connection; it must never touch the graph.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "core/bridge_model.h"
#include "engine/bridge_graph.h"
#include "engine/command_router.h"
#include "engine/control_server.h"

namespace ob {

class Engine {
 public:
  Engine(int controlPort, std::string token, std::string bridgeFile = {});
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Builds the graph from the model. Safe to call repeatedly, but it REPLACES the router's model
  // (discarding anything commands minted) and re-seeds the id counters — so it is a startup call:
  // run it on the same thread as start()/stop(), never from a command, adaptor or deliver thread.
  void apply(const Bridge& bridge);

  // Starts the graph and, when controlPort > 0, the control server. On an AUTHORISED open the
  // server pushes a full `state` reply before any command, so a UI should treat its first message
  // as its initial snapshot rather than the answer to something it sent.
  void start();
  void stop();

  BridgeGraph& graph() { return graph_; }

  // Called when a UI disconnects. Deliberately does not stop the graph (design 6.1, 1.1).
  void onControlClientDisconnected(const ClientId& client);

  uint64_t controlConnections() const { return controlConnections_; }

 private:
  std::string handleCommand(const ClientId& client, const std::string& text);

  int controlPort_;
  std::string token_;
  std::string bridgeFile_;
  // Serialises command handling. ixwebsocket runs each connection on its own thread, so two
  // clients (or a supervisor restart overlapping a live UI) would otherwise race the router's
  // model/counter — and would call BridgeGraph::apply() from two threads, breaking its documented
  // one-owner-thread rule (it holds raw row/hub pointers across a deliberate unlock).
  std::mutex controlMu_;
  CommandRouter router_;
  BridgeGraph graph_{[](const std::shared_ptr<const Frame>&) {}};
  std::unique_ptr<ControlServer> server_;
  std::atomic<uint64_t> controlConnections_{0};
};

}  // namespace ob
```

- [ ] **Step 3: Write `src/engine/engine.cpp`**

```cpp
#include "engine/engine.h"

#include <exception>
#include <utility>

#include "core/bridge_store.h"

namespace ob {

Engine::Engine(int controlPort, std::string token, std::string bridgeFile)
    : controlPort_(controlPort), token_(std::move(token)), bridgeFile_(std::move(bridgeFile)) {}

Engine::~Engine() { stop(); }

void Engine::apply(const Bridge& bridge) {
  router_.setBridge(bridge);
  graph_.apply(bridge);
}

void Engine::start() {
  graph_.start();
  if (controlPort_ > 0 && server_ == nullptr) {
    server_ = std::make_unique<ControlServer>(controlPort_, token_);
    ControlServer::Handlers h;
    h.open = [this](const ClientId& c) {
      ++controlConnections_;
      if (server_) server_->send(c, handleCommand(c, R"({"cmd":"state"})"));
    };
    h.close = [this](const ClientId& c) { onControlClientDisconnected(c); };
    h.text = [this](const ClientId& c, const std::string& t) { return handleCommand(c, t); };
    server_->setHandlers(std::move(h));
    server_->start();
  }
}

void Engine::stop() {
  if (server_) {
    server_->stop();
    server_.reset();
  }
  graph_.stop();
}

void Engine::onControlClientDisconnected(const ClientId&) {
  // The graph is owned by the engine, not by a connection. Nothing to do, by design.
}

std::string Engine::handleCommand(const ClientId& client, const std::string& text) {
  // Commands arrive on per-connection ixwebsocket threads. Serialise the model edit together with
  // the reconcile for two reasons: the router's model and id counters are plain members, and
  // BridgeGraph::apply() is the single writer of hub demand — it holds raw row/hub pointers across
  // a deliberate unlock, so a second apply() from another thread could dangle them.
  std::lock_guard l(controlMu_);
  try {
    const std::string reply = router_.handle(client, text);
    // Every command may have edited the router's model (add-source, remove-source, add-output, …),
    // so reconcile the graph with it here. Applying unconditionally needs no per-command list:
    // apply() moves demand by delta, so an unchanged model costs nothing and fires no transition.
    // Without this the graph never learns about an edit, and nothing would actually start or stop.
    graph_.apply(router_.bridge());
    return reply;
  } catch (const std::exception&) {
    // The router itself never throws, but apply() can (thread creation, allocation). An exception
    // escaping here would reach an ixwebsocket callback thread and call std::terminate, so refuse.
    return R"({"error":"internal error","ok":false})";
  }
}

}  // namespace ob
```

- [ ] **Step 4: Replace `src/engine/engine_main.cpp`**

```cpp
// OMT-Bridge-Engine.exe: headless. Owns the sources, outputs and bridge graph; keeps them
// running when the UI is closed (design 6.1).
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#include <ixwebsocket/IXNetSystem.h>

#include "core/bridge_store.h"
#include "engine/engine.h"

namespace {

HANDLE g_stopEvent = nullptr;

BOOL WINAPI onConsoleControl(DWORD) {
  if (g_stopEvent) SetEvent(g_stopEvent);
  return TRUE;  // handled, so the default handler does not kill us mid-frame
}

}  // namespace

int main(int argc, char** argv) {
  // Winsock must be initialised before ixwebsocket creates a socket, or listen() fails with
  // WSA 10093 (WSANOTINITIALISED). BreadCamera's engine_main does exactly this; ixwebsocket does
  // not initialise itself.
  if (!ix::initNetSystem()) {
    std::fprintf(stderr, "Winsock initialisation failed\n");
    return 1;
  }

  int port = 0;
  std::string token, bridgeFile;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    if (k == "--port") port = std::atoi(argv[i + 1]);
    else if (k == "--token") token = argv[i + 1];
    else if (k == "--bridge") bridgeFile = argv[i + 1];
  }
  // A port outside 1..65535 is a usage error, not "run without a control channel": atoi("-1") and
  // atoi("70000") are non-zero, so a `== 0` check would let them through and the engine would
  // bridge with no way to command or stop it.
  if (port < 1 || port > 65535) {
    std::fprintf(stderr, "usage: omt-bridge-engine --port <n> [--token <s>] [--bridge <file>]\n");
    return 2;
  }
  // The token is the only guard on the loopback control channel, so it must come from a CSPRNG.
  // Reuse the lifted randomToken() (BCryptGenRandom); std::rand() is unseeded and predictable.
  if (token.empty()) token = ob::randomToken();

  ob::Engine engine(port, token, bridgeFile);

  if (!bridgeFile.empty()) {
    if (auto b = ob::loadBridge(bridgeFile)) {
      engine.apply(*b);
    } else {
      std::fprintf(stderr, "warning: could not read %s; starting with an empty bridge\n",
                   bridgeFile.c_str());
    }
  }

  try {
    engine.start();
  } catch (const std::exception& e) {
    // A bind/listen failure surfaces here (ControlServer::start throws). Report it: an uncaught
    // exception would terminate with no message, which a supervisor sees as an unexplained loop.
    std::fprintf(stderr, "engine start failed: %s\n", e.what());
    return 1;
  }

  // Report readiness on stdout; the UI reads this first line.
  std::printf("READY %s\n", token.c_str());
  std::fflush(stdout);

  // Run until the process is asked to stop. Deliberately NOT tied to stdin: the UI launches the
  // engine without a console and may close at any moment, and design 6.1 says closing the window
  // leaves the engine bridging. Ctrl+C (or a console close) ends it cleanly.
  g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_stopEvent != nullptr) {
    SetConsoleCtrlHandler(onConsoleControl, TRUE);
    WaitForSingleObject(g_stopEvent, INFINITE);
    SetConsoleCtrlHandler(onConsoleControl, FALSE);
    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
  } else {
    Sleep(INFINITE);  // could not arm the handler: keep bridging until terminated
  }

  engine.stop();
  return 0;
}
```

- [ ] **Step 5: Wire the engine target, build and run**

Three changes:
- add `src/engine/engine.cpp` to **`ob_engine`**;
- add `tests/engine_test.cpp` to `ob_tests` (which already links `ob_engine`);
- make the executable link the engine library — it currently links nothing, so `ob::Engine` would be undefined:
```cmake
target_link_libraries(omt-bridge-engine PRIVATE ob_engine)
```
(`ob_engine` is `PUBLIC ob_core ixwebsocket::ixwebsocket`, so the exe gets the graph, the router, the control server and its transport; `ob_copy_runtime(omt-bridge-engine)` already copies the OMT DLLs.)

Run:
```powershell
cmake --preset release; cmake --build --preset release; .\build\release\ob_tests.exe
```
Expected: `[doctest] Status: SUCCESS!` — the two new cases pass alongside the existing fifty-four.

- [ ] **Step 6: Prove the two processes talk**

Run in one shell:
```powershell
.\build\release\omt-bridge-engine.exe --port 5990 --token deadbeef --bridge build\release\test.ombridge.json
```
Expected: `READY deadbeef`.

In another shell run a WebSocket client against `ws://127.0.0.1:5990/?token=deadbeef` and send `{"cmd":"state"}`.
Expected: a JSON reply containing `"ok":true`. Sending with a wrong token gets no reply / a rejected connection.

Then stop the engine with **Ctrl+C** in its shell, and check it exits 0. (It deliberately does not
stop when stdin or the UI disappears — design 6.1 — so there is no "type quit" path.)

- [ ] **Step 7: Commit**

```bash
git add src/engine/engine.h src/engine/engine.cpp src/engine/engine_main.cpp tests/engine_test.cpp CMakeLists.txt
git commit -m "feat(engine): headless engine, bridge load, and the UI-close guarantee"
```

**M1 is complete when:** `ob_tests.exe` passes all tasks' tests, and `omt-bridge-engine.exe` answers `{"cmd":"state"}` over the token-authenticated loopback channel while its graph keeps running after a client disconnects.

---

### Notes for M2 — the launcher is lifted but deliberately not wired

Task 14 lifted BreadCamera's `ProcessLauncher`/`Supervisor` verbatim, and this milestone exercises only the `Supervisor` (by tests). Nothing launches an engine in M1 — the UI is a stub — so three BreadCamera-specific assumptions in the lift are **not yet** re-pointed. A code-quality review found them; they are silent at runtime rather than compile errors, so M2 must address them before the UI adopts an engine:

1. **BreadCamera's identity is hard-coded.** `isEngineProcess(pid)` matches `L"breadcamera-engine.exe"` and `engineInstanceRunning()` opens `L"Local\\BreadCameraEngine"`. Both are free functions with no parameters, so no caller can re-point them without editing the lift. Until they are aimed at `omt-bridge-engine.exe` and a `Local\OMTBridgeEngine` mutex (parameterise them, or add OMT-Bridge wrappers), an adoption check silently returns false and the UI would launch a *second* engine every time.
2. **The discovery contract has no producer.** `readDiscovery`/`waitForDiscovery` parse `{"pid":u32,"port":int,"token":str}` from a file, but this milestone's engine writes no such file — it prints `READY <token>` on stdout and reads `--port/--token/--bridge`. M2 must pick one: either add a `--discovery <file>` writer (atomic tmp + rename, as BreadCamera's engine does) and point the launcher at `%LOCALAPPDATA%\OMTBridge\engine.json`, or drive pid/port/token from the `READY` line and drop the file path.
3. **The engine takes no single-instance mutex.** BreadCamera's engine creates `Local\BreadCameraEngine` and exits 3 if it already exists; `engineInstanceRunning()` exists only to notice that mutex. Without it, two engines can bind the same port. M2 adds the mutex (with the OMT-Bridge name) before the engine starts listening.

Two smaller M2 items: `Supervisor::poll` holds its mutex across the virtual `launcher_.launch()`/`kill()` calls (a `kill()` can block ~2 s in `WaitForSingleObject`), so poll from a dedicated thread and keep `waitForDiscovery` out of `launch()`; and delete `ProcessLauncher`'s copy constructor — it owns a raw `HANDLE` and a copy would `CloseHandle` it twice.

Two tests to add in M2 alongside those: a discovery round trip for OMT-Bridge's own schema, and `stopSupervising()` halting relaunches.

### M2-gating API work (from the final branch review)

These are not M1 defects — nothing in M1 can reach them — but each is far cheaper to change before the first adapter exists than after. Land them with M2's first adapter:

1. **There is no way to install `onRunningChanged` before a hub's first transition.** `apply()` creates the hub and moves its demand in the same call, so the "install before the first demand change" contract is unsatisfiable for hubs `apply` creates; and because demand moves by delta, a later `apply` fires nothing. Also `BridgeGraph::stop()`, and `engine_main`'s `apply()`-before-`start()` order, mean an M2 adapter started by that callback could run before the graph/server are up and could survive `Engine::stop()`. Fix: a hub-created hook (or a factory in the `BridgeGraph` ctor), gate phase-2 transitions on `started_`, and have `stop()` release the refs it added and `start()` re-add them.
2. **A row cannot revive after a sink fault, and nothing reads the fault.** `OutputRow::start()` early-returns while `thread_.joinable()`, which is still true after the worker exited on a fault — so `apply()`'s `row->start()` silently does nothing, and `faulted()` has no reader outside tests, so the promised "mark that output Offline" is unimplemented. Fix a health/restart path, but do not restart on every `apply()` — `state` calls `apply()`, so that would restart a faulted row on every query.
3. **The per-output sink cannot be built from the current shape.** `BridgeGraph` takes one shared `Deliver` with no output identity, rows carry only `id`/`sourceId`, and `apply()` copies only `sourceId` — `target`/`codec`/`quality`/`alpha`/`name` never reach a row and the config `operator==` is never used to diff. M2 needs a per-output factory and a config diff.
4. **Two functions named `publish`, and a raw hub pointer with a short life.** `SourceHub::publish` only stores the slot; fan-out lives in `BridgeGraph::publish`. An adapter calling `hub->publish(frame)` sees `framesPublished()` climb and no output ever receives a frame. Worse, `BridgeGraph::hub()` returns a pointer into a map that `apply()` erases, so a cached `SourceHub*` is a use-after-free at the next source edit. Make `SourceHub::publish` private (or rename it `storeFrame`) and have adapters publish by source id through the graph.
5. **Runtime state has no path into the `state` reply.** The router reports only its own config model; `Source::state`/`format` and `Output::state` are written nowhere. The graph owns liveness, the router owns the model, and nothing connects them — so no signal state, format, stats or thumbnail can be reported until one does.

Cheap follow-ups also worth taking early: check the stream after `close()` in `saveBridge` and remove the `.tmp` on a rename failure; take `controlMu_` in `Engine::apply` (M4 will load a bridge at runtime); unit-test `tokenMatches`/`tokenFromUri`/`randomToken` (the only guard on the control channel) and make the token length floor consistent; reject duplicate ids in a hand-edited bridge file; bound `ControlServer::send`'s send timeout when a UI exists.

---

## What this plan does not cover

M2 (OMT/OMT-HX in and out), M3 (NDI), M4 (the Qt UI proper) and M5 (hardening) each get their own plan. In this plan the `OutputRow` sink is a `std::function` and `SourceHub::publish` is driven by tests; M2 replaces both with the real `OmtSource`/`OmtSink` adapters from the spec's Section 8.