cmake_minimum_required(VERSION 3.28)
project(BMBRemote LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# libsrt: vcpkg's port on Windows ships headers and srt.lib but no CMake config, so find it directly.
# Debian's libsrt-openssl-dev ships srt.pc.
if(WIN32)
  find_path(BMB_SRT_INCLUDE_DIR NAMES srt/srt.h REQUIRED)
  find_library(BMB_SRT_LIBRARY NAMES srt REQUIRED)
  add_library(bmb_srt INTERFACE)
  target_include_directories(bmb_srt INTERFACE "${BMB_SRT_INCLUDE_DIR}")
  target_link_libraries(bmb_srt INTERFACE "${BMB_SRT_LIBRARY}" ws2_32)
  set(BMB_SRT_TARGET bmb_srt)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SRT REQUIRED IMPORTED_TARGET srt)
  set(BMB_SRT_TARGET PkgConfig::SRT)
endif()

find_package(doctest CONFIG QUIET)
if(doctest_FOUND)
  set(BMB_DOCTEST_TARGET doctest::doctest)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DOCTEST REQUIRED IMPORTED_TARGET doctest)
  set(BMB_DOCTEST_TARGET PkgConfig::DOCTEST)
endif()

add_library(bmb_net STATIC
  src/stream_id.cpp
  src/srt_util.cpp
  src/bench_payload.cpp
  src/bench.cpp
)
target_include_directories(bmb_net PUBLIC src)
target_link_libraries(bmb_net PUBLIC ${BMB_SRT_TARGET})

add_executable(remote_tests
  tests/main.cpp
  tests/stream_id_test.cpp
  tests/srt_util_test.cpp
  tests/bench_test.cpp
)
target_link_libraries(remote_tests PRIVATE bmb_net ${BMB_DOCTEST_TARGET})
# Keep the programs in the build root: scripts/test.ps1 and BENCHMARK.md run them from there.
set_target_properties(remote_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(srt-bench apps/srt_bench.cpp)
target_link_libraries(srt-bench PRIVATE bmb_net)
set_target_properties(srt-bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")