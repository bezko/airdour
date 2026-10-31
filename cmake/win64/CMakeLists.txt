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

set(AIRDOUR_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

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
    "${AIRDOUR_SRC_ROOT}/win_ardour/shell_window.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/engine_host.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/wave_strip.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/cairo_wave_blit.cc"
    "${AIRDOUR_SRC_ROOT}/win_ardour/transport_bar.cc"
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

  # Optional: vcpkg Cairo (gtk-libs) for WaveStrip offline ImageSurface → D2D blit.
  # Works for UI-only and engine-linked shells. No GTK chrome; FillRectangle fallback.
  set(_cairo_prefix "${AIRDOUR_SRC_ROOT}/tools/win-native/vcpkg_installed/x64-windows")
  if(EXISTS "${_cairo_prefix}/include/cairo/cairo.h" AND EXISTS "${_cairo_prefix}/lib/cairo.lib")
    target_compile_definitions(airdour_win_shell PRIVATE AIRDOUR_HAVE_CAIRO=1)
    target_include_directories(airdour_win_shell PRIVATE "${_cairo_prefix}/include/cairo")
    target_link_directories(airdour_win_shell PRIVATE "${_cairo_prefix}/lib")
    target_link_libraries(airdour_win_shell PRIVATE cairo)
    foreach(_cairo_dll IN ITEMS cairo-2.dll pixman-1-0.dll)
      if(EXISTS "${_cairo_prefix}/bin/${_cairo_dll}")
        add_custom_command(TARGET airdour_win_shell POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_cairo_prefix}/bin/${_cairo_dll}" "$<TARGET_FILE_DIR:airdour_win_shell>"
          COMMENT "Copy ${_cairo_dll} next to airdour_win_shell"
        )
      endif()
    endforeach()
    message(STATUS "Airdour win64: WaveStrip Cairo→D2D blit enabled (vcpkg cairo)")
  else()
    message(STATUS "Airdour win64: WaveStrip D2D-only peaks (Install-HeadlessDeps -WithGtkLibs for Cairo)")
  endif()

  # Console subsystem so --timeout smoke logs are capturable; still creates a HWND.
  set_target_properties(airdour_win_shell PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts"
    OUTPUT_NAME "airdour_win_shell"
  )
endif()
