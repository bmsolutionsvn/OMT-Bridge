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
endfunction()

# ---- tests ----
# frame.h is header-only, so nothing links yet; ob_core is created in Task 6.
add_executable(ob_tests
  tests/main.cpp
  tests/frame_test.cpp
)
target_include_directories(ob_tests PRIVATE tests src)
target_link_libraries(ob_tests PRIVATE doctest::doctest)
target_link_libraries(ob_tests PRIVATE omthx)
ob_copy_runtime(ob_tests)
