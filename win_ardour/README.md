# win_ardour — Windows-native GUI shell (MSVC)

**Win32 + Direct2D + DirectWrite** host for Airdour. No GTK2.

## What works

- Brand window (**Airdour**), DPI-aware layout
- **Transport** — Play / Stop (+ spacebar); engine start/stop; playhead locked to content
- **Editor** — 3 stacked wave lanes (Audio 1–3), linked zoom/pan, labels
- **Mixer** — 3 strips with faders + **Session PeakMeter** (Dummy by default; stub if Session fails)
- Playhead follows Session transport when attached, else engine `processed_samples`
- Optional **engine attach** (Dummy or WASAPI shared)
- `--timeout N` for unattended smoke

## Build

### UI-only (no libardour)

```bat
cmake -S cmake\win64 -B build-win64-gui -G "Visual Studio 17 2022" -A x64 -DAIRDOUR_BUILD_WIN_SHELL=ON
cmake --build build-win64-gui --config Release --target airdour_win_shell
build-win64-gui\artifacts\Release\airdour_win_shell.exe --timeout 3 --no-engine
```

### With engine (laptop)

Requires existing waf MSVC build under `build/` (Debug `/MDd` libs), including **panner** DLLs:

```bat
python waf build --targets=libardour,libardour_pan1in2out,libardour_pan2in2out,libardour_panvbap,libardour_panbalance
mkdir build\libs\ardour\panners 2>nul
copy /Y build\libs\panners\1in2out\pan1in2out.dll build\libs\ardour\panners\
copy /Y build\libs\panners\2in2out\pan2in2out.dll build\libs\ardour\panners\
copy /Y build\libs\panners\stereobalance\panbalance.dll build\libs\ardour\panners\
copy /Y build\libs\panners\vbap\panvbap.dll build\libs\ardour\panners\
```

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

## Session / PeakMeter

Default: create a minimal unnamed Session with **3 empty audio tracks** so mixer PeakMeters are live (levels move when rolling with click / content). `--timeout` exits via `TerminateProcess(0)` to skip join hangs.

| Flag / env | Effect |
|------------|--------|
| (default) | Session + PeakMeter (Dummy). WASAPI Session create may fall back to stubs |
| `AIRDOUR_WIN_SHELL_SESSION=0` | Skip Session; stub meters only |
| `AIRDOUR_WIN_SHELL_DEMO_TONES=1` | Opt-in sine wav regions (can hang/AV under Dummy on MSVC) |
| `ARDOUR_PANNER_PATH` | Extra search path if panners are not under `%ARDOUR_DLL_PATH%\panners` |

**MSVC notes:**
- `process_lock().trylock()` while already held deadlocks (SRWLOCK). `Delivery::configure_io` and related asserts are `#ifndef PLATFORM_WINDOWS`.
- Panner DLLs must be discoverable or `reset_panners` fatals.

## Threading

UI thread: init / set_backend / start / stop / paint.  
Backend realtime thread: audio only. Do not call D2D from the process callback.

## Constraints

WASAPI **shared** only. No ASIO, no GTK2, no exclusive mode.
