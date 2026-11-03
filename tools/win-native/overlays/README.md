# vcpkg overlays (headless C++ GLib stack)

## Why this folder exists

Ardour/`libpbd` needs **`glibmm-2.4`** + **`sigc++-2.0`** (pkg-config names).

Current stock vcpkg ports ship:

| Port | Stock version | Ardour needs |
|------|---------------|--------------|
| `glibmm` | 2.68+ / 2.90 (sigc++3) | **2.4 API** (last line ~2.66) |
| `libsigcpp` | 3.x | **2.x** (~2.10) |

Do **not** enable the manifest feature `cxx-glib-modern-incompatible` for product linking.

## Overlay ports (this tree)

| Overlay port | Upstream | Provides |
|--------------|----------|----------|
| `libsigcpp2` | libsigc++ **2.10.8** | `sigc++-2.0` |
| `glibmm24` | glibmm **2.66.8** | `glibmm-2.4`, `giomm-2.4` |

Wired via `tools/win-native/vcpkg-configuration.json` (`overlay-ports: ["./overlays"]`) and listed in `vcpkg.json`.

```bat
powershell -ExecutionPolicy Bypass -File tools\win-native\Install-HeadlessDeps.ps1 -VcpkgRoot C:\vcpkg
```

Or:

```bat
set VCPKG_OVERLAY_PORTS=%CD%\tools\win-native\overlays
cd tools\win-native
C:\vcpkg\vcpkg.exe install --triplet x64-windows
```

Built against the same triplet’s `glib` (currently 2.90 from stock vcpkg).

### MSVC + glib 2.90 header surgery (`glibmm24` portfile)

glibmm 2.66 emits `using GFooClass = struct _GFooClass;` for types that glib 2.90 declares with `G_DECLARE_FINAL_TYPE` (anonymous Class typedef). MSVC then hits **C2371**. The overlay portfile:

1. Scans installed `glib-2.0` headers for `G_DECLARE_FINAL_TYPE` names.
2. Strips matching `using …Class = …` aliases from glibmm/giomm headers.
3. Injects `#include <gio/gio.h>` (individual `gio/*.h` refuse direct include).
4. Passes meson `-Dcpp_args=['/permissive']`.

Do **not** switch to stock `glibmm`. If meson still fails on removed GLib APIs, pin/overlay an older `glib` next.

## GTK2

Out of scope. GTK2-on-MSVC is a dead end.
