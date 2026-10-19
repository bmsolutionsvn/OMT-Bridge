# OMT-HX is a submodule under third_party/; OMTHX_ROOT lets a developer point at a sibling
# checkout instead, exactly like BreadCamera does.
include_guard(GLOBAL)
set(OMTHX_ROOT "${CMAKE_SOURCE_DIR}/third_party/OMT-HX" CACHE PATH "OMT-HX checkout")
if(EXISTS "${OMTHX_ROOT}/include/omthx/omthx.h" AND EXISTS "${OMTHX_ROOT}/omt/libomt/libomt.h")
  add_subdirectory("${OMTHX_ROOT}" omthx-build EXCLUDE_FROM_ALL)
  set(OB_WITH_OMT ON)
else()
  message(FATAL_ERROR "OMT-HX is missing at ${OMTHX_ROOT}. Run: git submodule update --init --recursive")
endif()
