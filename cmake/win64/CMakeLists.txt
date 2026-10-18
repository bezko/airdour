# Airdour win64 / MSVC first-slice spine
#
# This does NOT replace waf yet. It sketches the compile order a Windows agent
# should grow into a full MSVC build. Requires Windows SDK + dependency prefix.
#
# First audio product path: WASAPI shared mode (libs/backends/wasapi).
# Exclusive / DirectSound / ASIO: out of scope.

cmake_minimum_required(VERSION 3.20)
project(airdour_win64 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(WARNING "cmake/win64 is intended for MSVC/Win64; configuring stubs only.")
endif()

add_compile_definitions(
  PLATFORM_WINDOWS=1
  AIRDOUR_WINDOWS_ONLY=1
  WIN32=1
  _USE_MATH_DEFINES=1
  USE_CAIRO_IMAGE_SURFACE=1
)

# Dependency prefix (vcpkg toolchain or -DAIRDOUR_DEPS_ROOT=...)
set(AIRDOUR_DEPS_ROOT "" CACHE PATH "MSVC dependency root (glib, sndfile, …)")
if(AIRDOUR_DEPS_ROOT)
  list(APPEND CMAKE_PREFIX_PATH "${AIRDOUR_DEPS_ROOT}")
endif()

set(AIRDOUR_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# --- Slice order (grow targets as they link on a Windows machine) ---
# 1. pbd  2. temporal / midipp / evoral  3. ardour  4. dummy + wasapi  5. headless

add_custom_target(airdour_slice_notes
  COMMAND ${CMAKE_COMMAND} -E echo "Build order: pbd -> temporal/midi/evoral -> ardour -> dummy+wasapi -> hardour"
  COMMAND ${CMAKE_COMMAND} -E echo "Audio: WASAPI shared only (see libs/backends/wasapi/README.md)"
)

# Placeholder INTERFACE libs so IDEs can open the graph before sources are wired.
add_library(airdour_wasapi_backend INTERFACE)
target_include_directories(airdour_wasapi_backend INTERFACE
  "${AIRDOUR_SRC_ROOT}/libs/backends/wasapi"
  "${AIRDOUR_SRC_ROOT}/libs/ardour"
  "${AIRDOUR_SRC_ROOT}/libs/pbd"
)
target_compile_definitions(airdour_wasapi_backend INTERFACE BACKEND_NAME=\"WASAPI\")

message(STATUS "Airdour win64 spine: WASAPI shared-mode backend at libs/backends/wasapi")
message(STATUS "Next: wire real add_library() targets on a Windows agent with deps present")
