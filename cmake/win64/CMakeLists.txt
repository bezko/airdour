# Airdour win64 / MSVC first-slice spine
#
# Stages:
#   1) wasapi_io_smoke     — WasapiIO + Windows SDK only (CI green path)
#
# Audio product path: WASAPI shared (libs/backends/wasapi).
# Exclusive / DirectSound / ASIO: out of scope.
# GTK chrome: out of scope — D2D shell is product UI.

cmake_minimum_required(VERSION 3.20)
project(airdour_win64 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(AIRDOUR_BUILD_WASAPI_IO_SMOKE "Build WasapiIO smoke static lib (Windows SDK only)" ON)
option(AIRDOUR_WARNINGS_AS_ERRORS "Treat Airdour compiler warnings as errors" OFF)
option(AIRDOUR_BUILD_WIN_SHELL "Build minimal Win32+Direct2D Airdour shell (no GTK)" ON)

if(NOT WIN32)
  message(WARNING "cmake/win64 is intended for MSVC/Win64; smoke lib disabled off-Windows.")
  set(AIRDOUR_BUILD_WASAPI_IO_SMOKE OFF)
  set(AIRDOUR_BUILD_WIN_SHELL OFF)
endif()

add_compile_definitions(
  PLATFORM_WINDOWS=1
  AIRDOUR_WINDOWS_ONLY=1
  _USE_MATH_DEFINES=1
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

# /utf-8 is load-bearing: win_ardour sources are BOM-less UTF-8 and contain
# wide literals (U+2212, U+266F, …). Without it MSVC decodes them as cp1252
# and the shipped UI shows mojibake. /Zc:__cplusplus makes feature tests honest.
add_library(airdour_msvc_options INTERFACE)
if(MSVC)
  target_compile_options(airdour_msvc_options INTERFACE
    /utf-8
    /Zc:__cplusplus
    /permissive-
    /W4
    # Noise from third-party (waf/vcpkg) headers at /W4.
    /wd4100 /wd4244 /wd4245 /wd4267 /wd4324 /wd4389
    $<$<BOOL:${AIRDOUR_WARNINGS_AS_ERRORS}>:/WX>
  )
endif()
# Applies to every target defined below (probes + shell) in this directory.
link_libraries(airdour_msvc_options)

# Multi-config (VS) + vcpkg: map Release consumers onto RelWithDebInfo/Release imported locs.
set(CMAKE_MAP_IMPORTED_CONFIG_RELEASE RelWithDebInfo Release MinSizeRel "")
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO RelWithDebInfo Release MinSizeRel "")
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL MinSizeRel Release RelWithDebInfo "")

set(AIRDOUR_DEPS_ROOT "" CACHE PATH "MSVC dependency root (glib, sndfile, …)")
if(AIRDOUR_DEPS_ROOT)
  list(APPEND CMAKE_PREFIX_PATH "${AIRDOUR_DEPS_ROOT}")
endif()

