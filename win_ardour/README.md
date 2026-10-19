# win_ardour — Windows-native GUI shell (MSVC)

**Win32 + Direct2D + DirectWrite** host for Airdour. No GTK2.

## What works

- Brand window (**Airdour**), DPI-aware layout
- **Transport** — Play / Stop (+ spacebar); engine start/stop; playhead locked to content
- **Editor** — 3 stacked wave lanes (Audio 1–3), linked zoom/pan, labels
- **Mixer** — 3 strips with faders + **Session PeakMeter** (Dummy and WASAPI; stub only if Session fails / opted out)
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

**Do not double-click the exe alone** — without PATH you get `ptformat.dll was not found` (and other waf DLLs). Prefer the launcher:

```bat
win_ardour\run-engine-shell.bat --timeout 3 --backend Dummy
win_ardour\run-engine-shell.bat --timeout 3 --backend WASAPI
win_ardour\run-engine-shell.bat
```

Or set PATH yourself (include **`build\libs\ptformat`**):

```bat
set PATH=%CD%\tools\win-native\vcpkg_installed\x64-windows\debug\bin;%CD%\tools\win-native\vcpkg_installed\x64-windows\bin;%CD%\build\libs\pbd;%CD%\build\libs\temporal;%CD%\build\libs\midi++2;%CD%\build\libs\evoral;%CD%\build\libs\ardour;%CD%\build\libs\audiographer;%CD%\build\libs\ptformat;%CD%\build\libs\ctrl-interface\control_protocol;%CD%\build\libs\backends\dummy;%CD%\build\libs\backends\wasapi;%CD%\build\libs\ardour\panners;%PATH%
set ARDOUR_BACKEND_PATH=%CD%\build\libs\backends\dummy;%CD%\build\libs\backends\wasapi
set ARDOUR_DLL_PATH=%CD%\build\libs\ardour
build-win64-gui-engine\artifacts\Debug\airdour_win_shell.exe --timeout 3 --backend Dummy
build-win64-gui-engine\artifacts\Debug\airdour_win_shell.exe --timeout 3 --backend WASAPI
```

Engine builds also **copy** `ptformat.dll` / `ardour.dll` / … next to the exe on POST_BUILD (still need vcpkg + VsDevCmd CRT on PATH).

## Session / PeakMeter / Editor / Record / Import / Save / Export / Undo / Loop / MIDI

Default: create a minimal unnamed Session with **3 empty audio tracks** + **MIDI 1** so mixer PeakMeters attach. Editor lanes read playlist peaks via `AudioRegion::read_peaks` — **empty tracks stay empty**. Mixer **R · arm**; transport **Rec** / **Loop** / **Mark** / **Import** / **Save** / **Open** / **Export**. **Export** bounces the master bus to a 16-bit WAV (ExportHandler, no GTK). **Ctrl+Z / Ctrl+Y** undo/redo; click a region to select; **Delete** removes it. **Loop** / **Mark** on the Editor ruler. **MIDI:** WASAPI uses real **WinMME** (not PortAudio); Dummy prefers loopback. Editor shows a piano-roll stub + device count. Interactive runs auto-Play; `--timeout` smoke **skips auto-roll** then `ExitProcess(0)`.

| Flag / env | Effect |
|------------|--------|
| (default) | Session + PeakMeter + MIDI track; arm/rec/import/save/export/undo/loop UI live |
| `--smoke-import` | Write+import test WAV onto track 0 |
| `--smoke-record` | Arm track 0, record ~1.2s, stop — region peaks |
| `--smoke-save-load` | Import → `save_state` → reopen — peaks must return |
| `--smoke-export` | Import if needed → write `export/airdour_smoke_export.wav` |
| `--smoke-undo` | Import → undo (empty) → redo → undo again |
| `--smoke-loop` | Set loop [0.5–2.5]s + add/remove markers |
| `--smoke-midi` | Enumerate MIDI devices + ensure MIDI track |
| `--session DIR` | Open existing session folder after engine start |
| `AIRDOUR_WIN_SHELL_SESSION=0` | Skip Session; stub meters; empty lanes |
| `AIRDOUR_WIN_SHELL_DEMO_TONES=1` | Opt-in sine regions at Session create (prefer Import) |
| `ARDOUR_PANNER_PATH` | Extra search path if panners are not under `%ARDOUR_DLL_PATH%\panners` |

**MIDI hardware note:** Live note capture needs a MIDI input (USB controller / loopMIDI). Stock Windows often only exposes **Microsoft GS Wavetable Synth** (output). Enumeration and Session MIDI tracks work without hardware.

**MSVC notes:**
- `process_lock().trylock()` while already held deadlocks (SRWLOCK). `Delivery::configure_io` and related asserts are `#ifndef PLATFORM_WINDOWS`.
- Panner DLLs must be discoverable or `reset_panners` fatals.
- WASAPI Session create requires non-zero MIDI `raw_buffer_size` (ThreadBuffers / VSTBuffer).

## Threading

UI thread: init / set_backend / start / stop / paint.  
Backend realtime thread: audio only. Do not call D2D from the process callback.

## Constraints

WASAPI **shared** only. No ASIO, no GTK2, no exclusive mode.
