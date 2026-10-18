# vcpkg overlays (headless C++ GLib stack)

## Why this folder exists

Ardour/`libpbd` needs **`glibmm-2.4`** + **`sigc++-2.0`** (pkg-config names).

Current stock vcpkg ports ship:

| Port | Stock version | Ardour needs |
|------|---------------|--------------|
| `glibmm` | 2.68+ / 2.90 (sigc++3) | **2.4 API** (last line ~2.66) |
| `libsigcpp` | 3.x | **2.x** (~2.10) |

Do **not** enable the manifest feature `cxx-glib-modern-incompatible` for product linking — those ABIs will not compile `libs/pbd`.

## Next overlay work (not shipped yet)

Add ports under this directory, then configure with:

```bat
set VCPKG_OVERLAY_PORTS=%CD%\tools\win-native\overlays
```

Suggested ports:

1. `libsigcpp2` — build libsigc++ 2.10.x for MSVC (`x64-windows`, `/MD`)
2. `glibmm24` — build glibmm 2.66.x against that sigc++2 + vcpkg `glib`

Until those land, cmake/win64 can only prove **C** deps (`glib`, `libxml2`, …) via `deps_headless_smoke`. Full `libpbd` remains blocked on the C++ wrappers.

## GTK2

Out of scope here. See migration brief: GTK2-on-MSVC is a dead end; GUI path is GTK3/gvsbuild or Direct2D later.
