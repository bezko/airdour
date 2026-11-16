# win_ardour — Windows-native GUI shell (MSVC)

**Win32 + Direct2D + DirectWrite** host for Airdour. No GTK2.

## What works

- Brand window (**Airdour**), DPI-aware layout
- **Transport** — Play / Stop (+ spacebar); engine start/stop; playhead locked to content
- **Editor** — stacked wave lanes + MIDI piano-roll, linked zoom/pan, Fit/Sel/All, scrub
- **Mixer** — strips + Monitor + History; faders, **M/S**, rec-arm, PeakMeter
- **Help** — `?` / F1 / **?** button toggles shortcuts overlay (bindings the shell supports)
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

### Release engine (laptop perf path)

```bat
tools\win-native\configure-waf-release.cmd
tools\win-native\build-waf-release.cmd
cmake -S cmake\win64 -B build-win64-gui-engine-rel -G "Visual Studio 17 2022" -A x64 ^
  -DAIRDOUR_BUILD_WIN_SHELL=ON -DAIRDOUR_WIN_SHELL_WITH_ENGINE=ON ^
  -DAIRDOUR_WAF_BUILD_DIR=%CD%\build-rel
cmake --build build-win64-gui-engine-rel --config Release --target airdour_win_shell
win_ardour\run-engine-shell-release.bat --timeout 6 --backend WASAPI --smoke-perf
```

Uses `build-rel/` (`/MD /O2`). Expect ~11% DSP / ~116 MB WS vs Debug ~60%+ / ~410 MB.

### Debug engine (laptop)

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

Default: create a minimal unnamed Session with **3 empty audio tracks** + **MIDI 1** so mixer PeakMeters attach. Editor lanes read playlist peaks via `AudioRegion::read_peaks` — **empty tracks stay empty**. Mixer **R · arm** + **+ FX** (ACE Reverb on Audio 1, headless insert); transport **Rec** / **Loop** / **Mark** / **Import** / **Save** / **Open** / **Export**. **Export** bounces the master bus to a 16-bit WAV (ExportHandler, no GTK). **Ctrl+Z / Ctrl+Y** undo/redo; click a region to select; **Delete** removes it. **Loop** / **Mark** on the Editor ruler. **MIDI:** WASAPI uses real **WinMME** (not PortAudio); Dummy prefers loopback. Editor **piano-roll** (C3–B4): click/drag to draw notes, click to select, **Delete** removes; notes live in Session `MidiModel`. **ACE Reasonable Synth** (bundled LV2) sits on MIDI 1 and routes audio to master so notes are audible through Dummy/WASAPI shared — no MIDI hardware required. Interactive runs auto-Play; `--timeout` smoke **skips auto-roll** then `ExitProcess(0)`.

`--smoke-<name>` is not a closed list. The full set is whatever `run_smoke_check` / `has_smoke` dispatch in `shell_window.cc`. Rows below are examples.

| Flag / env | Effect |
|------------|--------|
| (default) | Session + PeakMeter + MIDI track/region; arm/rec/import/save/export/undo/loop/piano-roll UI live |
| `--smoke-import` | Write+import test WAV onto track 0 |
| `--smoke-record` | Arm track 0, record ~1.2s, stop — region peaks |
| `--smoke-save-load` | Import → `save_state` → reopen — peaks must return |
| `--smoke-export` | Import if needed → write `export/airdour_smoke_export.wav` |
| `--smoke-undo` | Import → undo (empty) → redo → undo again |
| `--smoke-loop` | Set loop [0.5–2.5]s + add/remove markers |
| `--smoke-midi` | Enumerate MIDI devices + ensure MIDI track |
| `--smoke-midi-edit` | Add/select/delete notes + attach Reasonable Synth → master |
| `--smoke-plugin` | Insert ACE Reverb; toggle bypass; set Blend; verify on route |
| `--smoke-vst3` | Scan system VST3 dirs (+ `VST3_PATH`); list; insert FX on Audio 2 if found |
| `--smoke-mute-solo` | Mute/solo Audio 1 + MIDI (Session Route; RT settle) |
| `--smoke-locate` | Locate/scrub ruler path + shuttle speed + BBT/timecode clock |
| `--smoke-zoom` | Session/regions extents + Editor Fit/Sel/All view windows |
| `--smoke-history` | Undo list names + jump via undo(n) + redo |
| `--smoke-live-level` | Roll the transport on MIDI notes through the synth and report the RT meters + master/monitor device wiring — the check that Play makes signal without needing a device |
| `--smoke-demo` | Build the first-run demo (Drums/Bass/FM Keys + 2 AUX buses + master comp→limiter), roll 5 s, and bounce 12 s of master to `%TEMP%\airdour-demo.wav` |
| `--smoke-demo-proof` | Render the first-run demo's master (16 s loop) to `%TEMP%\airdour-demo-<mode>-<tick>.wav`, measure peak/RMS and report the MIDI-only tracks, instruments, AUX and master chains. `AIRDOUR_DEMO_PROOF_MODE=full\|noaux\|nomaster\|track1..3` selects one configuration per process |
| `--smoke-demo-menu` | Dispatch **File ▸ Airdour Demo Song** exactly as the menu does, then report the whole song and whether the transport is rolling (`smoke-demo-menu ok (tracks=3 notes=224 rolling=1)`) |
| `AIRDOUR_DUMMY_CAPTURE=<file.wav>` | Record the master bus from the backend's own playback cycle — the interleaved buffer the device would receive — while the app plays. The backend calls `prepare()` from its non-realtime start, which sets up the file, the writer thread and a pre-sized buffer; the audio thread only converts the bus buffers into that array, so it never opens, writes, locks or allocates. `AIRDOUR_DUMMY_CAPTURE_SECS` sets the length (default 18). Nothing happens unless the variable is set |
| `AIRDOUR_DEMO_TRACE=1` | Print the first-run demo's per-second `demo transport=… rolling=…` line (what `e2e/first-launch/check.py` reads); ordinary launches stay quiet |
| `e2e/windows-desktop` (pytest) | pywinauto/UIA end-to-end: launch the packaged shell on an isolated profile, answer the recovery prompt, scroll the roll to the kit, step-enter a bar of 8th notes and assert the autosaved session gains them |
| `--smoke-perf` | Sample WASAPI callback µs vs period, DSP %, xruns, working set (`--timeout` = window) |
| `--session DIR` | Open existing session folder after engine start |
| `AIRDOUR_WIN_SHELL_SESSION=0` | Skip Session; stub meters; empty lanes |
| `AIRDOUR_WIN_SHELL_DEMO_TONES=1` | Opt-in sine regions at Session create (prefer Import) |

