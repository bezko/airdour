# Canvas C++ spike (MSVC) — cairomm / pangomm

Isolated vcpkg tree to prove **cairomm** installs on MSVC without touching the main `tools/win-native/vcpkg_installed` (that tree uses overlay **sigc++2 / glibmm-2.4**; stock cairomm pulls **sigc++3 / glibmm 2.68+**).

**Not product chrome.** Airdour UI stays Win32 + Direct2D.

## Install

```powershell
powershell -ExecutionPolicy Bypass -File tools\win-native\canvas-cxx-spike\Install-CanvasCxxSpike.ps1 `
  -VcpkgRoot C:\vcpkg
```

Optional `-WithGtkmm4` pulls stock **gtkmm 4.x** (GTK4). That does **not** unlock `libs/canvas`.

## Probes

```bat
tools\win-native\canvas-cxx-spike\build-and-run.cmd
```

| Target | Expected | Result (TUFNUTZ 2026-10-19) |
|--------|----------|------------------------------|
| `cairomm_probe` | PASS | **PASS** — cairomm 1.19.1 / cairo 1.18.6; ImageSurface inked |
| `canvas_types_probe` | FAIL compile | **FAIL** — `ydkmm/types.h` missing via `gtkmm2ext/colors.h` |

## Why `libs/canvas` still fails

1. `canvas/types.h` → `gtkmm2ext/colors.h` → **`ydkmm/types.h`** (Ardour GTK2 fork).
2. Full canvas `wscript` also `use`s **libytkmm** + **libgtkmm2ext**.
3. Stock vcpkg **gtkmm** is **4.x only** (no gtkmm2 / gtkmm3 ports).
4. **gtkmm2 on MSVC is not available** via vcpkg; Ardour’s GTK2 stack (`libs/tk`, ytk/ydk) is culled on the Windows-only path.

**Reuse strategy:** keep thin Cairo C (`win_ardour/cairo_wave_blit`) behind D2D. See store doc `gtk-native-msvc.md`.
