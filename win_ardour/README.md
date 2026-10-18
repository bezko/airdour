# win_ardour — Windows-native GUI shell (MSVC)

**Win32 + Direct2D + DirectWrite** host for Airdour. No GTK2.

## What works

- Brand window (**Airdour**), DPI-aware layout
- Editor / Mixer placeholder panes (D2D only)
- Optional **engine attach** to waf-built libardour (Dummy or WASAPI shared)
- Status bar: backend name, sample rate, period
- `--timeout N` for unattended smoke

## Build

### UI-only (no libardour)

```bat
cmake -S cmake\win64 -B build-win64-gui -G "Visual Studio 17 2022" -A x64 -DAIRDOUR_BUILD_WIN_SHELL=ON
cmake --build build-win64-gui --config Release --target airdour_win_shell
build-win64-gui\artifacts\Release\airdour_win_shell.exe --timeout 3 --no-engine
```

### With engine (laptop)

Requires existing waf MSVC build under `build/` (Debug `/MDd` libs):

```bat
cmake -S cmake\win64 -B build-win64-gui-engine -G "Visual Studio 17 2022" -A x64 ^
  -DAIRDOUR_BUILD_WIN_SHELL=ON -DAIRDOUR_WIN_SHELL_WITH_ENGINE=ON ^
  -DAIRDOUR_WAF_BUILD_DIR=%CD%\build
cmake --build build-win64-gui-engine --config Debug --target airdour_win_shell
```

Set `PATH` to vcpkg bins + `build\libs\...`, plus:

```bat
set ARDOUR_BACKEND_PATH=%CD%\build\libs\backends\dummy;%CD%\build\libs\backends\wasapi
set ARDOUR_DLL_PATH=%CD%\build\libs\ardour
build-win64-gui-engine\artifacts\Debug\airdour_win_shell.exe --timeout 3 --backend Dummy
build-win64-gui-engine\artifacts\Debug\airdour_win_shell.exe --timeout 3 --backend WASAPI
```

## Threading

UI thread: init / set_backend / start / stop / paint.  
Backend realtime thread: audio only. Do not call D2D from the process callback.

## Constraints

WASAPI **shared** only. No ASIO, no GTK2, no exclusive mode.