## First-run demo session

A fresh install opens onto a demo and starts playing it once per user. The session is
built by `EngineHost::build_demo_session ()` into `<Documents>\Airdour\Airdour Demo`;
the one-shot marker is `%LOCALAPPDATA%\Airdour\first-run-demo` (delete it to replay).

A redirected profile is honoured: when `USERPROFILE` / `LOCALAPPDATA` are set they win over the machine folders, so a wiped "first launch" run and the desktop E2E keep their demo and marker out of the developer's own profile.

| Part | Contents |
| --- | --- |
| Drums | `Airdour Drums` VST3 — kick/snare/clap/hats/toms/cymbals from oscillators + noise |
| Bass | `Airdour Mono` VST3 — saw/square, TPT filter, ADSR |
| FM Keys | `Airdour FM` VST3 — 8-voice 2-op FM chords + lead |
| AUX Chorus | `Airdour Chorus` VST3, fed by sends from all three tracks |
| AUX Space | ACE Reverb + ACE Delay, fed by sends from all three tracks |
| Master | ACE Compressor (pre-fader) → ACE Limiter (post-fader, −1 dB ceiling) |

The 8-bar arrangement loops; nothing is sampled — every sound is synthesised at run time.
The instrument bundle ships as `VST3\AirdourSynths.vst3` in the portable zip and installer,
and `run-airdour.bat` puts it on `VST3_PATH`.
**Editing it in the piano roll.** The roll opens on the part the session shows and
snaps the pitch window onto that part's notes, so the demo's drums appear instead of
empty rows above them; **PageUp**/**PageDown** move the window by an octave. **F4**
arms step entry, and the typing keyboard follows the visible window: with `24..47` in
view **Z** writes MIDI 36 (kick), **X** 38 (snare) and **G** 42 (closed hat), the
rest of the rows map to the other kit pieces, and **→** steps one grid step without
a note. Every placement logs `step input pitch=<p> start=<s>s`, which is what the desktop E2E asserts.

| `ARDOUR_PANNER_PATH` | Extra search path if panners are not under `%ARDOUR_DLL_PATH%\panners` |
| `VST3_PATH` / `AIRDOUR_SMOKE_VST3` | Extra VST3 roots / preferred plugin name for `--smoke-vst3` |

**MIDI / audible path:**
- **Soft synth:** ACE Reasonable Synth LV2 on MIDI 1 → master → Dummy/WASAPI shared (no MIDI HW needed). Helper: `win_ardour\ensure-reasonable-synth.bat`.
- Live MIDI **capture** still needs a MIDI input (USB / loopMIDI). GS Wavetable is optional; soft-synth is the default hear-path.

**Audio FX (v0, no GTK plugin UI):**
- **ACE Reverb** LV2 inserted pre-fader on Audio 1 at Session create (and via Mixer **+ FX** / `--smoke-plugin`). Helper: `win_ardour\ensure-a-reverb.bat`.
- Mixer: click FX label to **bypass**/enable; drag the **Blend** bar under it (wet mix). No LV2 GUI.
- **VST3** DSP scan/load via libardour (`VST3_SUPPORT`); `--smoke-vst3` inserts on Audio 2. No native VST3 editor (GTK HWND host is out of scope).

**MSVC notes:**
- `process_lock().trylock()` while already held deadlocks (SRWLOCK). `Delivery::configure_io` and related asserts are `#ifndef PLATFORM_WINDOWS`.
- Panner DLLs must be discoverable or `reset_panners` fatals.
- WASAPI Session create requires non-zero MIDI `raw_buffer_size` (ThreadBuffers / VSTBuffer).

## Threading

UI thread: init / set_backend / start / stop / paint.  
Backend realtime thread: audio only. Do not call D2D from the process callback.

## Portable zip (no source tree)

Default packaged artifact is **MSVC Release** (`build-rel` + `build-win64-gui-engine-rel`):

```bat
powershell -File tools\win-native\ci\Package-WinShell-Portable.ps1 -RepoRoot %CD% -OutDir dist -Version local -Mode Engine -Config Release
```

`-Config Auto` (default) picks Release when those trees exist. Pass `-Config Debug` for `*-shell-debug.zip`.

Unzip `dist\airdour-*-win64-shell.zip` anywhere and run `run-airdour.bat`. See `tools/win-native/ci/` and the release-pipeline docs for GHA UiOnly vs Engine Release.

## Constraints

WASAPI **shared** only. No ASIO, no GTK2, no exclusive mode.
