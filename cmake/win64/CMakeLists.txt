# Airdour win64 / MSVC first-slice spine
#
# Stages:
#   1) wasapi_io_smoke     — WasapiIO + Windows SDK only (CI green path)
#   2) deps_headless_smoke — vcpkg C stack (glib/xml/sndfile/curl/archive/pthread)
#   3) deps_glibmm_smoke   — overlay glibmm-2.4 + sigc++-2.0 link probe
#   4) INTERFACE notes for full wasapi_backend / libpbd
#
# Audio product path: WASAPI shared (libs/backends/wasapi).
# Exclusive / DirectSound / ASIO: out of scope.
# GTK2: out of scope for this spine.

cmake_minimum_required(VERSION 3.20)
project(airdour_win64 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(AIRDOUR_BUILD_WASAPI_IO_SMOKE "Build WasapiIO smoke static lib (Windows SDK only)" ON)
option(AIRDOUR_BUILD_DEPS_HEADLESS_SMOKE "Build headless vcpkg C-deps probe executable" OFF)
option(AIRDOUR_BUILD_DEPS_GLIBMM_SMOKE "Build overlay glibmm-2.4 + sigc++-2.0 probe executable" OFF)
option(AIRDOUR_BUILD_WIN_SHELL "Build minimal Win32+Direct2D Airdour shell (no GTK)" ON)

if(NOT WIN32)
  message(WARNING "cmake/win64 is intended for MSVC/Win64; smoke lib disabled off-Windows.")
  set(AIRDOUR_BUILD_WASAPI_IO_SMOKE OFF)
  set(AIRDOUR_BUILD_DEPS_HEADLESS_SMOKE OFF)
  set(AIRDOUR_BUILD_DEPS_GLIBMM_SMOKE OFF)
  set(AIRDOUR_BUILD_WIN_SHELL OFF)
endif()

add_compile_definitions(
  PLATFORM_WINDOWS=1
  AIRDOUR_WINDOWS_ONLY=1
  _USE_MATH_DEFINES=1
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

# Multi-config (VS) + vcpkg: map Release consumers onto RelWithDebInfo/Release imported locs.
set(CMAKE_MAP_IMPORTED_CONFIG_RELEASE RelWithDebInfo Release MinSizeRel "")
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO RelWithDebInfo Release MinSizeRel "")
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL MinSizeRel Release RelWithDebInfo "")

set(AIRDOUR_DEPS_ROOT "" CACHE PATH "MSVC dependency root (glib, sndfile, …)")
if(AIRDOUR_DEPS_ROOT)
  list(APPEND CMAKE_PREFIX_PATH "${AIRDOUR_DEPS_ROOT}")
endif()

