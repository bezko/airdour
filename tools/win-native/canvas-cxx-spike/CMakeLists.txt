# Isolated MSVC spike: cairomm link + attempted libs/canvas header compile.
# Product UI stays Direct2D. No GTK windows.
#
# After Install-CanvasCxxSpike.ps1:
#   cmake -S tools/win-native/canvas-cxx-spike -B build-canvas-cxx-spike -G "Visual Studio 17 2022" -A x64
#   cmake --build build-canvas-cxx-spike --config Release --target cairomm_probe

cmake_minimum_required(VERSION 3.20)
project(airdour_canvas_cxx_spike LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(_SPIKE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(_VCPKG_PREFIX "${_SPIKE_ROOT}/vcpkg_installed/x64-windows"
    CACHE PATH "canvas-cxx-spike vcpkg_installed prefix")
set(_AIRDOUR_SRC "${_SPIKE_ROOT}/../../.." CACHE PATH "airdour repo root")

# --- cairomm header discovery ---
set(_CAIROMM_INC "")
foreach(_cand IN ITEMS
    "${_VCPKG_PREFIX}/include/cairomm-1.0"
    "${_VCPKG_PREFIX}/include")
  if(EXISTS "${_cand}/cairomm/cairomm.h" OR EXISTS "${_cand}/cairomm/context.h")
    set(_CAIROMM_INC "${_cand}")
    break()
  endif()
endforeach()

if(NOT _CAIROMM_INC)
  message(FATAL_ERROR
    "cairomm headers missing under ${_VCPKG_PREFIX}. "
    "Run: powershell -File tools/win-native/canvas-cxx-spike/Install-CanvasCxxSpike.ps1")
endif()

set(_inc "${_VCPKG_PREFIX}/include")
set(_lib "${_VCPKG_PREFIX}/lib")

# --- cairomm_probe (expected PASS) ---
add_executable(cairomm_probe cairomm_probe.cc)
target_include_directories(cairomm_probe PRIVATE
  "${_CAIROMM_INC}"
  "${_inc}"
  "${_inc}/cairo"
  "${_inc}/sigc++-3.0"
  "${_lib}/sigc++-3.0/include"
)
foreach(_name IN ITEMS cairomm-1.16.lib cairomm-1.0.lib cairo.lib sigc-3.0.lib pixman-1.lib)
  if(EXISTS "${_lib}/${_name}")
    target_link_libraries(cairomm_probe PRIVATE "${_lib}/${_name}")
  endif()
endforeach()
set_target_properties(cairomm_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/artifacts/Release"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/artifacts/Debug"
)

# --- canvas_types_probe (expected FAIL compile — documents blockers) ---
option(AIRDOUR_TRY_CANVAS_TYPES_PROBE
  "Attempt to compile canvas/types.h (expected FAIL without gtkmm2ext/ytkmm)" ON)

if(AIRDOUR_TRY_CANVAS_TYPES_PROBE)
  add_executable(canvas_types_probe canvas_types_probe.cc)
  target_include_directories(canvas_types_probe PRIVATE
    "${_AIRDOUR_SRC}/libs"
    "${_AIRDOUR_SRC}/libs/canvas"
    "${_AIRDOUR_SRC}/libs/gtkmm2ext"
    "${_AIRDOUR_SRC}/libs/pbd"
    "${_CAIROMM_INC}"
    "${_inc}"
    "${_inc}/cairo"
    "${_inc}/sigc++-3.0"
    "${_lib}/sigc++-3.0/include"
  )
  # Intentionally do NOT add ytk/ydk/gtkmm2 — they are not on this MSVC path.
  # Compile failure is the documented result.
  set_target_properties(canvas_types_probe PROPERTIES
    EXCLUDE_FROM_ALL TRUE
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts"
  )
  message(STATUS "canvas_types_probe: configured (EXCLUDE_FROM_ALL; expect compile FAIL)")
endif()

message(STATUS "cairomm_probe: includes ${_CAIROMM_INC}")
