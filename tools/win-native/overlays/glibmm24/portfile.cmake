vcpkg_download_distfile(ARCHIVE
    URLS
        "https://download.gnome.org/sources/glibmm/2.66/glibmm-${VERSION}.tar.xz"
        "https://ftp.gnome.org/pub/GNOME/sources/glibmm/2.66/glibmm-${VERSION}.tar.xz"
    FILENAME "glibmm-${VERSION}.tar.xz"
    SHA512 4ebf203324d3ee95c47012915efb39d4dc59eb7a6f337e7b8c7c0b3589574b07967974363931b0d4159618f88178b04715b2c359c3dc3f67a7781bfac0d9f277
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
)

# Discover FINAL GObject types from installed glib headers.
set(_glib_inc "${CURRENT_INSTALLED_DIR}/include/glib-2.0")
file(GLOB_RECURSE _glib_hdrs "${_glib_inc}/*.h")
set(_final_types "")
foreach(_gh IN LISTS _glib_hdrs)
    file(READ "${_gh}" _gh_content)
    string(REGEX MATCHALL "G_DECLARE_FINAL_TYPE[ \t]*\\([ \t]*([A-Za-z0-9_]+)" _matches "${_gh_content}")
    foreach(_m IN LISTS _matches)
        if(_m MATCHES "G_DECLARE_FINAL_TYPE[ \t]*\\([ \t]*([A-Za-z0-9_]+)")
            set(_t "${CMAKE_MATCH_1}")
            if(NOT _t STREQUAL "ModuleObjName" AND NOT _t STREQUAL "MyAppWindow")
                list(APPEND _final_types "${_t}")
            endif()
        endif()
    endforeach()
endforeach()
list(REMOVE_DUPLICATES _final_types)
message(STATUS "glibmm24: FINAL Class aliases to strip: ${_final_types}")

# Strip `using GFooClass = struct _GFooClass` for FINAL types and ensure <gio/gio.h>
# (individual gio/*.h refuse direct include without __GIO_GIO_H_INSIDE__).
file(GLOB_RECURSE _mm_hs
    "${SOURCE_PATH}/untracked/gio/giomm/*.h"
    "${SOURCE_PATH}/untracked/glib/glibmm/*.h"
)
foreach(_h IN LISTS _mm_hs)
    file(READ "${_h}" _content)
    set(_orig "${_content}")
    set(_changed FALSE)
    foreach(_t IN LISTS _final_types)
        set(_line "using ${_t}Class = struct _${_t}Class;")
        if(_content MATCHES "using ${_t}Class = struct _${_t}Class;")
            string(REPLACE "${_line}\n" "" _content "${_content}")
            string(REPLACE "${_line}\r\n" "" _content "${_content}")
            set(_changed TRUE)
        endif()
    endforeach()
    if(_changed)
        if(NOT _content MATCHES "#include <gio/gio.h>")
            if(_content MATCHES "#include <glibmm/object.h>\n")
                string(REPLACE
                    "#include <glibmm/object.h>\n"
                    "#include <glibmm/object.h>\n#include <gio/gio.h>\n"
                    _content "${_content}")
            elseif(_content MATCHES "#include <glibmm/object.h>\r\n")
                string(REPLACE
                    "#include <glibmm/object.h>\r\n"
                    "#include <glibmm/object.h>\r\n#include <gio/gio.h>\r\n"
                    _content "${_content}")
            else()
                string(REGEX REPLACE
                    "(#ifndef _G[A-Z0-9_]+\\r?\\n#define _G[A-Z0-9_]+\\r?\\n)"
                    "\\1#include <gio/gio.h>\n"
                    _content "${_content}")
            endif()
        endif()
        file(WRITE "${_h}" "${_content}")
    endif()
endforeach()

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -Dbuild-examples=false
        -Dbuild-documentation=false
        -Dmaintainer-mode=false
        -Dmsvc14x-parallel-installable=false
        "-Dcpp_args=['/permissive']"
)

vcpkg_install_meson()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/lib/glibmm-2.4/proc"
    "${CURRENT_PACKAGES_DIR}/lib/glibmm-2.4/proc"
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_fixup_pkgconfig()
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