set(AIRDOUR_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# --- Slice order (full product) ---
# 1. pbd  2. temporal / midipp / evoral  3. ardour  4. dummy + wasapi  5. headless
# glibmm-2.4 + sigc++-2.0: overlay ports in tools/win-native/overlays.

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
  COMMAND ${CMAKE_COMMAND} -E echo "Next: deps_glibmm_smoke then waf libpbd (vamp-sdk + rubberband)"
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

# --- Headless C deps probe (vcpkg) ---
# Prefer CMake CONFIG/BUILTIN packages so multi-config Release does not pull debug/*.lib.
if(AIRDOUR_BUILD_DEPS_HEADLESS_SMOKE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)

  find_package(LibXml2 REQUIRED)
  find_package(CURL REQUIRED)
  find_package(SndFile CONFIG REQUIRED)
  find_package(PThreads4W CONFIG REQUIRED)

  # VS multi-config + pkg-config often resolves to debug/*.lib; prefer explicit release paths.
  if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(_vcpkg_lib "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
    set(_vcpkg_inc "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
  else()
    set(_vcpkg_lib "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/lib")
    set(_vcpkg_inc "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/include")
  endif()

  add_executable(deps_headless_smoke
    "${CMAKE_CURRENT_SOURCE_DIR}/deps_headless_smoke.cc"
  )

  target_include_directories(deps_headless_smoke PRIVATE
    "${_vcpkg_inc}"
    "${_vcpkg_inc}/glib-2.0"
    "${_vcpkg_lib}/glib-2.0/include"
  )

  target_link_libraries(deps_headless_smoke PRIVATE
    LibXml2::LibXml2
    CURL::libcurl
    SndFile::sndfile
    PThreads4W::PThreads4W
  )

  foreach(_rel_lib IN ITEMS glib-2.0.lib intl.lib archive.lib)
    if(EXISTS "${_vcpkg_lib}/${_rel_lib}")
      target_link_libraries(deps_headless_smoke PRIVATE "${_vcpkg_lib}/${_rel_lib}")
    endif()
  endforeach()

  set_target_properties(deps_headless_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts"
  )
  message(STATUS "Airdour win64: deps_headless_smoke enabled (vcpkg C stack)")
endif()

# --- Overlay glibmm-2.4 + sigc++-2.0 probe ---
if(AIRDOUR_BUILD_DEPS_GLIBMM_SMOKE)
  if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(_gmm_lib "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
    set(_gmm_inc "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
  else()
    set(_gmm_lib "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/lib")
    set(_gmm_inc "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/include")
  endif()

  # Manifest install may live under tools/win-native when installed via Install-HeadlessDeps.ps1.
  if(NOT EXISTS "${_gmm_lib}/glibmm-2.4.lib")
    set(_manifest_gmm "${AIRDOUR_SRC_ROOT}/tools/win-native/vcpkg_installed/x64-windows")
    if(EXISTS "${_manifest_gmm}/lib/glibmm-2.4.lib")
      set(_gmm_lib "${_manifest_gmm}/lib")
      set(_gmm_inc "${_manifest_gmm}/include")
    endif()
  endif()

  if(NOT EXISTS "${_gmm_lib}/glibmm-2.4.lib")
    message(FATAL_ERROR "glibmm-2.4.lib not found under ${_gmm_lib}; run tools/win-native/Install-HeadlessDeps.ps1")
  endif()

  add_executable(deps_glibmm_smoke
    "${CMAKE_CURRENT_SOURCE_DIR}/deps_glibmm_smoke.cc"
  )

  target_include_directories(deps_glibmm_smoke PRIVATE
    "${_gmm_inc}"
    "${_gmm_inc}/glibmm-2.4"
    "${_gmm_lib}/glibmm-2.4/include"
    "${_gmm_inc}/sigc++-2.0"
    "${_gmm_lib}/sigc++-2.0/include"
    "${_gmm_inc}/glib-2.0"
    "${_gmm_lib}/glib-2.0/include"
  )

  foreach(_rel_lib IN ITEMS glibmm-2.4.lib sigc-2.0.lib gobject-2.0.lib gio-2.0.lib glib-2.0.lib intl.lib)
    if(EXISTS "${_gmm_lib}/${_rel_lib}")
      target_link_libraries(deps_glibmm_smoke PRIVATE "${_gmm_lib}/${_rel_lib}")
    endif()
  endforeach()

  set_target_properties(deps_glibmm_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts"
  )
  message(STATUS "Airdour win64: deps_glibmm_smoke enabled (overlay glibmm-2.4)")
endif()

# --- Minimal Win32 + Direct2D + DirectWrite GUI shell (no GTK2) ---
option(AIRDOUR_WIN_SHELL_WITH_ENGINE "Link airdour_win_shell against waf-built libardour (laptop)" OFF)
set(AIRDOUR_WAF_BUILD_DIR "${AIRDOUR_SRC_ROOT}/build" CACHE PATH "waf build dir with ardour.lib / backends")

if(AIRDOUR_BUILD_WIN_SHELL)
  add_executable(airdour_win_shell
    "${AIRDOUR_SRC_ROOT}/win_ardour/main.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/shell_window.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/engine_host.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/wave_strip.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/transport_bar.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/mixer_strip.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/monitor_strip.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/history_panel.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/prefs_panel.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/about_panel.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/recent_panel.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/level_meter.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/automation_lane.cc"
  )
  target_include_directories(airdour_win_shell PRIVATE
    "${AIRDOUR_SRC_ROOT}/win_ardour"
  )
  target_compile_definitions(airdour_win_shell PRIVATE
    UNICODE
    _UNICODE
    PLATFORM_WINDOWS=1
    AIRDOUR_WINDOWS_ONLY=1
    _CRT_SECURE_NO_WARNINGS
  )
  target_link_libraries(airdour_win_shell PRIVATE
    d2d1
    dwrite
    user32
    gdi32
    ole32
    shell32
    shcore
  )

  set(_waf_ardour_lib "${AIRDOUR_WAF_BUILD_DIR}/libs/ardour/ardour.lib")
  if(AIRDOUR_WIN_SHELL_WITH_ENGINE AND EXISTS "${_waf_ardour_lib}")
    # Match waf CRT: Debug build/ → /MDd; Release build-rel → /MD.
    set_property(TARGET airdour_win_shell PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

    set(_vcpkg_prefix "${AIRDOUR_SRC_ROOT}/tools/win-native/vcpkg_installed/x64-windows")
    target_compile_definitions(airdour_win_shell PRIVATE
      AIRDOUR_WIN_SHELL_WITH_ENGINE=1
      COMPILER_MSVC=1
      PLATFORM_WINDOWS=1
      AIRDOUR_WINDOWS_ONLY=1
      VST3_SUPPORT=1
      WAF_BUILD=1
      LOCALEDIR=\"\"
      PROGRAM_NAME=\"Airdour\"
      PROGRAM_VERSION=\"9\"
      LIBARDOUR=\"ardour\"
      ARCH_X86=1
      BOOST_SYSTEM_NO_DEPRECATED
      BOOST_ERROR_CODE_HEADER_ONLY
    )
    target_include_directories(airdour_win_shell PRIVATE
      "${AIRDOUR_SRC_ROOT}"
      "${AIRDOUR_SRC_ROOT}/libs"
      "${AIRDOUR_SRC_ROOT}/libs/ardour"
      "${AIRDOUR_SRC_ROOT}/libs/pbd"
      "${AIRDOUR_SRC_ROOT}/libs/temporal"
      "${AIRDOUR_SRC_ROOT}/libs/midi++2"
      "${AIRDOUR_SRC_ROOT}/libs/evoral"
      "${AIRDOUR_SRC_ROOT}/libs/ctrl-interface/control_protocol"
      "${AIRDOUR_SRC_ROOT}/libs/zita-resampler"
      "${AIRDOUR_SRC_ROOT}/libs/libltc/ltc"
      "${AIRDOUR_SRC_ROOT}/libs/lua"
      "${AIRDOUR_SRC_ROOT}/libs/audiographer"
      "${AIRDOUR_SRC_ROOT}/libs/fluidsynth/include"
      "${AIRDOUR_SRC_ROOT}/libs/ptformat"
      "${AIRDOUR_SRC_ROOT}/libs/vst3"
      "${AIRDOUR_SRC_ROOT}/libs/panners"
      "${AIRDOUR_WAF_BUILD_DIR}/libs/pbd"
      "${AIRDOUR_WAF_BUILD_DIR}/libs/ardour"
      "${_vcpkg_prefix}/include"
      "${_vcpkg_prefix}/include/glib-2.0"
      "${_vcpkg_prefix}/lib/glib-2.0/include"
      "${_vcpkg_prefix}/include/glibmm-2.4"
      "${_vcpkg_prefix}/lib/glibmm-2.4/include"
      "${_vcpkg_prefix}/include/sigc++-2.0"
      "${_vcpkg_prefix}/lib/sigc++-2.0/include"
      "${_vcpkg_prefix}/include/libxml2"
    )
    target_link_directories(airdour_win_shell PRIVATE
      "${AIRDOUR_WAF_BUILD_DIR}/libs/ardour"
      "${AIRDOUR_WAF_BUILD_DIR}/libs/pbd"
      "${AIRDOUR_WAF_BUILD_DIR}/libs/temporal"
      "${AIRDOUR_WAF_BUILD_DIR}/libs/midi++2"
      "${AIRDOUR_WAF_BUILD_DIR}/libs/evoral"
      "${AIRDOUR_WAF_BUILD_DIR}/libs/ctrl-interface/control_protocol"
      "$<$<CONFIG:Debug>:${_vcpkg_prefix}/debug/lib>"
      "${_vcpkg_prefix}/lib"
    )
    target_link_libraries(airdour_win_shell PRIVATE
      ardour
      pbd
      temporal
      midipp
      evoral
      ardourcp
      # Headers instantiate PBD/XML templates that reference glib; Release needs
      # an explicit import (Debug often pulled it via debug/lib order).
      glib-2.0
      glibmm-2.4
      sigc-2.0
      libxml2
      "$<$<CONFIG:Debug>:pthreadVC3d>"
      "$<$<NOT:$<CONFIG:Debug>>:pthreadVC3>"
    )
    # Side-by-side waf DLLs so Explorer / incomplete PATH still finds
    # ptformat.dll (and friends) — avoids "ptformat.dll was not found".
    set(_waf_runtime_dlls
      "${AIRDOUR_WAF_BUILD_DIR}/libs/ardour/ardour.dll"
      "${AIRDOUR_WAF_BUILD_DIR}/libs/pbd/pbd.dll"
      "${AIRDOUR_WAF_BUILD_DIR}/libs/temporal/temporal.dll"
      "${AIRDOUR_WAF_BUILD_DIR}/libs/midi++2/midipp.dll"
      "${AIRDOUR_WAF_BUILD_DIR}/libs/evoral/evoral.dll"
      "${AIRDOUR_WAF_BUILD_DIR}/libs/audiographer/audiographer.dll"
      "${AIRDOUR_WAF_BUILD_DIR}/libs/ptformat/ptformat.dll"
      "${AIRDOUR_WAF_BUILD_DIR}/libs/ctrl-interface/control_protocol/ardourcp.dll"
    )
    foreach(_dll IN LISTS _waf_runtime_dlls)
      if(EXISTS "${_dll}")
        add_custom_command(TARGET airdour_win_shell POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_dll}" "$<TARGET_FILE_DIR:airdour_win_shell>"
          COMMENT "Copy ${_dll} next to airdour_win_shell"
        )
      endif()
    endforeach()
    message(STATUS "Airdour win64: airdour_win_shell WITH engine (waf libs @ ${AIRDOUR_WAF_BUILD_DIR})")
  elseif(AIRDOUR_WIN_SHELL_WITH_ENGINE)
    message(WARNING "AIRDOUR_WIN_SHELL_WITH_ENGINE set but ${_waf_ardour_lib} missing — building UI-only")
  else()
    message(STATUS "Airdour win64: airdour_win_shell UI-only (enable AIRDOUR_WIN_SHELL_WITH_ENGINE for attach)")
  endif()

  # Console subsystem so --timeout smoke logs are capturable; still creates a HWND.
  set_target_properties(airdour_win_shell PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts"
    OUTPUT_NAME "airdour_win_shell"
  )
endif()
