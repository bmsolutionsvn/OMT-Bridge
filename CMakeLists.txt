cmake_minimum_required(VERSION 3.28)
project(OMTBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(/W4 /permissive- /utf-8 /EHsc)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

find_package(doctest CONFIG REQUIRED)

# FFmpeg comes from vcpkg; its ffmpeg package provides FindFFMPEG.cmake, so no module is vendored.
find_package(FFMPEG REQUIRED)
include(cmake/omt-runtime.cmake)  # defines the omthx target and omthx_copy_runtime()

# Copies the DLLs vcpkg does not manage (libomt, libvmx, and NDI when enabled) beside an exe.
function(ob_copy_runtime target)
  if(OB_WITH_OMT)
    omthx_copy_runtime(${target})
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

# ---- tests ----
# ob_core is created in Task 6; until then ob_tests links only doctest and omthx.
add_executable(ob_tests
  tests/main.cpp
  tests/frame_test.cpp
)
target_include_directories(ob_tests PRIVATE tests src)
target_link_libraries(ob_tests PRIVATE doctest::doctest)
target_link_libraries(ob_tests PRIVATE omthx)
ob_copy_runtime(ob_tests)

# ---- shells ----
add_executable(omt-bridge-engine src/engine/engine_main.cpp)
ob_copy_runtime(omt-bridge-engine)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
set(CMAKE_AUTOMOC ON)
add_executable(omt-bridge WIN32 src/ui/main.cpp src/ui/main_window.cpp src/ui/main_window.h)
# The UI includes "ui/main_window.h" (project-root relative), so src must be on the include path.
target_include_directories(omt-bridge PRIVATE src)
target_link_libraries(omt-bridge PRIVATE Qt6::Widgets)
