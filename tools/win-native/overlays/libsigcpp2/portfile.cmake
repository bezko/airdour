vcpkg_download_distfile(ARCHIVE
    URLS
        "https://download.gnome.org/sources/libsigc++/2.10/libsigc++-${VERSION}.tar.xz"
        "https://ftp.gnome.org/pub/GNOME/sources/libsigc++/2.10/libsigc++-${VERSION}.tar.xz"
    FILENAME "libsigc++-${VERSION}.tar.xz"
    SHA512 8b22fd8ae4eca3ebc1b65b68d4dc022e7bbde6d1d02a557e64d2fda2682e2e39b357af6d8b68e8741c287701be9fffd27125c6171790455a7657e0ea55cc08b3
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
)

vcpkg_configure_meson(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -Dbuild-examples=false
        -Dbuild-documentation=false
        -Dvalidation=false
        -Dbenchmark=false
        -Dmaintainer-mode=false
)

vcpkg_install_meson()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

# Match dynamic CRT DLL export macros when building shared (x64-windows).
if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    if(EXISTS "${CURRENT_PACKAGES_DIR}/include/sigc++config.h")
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/sigc++config.h" "#ifdef SIGC_DLL" "#if 1")
    endif()
else()
    if(EXISTS "${CURRENT_PACKAGES_DIR}/include/sigc++config.h")
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/sigc++config.h" "#ifdef SIGC_DLL" "#if 0")
    endif()
endif()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