set(AIRDOUR_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# Single source of truth for the vcpkg x64-windows prefix. Honors the vcpkg
# environment variable, else the in-repo install used by tools/win-native.
set(AIRDOUR_VCPKG_PREFIX "" CACHE PATH "vcpkg x64-windows prefix")
if(NOT AIRDOUR_VCPKG_PREFIX)
  if(DEFINED ENV{VCPKG_INSTALLED_DIR} AND EXISTS "$ENV{VCPKG_INSTALLED_DIR}/x64-windows")
    set(AIRDOUR_VCPKG_PREFIX "$ENV{VCPKG_INSTALLED_DIR}/x64-windows")
  else()
    set(AIRDOUR_VCPKG_PREFIX "${AIRDOUR_SRC_ROOT}/tools/win-native/vcpkg_installed/x64-windows")
  endif()
endif()
message(STATUS "Airdour win64: vcpkg prefix = ${AIRDOUR_VCPKG_PREFIX}")

# Imported target for a vcpkg lib so consumers never depend on link_directories.
function(airdour_import_vcpkg_lib _target _libname)
  add_library(${_target} UNKNOWN IMPORTED)
  set_target_properties(${_target} PROPERTIES
    IMPORTED_CONFIGURATIONS "RELEASE;DEBUG"
    IMPORTED_LOCATION_RELEASE "${AIRDOUR_VCPKG_PREFIX}/lib/${_libname}.lib"
    IMPORTED_LOCATION_DEBUG "${AIRDOUR_VCPKG_PREFIX}/debug/lib/${_libname}.lib"
  )
endfunction()

# --- Slice order (full product) ---
# 1. pbd  2. temporal / midipp / evoral  3. ardour  4. dummy + wasapi  5. headless
# glibmm-2.4 + sigc++-2.0: overlay ports in tools/win-native/overlays.

set(WASAPI_IO_SOURCES
  "${AIRDOUR_SRC_ROOT}/libs/backends/wasapi/wasapi_io.cc"
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

# --- Minimal Win32 + Direct2D + DirectWrite GUI shell (no GTK2) ---
option(AIRDOUR_WIN_SHELL_WITH_ENGINE "Link airdour_win_shell against waf-built libardour (laptop)" OFF)
set(AIRDOUR_WAF_BUILD_DIR "${AIRDOUR_SRC_ROOT}/build" CACHE PATH "waf build dir with ardour.lib / backends")

if(AIRDOUR_BUILD_WIN_SHELL)
  add_executable(airdour_win_shell
    "${AIRDOUR_SRC_ROOT}/win_ardour/main.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/ui_draw.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/shell_window.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/engine_host.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/engine_midi.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/engine_fx_ace.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/engine_vst3.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/engine_vst2.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/vst2_bridge_client.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/wave_strip.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/cairo_wave_blit.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/transport_bar.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/menu_bar.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/mixer_strip.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/monitor_strip.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/history_panel.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/prefs_panel.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/about_panel.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/recent_panel.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/recovery_panel.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/level_meter.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/automation_lane.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/spectrum_panel.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/correlation_panel.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/loudness_panel.cc"
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
  if(AIRDOUR_WIN_SHELL_WITH_ENGINE)
    if(NOT EXISTS "${_waf_ardour_lib}")
      message(FATAL_ERROR
        "AIRDOUR_WIN_SHELL_WITH_ENGINE=ON but ${_waf_ardour_lib} is missing.\n"
        "Build the engine with tools/win-native/build-waf-release.cmd first, or point "
        "-DAIRDOUR_WAF_BUILD_DIR=<waf out dir> at an existing build.")
    endif()

    # Match waf CRT: Debug build/ → /MDd; Release build-rel → /MD.
    set_property(TARGET airdour_win_shell PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

    set(_vcpkg_prefix "${AIRDOUR_VCPKG_PREFIX}")
    target_compile_definitions(airdour_win_shell PRIVATE
      AIRDOUR_WIN_SHELL_WITH_ENGINE=1
      COMPILER_MSVC=1
      PLATFORM_WINDOWS=1
      AIRDOUR_WINDOWS_ONLY=1
      VST3_SUPPORT=1
      WINDOWS_VST_SUPPORT=1
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

    # waf artifacts as imported targets: a missing library now fails at link
    # with a clear message instead of silently degrading to a UI-only shell.
    foreach(_entry IN ITEMS
        "ardour|libs/ardour"
        "pbd|libs/pbd"
        "temporal|libs/temporal"
        "midipp|libs/midi++2"
        "evoral|libs/evoral"
        "ardourcp|libs/ctrl-interface/control_protocol")
      string(REPLACE "|" ";" _pair "${_entry}")
      list(GET _pair 0 _lib)
      list(GET _pair 1 _rel)
      set(_waf_lib_dir "${AIRDOUR_WAF_BUILD_DIR}/${_rel}")
      add_library(waf_${_lib} SHARED IMPORTED)
      set_target_properties(waf_${_lib} PROPERTIES
        IMPORTED_IMPLIB "${_waf_lib_dir}/${_lib}.lib"
        IMPORTED_LOCATION "${_waf_lib_dir}/${_lib}.dll"
      )
    endforeach()

    airdour_import_vcpkg_lib(vcpkg_glib glib-2.0)
    airdour_import_vcpkg_lib(vcpkg_glibmm glibmm-2.4)
    airdour_import_vcpkg_lib(vcpkg_sigc sigc-2.0)
    airdour_import_vcpkg_lib(vcpkg_libxml2 libxml2)
    airdour_import_vcpkg_lib(vcpkg_sndfile sndfile)

    add_library(vcpkg_pthread UNKNOWN IMPORTED)
    set_target_properties(vcpkg_pthread PROPERTIES
      IMPORTED_CONFIGURATIONS "RELEASE;DEBUG"
      IMPORTED_LOCATION_RELEASE "${_vcpkg_prefix}/lib/pthreadVC3.lib"
      IMPORTED_LOCATION_DEBUG "${_vcpkg_prefix}/debug/lib/pthreadVC3d.lib"
    )

    target_link_libraries(airdour_win_shell PRIVATE
      waf_ardour
      waf_pbd
      waf_temporal
      waf_midipp
      waf_evoral
      waf_ardourcp
      # Headers instantiate PBD/XML templates that reference glib; Release needs
      # an explicit import (Debug often pulled it via debug/lib order).
      vcpkg_glib
      vcpkg_glibmm
      vcpkg_sigc
      vcpkg_libxml2
      vcpkg_sndfile
      vcpkg_pthread
    )

    # Side-by-side waf DLLs so Explorer / incomplete PATH still finds
    # ptformat.dll (and friends) — avoids "ptformat.dll was not found".
    # Existence is checked at build time (StageRuntime.cmake), not configure.
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
    add_custom_command(TARGET airdour_win_shell POST_BUILD
      COMMAND ${CMAKE_COMMAND}
        "-DSTAGE_DEST=$<TARGET_FILE_DIR:airdour_win_shell>"
        "-DSTAGE_FILES=${_waf_runtime_dlls}"
        -P "${CMAKE_CURRENT_LIST_DIR}/StageRuntime.cmake"
      COMMENT "Stage waf runtime DLLs next to airdour_win_shell"
      VERBATIM
    )
    message(STATUS "Airdour win64: airdour_win_shell WITH engine (waf libs @ ${AIRDOUR_WAF_BUILD_DIR})")
  else()
    message(STATUS "Airdour win64: airdour_win_shell UI-only (enable AIRDOUR_WIN_SHELL_WITH_ENGINE for attach)")
  endif()

  # Optional: vcpkg Cairo (gtk-libs) for WaveStrip offline ImageSurface → D2D blit.
  # Works for UI-only and engine-linked shells. No GTK chrome; FillRectangle fallback.
  set(_cairo_prefix "${AIRDOUR_VCPKG_PREFIX}")
  if(EXISTS "${_cairo_prefix}/include/cairo/cairo.h" AND EXISTS "${_cairo_prefix}/lib/cairo.lib")
    target_compile_definitions(airdour_win_shell PRIVATE AIRDOUR_HAVE_CAIRO=1)
    target_include_directories(airdour_win_shell PRIVATE "${_cairo_prefix}/include/cairo")
    airdour_import_vcpkg_lib(vcpkg_cairo cairo)
    target_link_libraries(airdour_win_shell PRIVATE vcpkg_cairo)
    set(_cairo_dlls
      "${_cairo_prefix}/bin/cairo-2.dll"
      "${_cairo_prefix}/bin/pixman-1-0.dll"
    )
    add_custom_command(TARGET airdour_win_shell POST_BUILD
      COMMAND ${CMAKE_COMMAND}
        "-DSTAGE_DEST=$<TARGET_FILE_DIR:airdour_win_shell>"
        "-DSTAGE_FILES=${_cairo_dlls}"
        -P "${CMAKE_CURRENT_LIST_DIR}/StageRuntime.cmake"
      COMMENT "Stage Cairo runtime DLLs next to airdour_win_shell"
      VERBATIM
    )
    message(STATUS "Airdour win64: WaveStrip Cairo→D2D blit enabled (vcpkg cairo)")
  else()
    message(STATUS "Airdour win64: WaveStrip D2D-only peaks (optional Cairo shell blit: Install-HeadlessDeps -WithGtkLibs)")
  endif()

  # Console subsystem so --timeout smoke logs are capturable; still creates a HWND.
  set_target_properties(airdour_win_shell PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts"
    OUTPUT_NAME "airdour_win_shell"
  )
endif()

# --- Smoke tests (CTest) ---
include(CTest)

if(BUILD_TESTING AND AIRDOUR_BUILD_WIN_SHELL)
  set(_shell_test_cmake "${CMAKE_CURRENT_LIST_DIR}/RunShellTest.cmake")

  add_test(NAME shell_ui_smoke
    COMMAND ${CMAKE_COMMAND}
      "-DTEST_EXE=$<TARGET_FILE:airdour_win_shell>"
      "-DTEST_ARGS=--timeout;3;--no-engine"
      "-DVCPKG_BIN=${AIRDOUR_VCPKG_PREFIX}/bin"
      "-DWAF_ROOT=${AIRDOUR_WAF_BUILD_DIR}"
      -P "${_shell_test_cmake}"
  )
  set_tests_properties(shell_ui_smoke PROPERTIES TIMEOUT 60)

  if(AIRDOUR_WIN_SHELL_WITH_ENGINE)
    # Each flag drives one engine subsystem, then --timeout exits 0 on success.
    set(_engine_smokes
      tracks mute-solo pan pan-width send send-pan mixer-reset track-color
      edit snap tempo click history prefs zoom locate master-fx
      midi-multi fx-chain vst2 vst2-bridge vst3-instrument
    )
    foreach(_smoke IN LISTS _engine_smokes)
      add_test(NAME "shell_engine_${_smoke}"
        COMMAND ${CMAKE_COMMAND}
          "-DTEST_EXE=$<TARGET_FILE:airdour_win_shell>"
          "-DTEST_ARGS=--smoke-${_smoke};--timeout;3"
          "-DVCPKG_BIN=${AIRDOUR_VCPKG_PREFIX}/bin"
          "-DWAF_ROOT=${AIRDOUR_WAF_BUILD_DIR}"
          -P "${_shell_test_cmake}"
      )
      set_tests_properties("shell_engine_${_smoke}" PROPERTIES TIMEOUT 120)
    endforeach()
  endif()
endif()

# --- VST2-32 bridge (Win32 / x86 helper; build with -A Win32 or a second preset) ---
option(AIRDOUR_BUILD_VST2_BRIDGE_X86 "Build ardour-vst2-bridge-x86.exe (Win32)" OFF)
if(AIRDOUR_BUILD_VST2_BRIDGE_X86 AND WIN32)
  add_executable(ardour-vst2-bridge-x86
    "${AIRDOUR_SRC_ROOT}/win_ardour/vst2_bridge_x86_main.cc"
  )
  target_include_directories(ardour-vst2-bridge-x86 PRIVATE
    "${AIRDOUR_SRC_ROOT}/win_ardour"
  )
  target_compile_definitions(ardour-vst2-bridge-x86 PRIVATE
    UNICODE _UNICODE PLATFORM_WINDOWS=1
  )
  target_link_libraries(ardour-vst2-bridge-x86 PRIVATE user32)
  set_target_properties(ardour-vst2-bridge-x86 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts"
    OUTPUT_NAME "ardour-vst2-bridge-x86"
  )
  if(AIRDOUR_BUILD_WIN_SHELL)
    add_custom_command(TARGET airdour_win_shell POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:ardour-vst2-bridge-x86>"
        "$<TARGET_FILE_DIR:airdour_win_shell>/ardour-vst2-bridge-x86.exe"
      COMMENT "Stage VST2-32 bridge beside win_shell"
      VERBATIM
    )
  endif()
endif()

# --- Install / CPack (basic portable tree; richer packaging lives in
#     tools/win-native/ci/Package-*.ps1) ---
if(AIRDOUR_BUILD_WIN_SHELL)
  install(TARGETS airdour_win_shell RUNTIME DESTINATION .)
  if(TARGET ardour-vst2-bridge-x86)
    install(TARGETS ardour-vst2-bridge-x86 RUNTIME DESTINATION .)
  endif()
  if(AIRDOUR_WIN_SHELL_WITH_ENGINE)
    install(FILES ${_waf_runtime_dlls} DESTINATION . OPTIONAL)
    install(FILES "${AIRDOUR_SRC_ROOT}/win_ardour/run-engine-shell-release.bat"
            DESTINATION . OPTIONAL)
  endif()
  set(CPACK_GENERATOR "ZIP")
  set(CPACK_PACKAGE_NAME "airdour")
  set(CPACK_PACKAGE_VENDOR "Airdour")
  set(CPACK_PACKAGE_VERSION "9")
  set(CPACK_PACKAGE_FILE_NAME "airdour-win64")
  include(CPack)
endif()
