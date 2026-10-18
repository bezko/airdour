# win_ardour — Windows-native GUI shell (MSVC)

Minimal **Win32 + Direct2D + DirectWrite** host for Airdour on win64.

## Why this exists

`gtk2_ardour` + bundled GTK2 (`libs/tk`) + Cairo canvas is the upstream GUI.
GTK2-on-MSVC is a dead end (see `docs` store notes). This directory is the
**replacement product UI path**: native window first, then D2D canvas/waveview
slices — not a GTK2 port.

## Current slice

- Create top-level window titled **Airdour**
- Direct2D clear/present with a simple atmospheric backdrop
- DirectWrite brand + one supporting line
- Message loop; optional `--timeout N` for unattended smoke

Does **not** yet: link libardour, open sessions, draw waveforms, or host menus.

## Build

```bat
cmake -S cmake\win64 -B build-win64-gui -G "Visual Studio 17 2022" -A x64 -DAIRDOUR_BUILD_WIN_SHELL=ON
cmake --build build-win64-gui --config Release --target airdour_win_shell
```

Run (auto-quit after 3s for smoke):

```bat
build-win64-gui\artifacts\Release\airdour_win_shell.exe --timeout 3
```

## Stack constraints

- WASAPI shared only (engine stays in hardour / backends)
- No ASIO, no GTK2, no exclusive WASAPI in this shell
