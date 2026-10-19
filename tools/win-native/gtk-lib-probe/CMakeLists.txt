# Standalone MSVC probe: link GTK3 + Cairo from win-native vcpkg_installed.
# No windows / themes. Product UI stays Direct2D.
#
# Configure example (from repo root, after Install-HeadlessDeps -WithGtkLibs):
#   cmake -S tools/win-native/gtk-lib-probe -B build-gtk-lib-probe -G "Visual Studio 17 2022" -A x64
#   cmake --build build-gtk-lib-probe --config Release
#   build-gtk-lib-probe\Release\gtk_lib_probe.exe

cmake_minimum_required(VERSION 3.20)
project(airdour_gtk_lib_probe LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(_PROBE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(_VCPKG_PREFIX "${_PROBE_ROOT}/vcpkg_installed/x64-windows"
    CACHE PATH "win-native vcpkg_installed/x64-windows prefix")

if(NOT EXISTS "${_VCPKG_PREFIX}/include/gtk-3.0/gtk/gtk.h")
  message(FATAL_ERROR
    "GTK3 headers missing under ${_VCPKG_PREFIX}. "
    "Run: powershell -File tools/win-native/Install-HeadlessDeps.ps1 -WithGtkLibs")
endif()

set(_inc "${_VCPKG_PREFIX}/include")
set(_lib "${_VCPKG_PREFIX}/lib")

add_executable(gtk_lib_probe main.cc)

target_include_directories(gtk_lib_probe PRIVATE
  "${_inc}"
  "${_inc}/gtk-3.0"
  "${_inc}/glib-2.0"
  "${_lib}/glib-2.0/include"
  "${_inc}/cairo"
  "${_inc}/pango-1.0"
  "${_inc}/harfbuzz"
  "${_inc}/gdk-pixbuf-2.0"
  "${_inc}/atk-1.0"
  "${_inc}/fribidi"
)

# Prefer Release import libs (multi-config VS otherwise may pick debug).
foreach(_name IN ITEMS
    gtk-3.lib gdk-3.lib gobject-2.0.lib gio-2.0.lib glib-2.0.lib gmodule-2.0.lib
    cairo.lib pangocairo-1.0.lib pango-1.0.lib pangowin32-1.0.lib
    gdk_pixbuf-2.0.lib atk-1.0.lib harfbuzz.lib intl.lib fribidi.lib)
  if(EXISTS "${_lib}/${_name}")
    target_link_libraries(gtk_lib_probe PRIVATE "${_lib}/${_name}")
  endif()
endforeach()

set_target_properties(gtk_lib_probe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/artifacts/Debug"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/artifacts/Release"
)

message(STATUS "gtk_lib_probe: vcpkg prefix ${_VCPKG_PREFIX}")
