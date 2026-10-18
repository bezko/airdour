# Airdour win64 / MSVC first-slice spine
#
# This does NOT replace waf yet. It documents the compile order and lists the
# WASAPI shared-mode sources a Windows agent should wire into real libraries.
#
# Audio product path: WASAPI shared (libs/backends/wasapi).
# Exclusive / DirectSound / ASIO: out of scope.

cmake_minimum_required(VERSION 3.20)
project(airdour_win64 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(WARNING "cmake/win64 is intended for MSVC/Win64; configuring docs/placeholders only.")
endif()

add_compile_definitions(
  PLATFORM_WINDOWS=1
  AIRDOUR_WINDOWS_ONLY=1
  WIN32=1
  _USE_MATH_DEFINES=1
  USE_CAIRO_IMAGE_SURFACE=1
)

set(AIRDOUR_DEPS_ROOT "" CACHE PATH "MSVC dependency root (glib, sndfile, …)")
if(AIRDOUR_DEPS_ROOT)
  list(APPEND CMAKE_PREFIX_PATH "${AIRDOUR_DEPS_ROOT}")
endif()

set(AIRDOUR_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# --- Slice order ---
# 1. pbd  2. temporal / midipp / evoral  3. ardour  4. dummy + wasapi  5. headless

set(WASAPI_BACKEND_SOURCES
  "${AIRDOUR_SRC_ROOT}/libs/backends/wasapi/wasapi_io.cc"
  "${AIRDOUR_SRC_ROOT}/libs/backends/wasapi/wasapi_backend.cc"
)

add_custom_target(airdour_slice_notes
  COMMAND ${CMAKE_COMMAND} -E echo "Build order: pbd -> temporal/midi/evoral -> ardour -> dummy+wasapi -> hardour"
  COMMAND ${CMAKE_COMMAND} -E echo "WASAPI sources:"
  COMMAND ${CMAKE_COMMAND} -E echo "  ${WASAPI_BACKEND_SOURCES}"
)

# When configuring on Windows with deps present, grow this into add_library(... SHARED).
add_library(airdour_wasapi_backend INTERFACE)
target_sources(airdour_wasapi_backend INTERFACE ${WASAPI_BACKEND_SOURCES})
target_include_directories(airdour_wasapi_backend INTERFACE
  "${AIRDOUR_SRC_ROOT}/libs/backends/wasapi"
  "${AIRDOUR_SRC_ROOT}/libs/ardour"
  "${AIRDOUR_SRC_ROOT}/libs/pbd"
)
target_compile_definitions(airdour_wasapi_backend INTERFACE
  BACKEND_NAME=\"WASAPI\"
  ARDOURBACKEND_DLL_EXPORTS
  USE_MMCSS_THREAD_PRIORITIES
)
if(WIN32)
  target_link_libraries(airdour_wasapi_backend INTERFACE ole32 winmm uuid avrt)
endif()

message(STATUS "Airdour win64: WASAPI shared-mode sources registered (INTERFACE until deps link)")
