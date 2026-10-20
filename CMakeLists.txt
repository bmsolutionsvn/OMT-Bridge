cmake_minimum_required(VERSION 3.28)
project(OMTBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(/W4 /permissive- /utf-8 /EHsc)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

find_package(doctest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ixwebsocket CONFIG REQUIRED)

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
  if(OB_WITH_NDI)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NDI_DLL}" "$<TARGET_FILE_DIR:${target}>")
  endif()
endfunction()

# Optional NDI 6 SDK (spec 14.3). Absent in CI, so OB_WITH_NDI defaults to whatever is found.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(NDI)
option(OB_WITH_NDI "Build the NDI adapters" ${NDI_FOUND})
if(OB_WITH_NDI AND NOT NDI_FOUND)
  message(WARNING "OB_WITH_NDI=ON but the NDI 6 SDK was not found; turning it off")
  set(OB_WITH_NDI OFF)
endif()
message(STATUS "OB_WITH_NDI=${OB_WITH_NDI}")

# ---- core ----
# The shared, UI-free core library. Every later task adds its .cpp to this list.
add_library(ob_core STATIC
  src/core/signal_state.cpp
  src/core/bridge_model.cpp
  src/core/bridge_store.cpp
  src/core/alpha_gate.cpp
  src/core/alpha_convert.cpp
)
target_include_directories(ob_core PUBLIC src)
# PUBLIC: signal_state.h includes <nlohmann/json.hpp>, and every later core header that
# does the same must pass that include path on to whoever links ob_core.
target_link_libraries(ob_core PUBLIC nlohmann_json::nlohmann_json)

# ---- engine ----
add_library(ob_engine STATIC
  src/engine/source_hub.cpp   # Tasks 11-15 append output_row, bridge_graph, command_router, control_server, engine
  src/engine/omt_source.cpp
  src/engine/omt_sink.cpp
  src/engine/output_row.cpp
  src/engine/bridge_graph.cpp
  src/engine/control_server.cpp
  src/engine/command_router.cpp
  src/engine/engine.cpp
)
target_include_directories(ob_engine PUBLIC src)
target_link_libraries(ob_engine PUBLIC ob_core ixwebsocket::ixwebsocket)
target_link_libraries(ob_engine PUBLIC omthx)
# bcrypt is a Windows SDK library (bcrypt.lib, in the VS environment's default lib paths), not a
# vcpkg port — exactly as BreadCamera's bc_engine links it. control_server uses it in Task 13.
target_link_libraries(ob_engine PUBLIC bcrypt)

# NDI is optional (spec 14.3): the adapters compile only when the SDK was found.
if(OB_WITH_NDI)
  target_sources(ob_engine PRIVATE src/engine/ndi_runtime.cpp src/engine/ndi_source.cpp
                               src/engine/ndi_sink.cpp)
  target_link_libraries(ob_engine PUBLIC NDI::NDI)
  target_compile_definitions(ob_engine PUBLIC OB_WITH_NDI=1)
endif()

# ---- shell ----
add_library(ob_shell STATIC src/shell/supervisor.cpp src/shell/process_launcher.cpp)
target_include_directories(ob_shell PUBLIC src)
target_link_libraries(ob_shell PUBLIC ob_core)

# ---- tests ----
add_executable(ob_tests
  tests/main.cpp
  tests/audio_test.cpp
  tests/frame_test.cpp
  tests/signal_state_test.cpp
  tests/bounded_queue_test.cpp
  tests/bridge_model_test.cpp
  tests/bridge_store_test.cpp
  tests/source_hub_test.cpp
  tests/output_row_test.cpp
  tests/omt_sink_test.cpp
  tests/bridge_graph_test.cpp
  tests/alpha_gate_test.cpp
  tests/alpha_convert_test.cpp
  tests/command_router_test.cpp
  tests/supervisor_test.cpp
  tests/engine_test.cpp
  tests/ndi_runtime_test.cpp
  tests/ndi_sink_test.cpp
)
target_include_directories(ob_tests PRIVATE tests src)
target_link_libraries(ob_tests PRIVATE doctest::doctest)
target_link_libraries(ob_tests PRIVATE omthx)
target_link_libraries(ob_tests PRIVATE ob_core)
target_link_libraries(ob_tests PRIVATE ob_engine)
target_link_libraries(ob_tests PRIVATE ob_shell)
ob_copy_runtime(ob_tests)

# ---- shells ----
add_executable(omt-bridge-engine src/engine/engine_main.cpp)
target_link_libraries(omt-bridge-engine PRIVATE ob_engine)
ob_copy_runtime(omt-bridge-engine)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
set(CMAKE_AUTOMOC ON)
add_executable(omt-bridge WIN32 src/ui/main.cpp src/ui/main_window.cpp src/ui/main_window.h)
# The UI includes "ui/main_window.h" (project-root relative), so src must be on the include path.
target_include_directories(omt-bridge PRIVATE src)
target_link_libraries(omt-bridge PRIVATE Qt6::Widgets)

# Deploy Qt's platform plugin beside the executable. Without it Qt6Core falls back to the plugin
# path baked into this vcpkg build, so the UI starts only on the build machine; copied anywhere
# else it dies with "This application failed to start because no Qt platform plugin could be
# initialized". vcpkg's Qt does not export a plugin target, so the file is located from Qt6_DIR:
# <prefix>/share/Qt6 -> <prefix>/Qt6/plugins/platforms.
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
