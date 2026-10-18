# Airdour win64 / MSVC first-slice spine
#
# Stages:
#   1) wasapi_io_smoke  — compiles/links WasapiIO with Windows SDK only (CI green path)
#   2) INTERFACE notes for full wasapi_backend (needs libardour — not linked in CI yet)
#
# Audio product path: WASAPI shared (libs/backends/wasapi).
# Exclusive / DirectSound / ASIO: out of scope.

cmake_minimum_required(VERSION 3.20)
project(airdour_win64 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(AIRDOUR_BUILD_WASAPI_IO_SMOKE "Build WasapiIO smoke static lib (Windows SDK only)" ON)

if(NOT WIN32)
  message(WARNING "cmake/win64 is intended for MSVC/Win64; smoke lib disabled off-Windows.")
  set(AIRDOUR_BUILD_WASAPI_IO_SMOKE OFF)
endif()

add_compile_definitions(
  PLATFORM_WINDOWS=1
  AIRDOUR_WINDOWS_ONLY=1
  _USE_MATH_DEFINES=1
)

set(AIRDOUR_DEPS_ROOT "" CACHE PATH "MSVC dependency root (glib, sndfile, …)")
if(AIRDOUR_DEPS_ROOT)
  list(APPEND CMAKE_PREFIX_PATH "${AIRDOUR_DEPS_ROOT}")
endif()

set(AIRDOUR_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# --- Slice order (full product) ---
# 1. pbd  2. temporal / midipp / evoral  3. ardour  4. dummy + wasapi  5. headless

set(WASAPI_IO_SOURCES
  "${AIRDOUR_SRC_ROOT}/libs/backends/wasapi/wasapi_io.cc"
)
set(WASAPI_BACKEND_SOURCES
  ${WASAPI_IO_SOURCES}
  "${AIRDOUR_SRC_ROOT}/libs/backends/wasapi/wasapi_backend.cc"
)

add_custom_target(airdour_slice_notes
  COMMAND ${CMAKE_COMMAND} -E echo "Build order: pbd -> temporal/midi/evoral -> ardour -> dummy+wasapi -> hardour"
  COMMAND ${CMAKE_COMMAND} -E echo "CI smoke: wasapi_io_smoke (Windows SDK only)"
)

if(AIRDOUR_BUILD_WASAPI_IO_SMOKE)
  add_library(wasapi_io_smoke STATIC ${WASAPI_IO_SOURCES})
  target_include_directories(wasapi_io_smoke PUBLIC
    "${AIRDOUR_SRC_ROOT}/libs/backends/wasapi"
  )
  target_compile_definitions(wasapi_io_smoke PRIVATE
    _WIN32
    UNICODE
    _UNICODE
  )
  target_link_libraries(wasapi_io_smoke PUBLIC ole32 winmm uuid avrt)
  set_target_properties(wasapi_io_smoke PROPERTIES
    OUTPUT_NAME "airdour_wasapi_io"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts"
  )
  message(STATUS "Airdour win64: wasapi_io_smoke (STATIC) enabled")
endif()

# Full backend still needs libardour — keep as INTERFACE inventory until deps land.
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
  AIRDOUR_WINDOWS_ONLY=1
)
if(WIN32)
  target_link_libraries(airdour_wasapi_backend INTERFACE ole32 winmm uuid avrt)
endif()

message(STATUS "Airdour win64: full wasapi_backend remains INTERFACE until libardour links")
