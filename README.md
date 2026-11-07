# Airdour

> **A lightweight, Windows-native Digital Audio Workstation (DAW)** powered by the Ardour 9 audio engine, a hardware-accelerated **Win32 + Direct2D / DirectWrite** shell, and a native **WASAPI** audio backend (shared by default, optional exclusive) with **WinMME MIDI**.

[![Windows MSVC CI](https://github.com/bezko/airdour/actions/workflows/windows-msvc.yml/badge.svg)](https://github.com/bezko/airdour/actions/workflows/windows-msvc.yml)
[![License: GPL-2.0](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](COPYING)
[![Platform: Windows x64](https://img.shields.io/badge/Platform-Windows%2010%2F11%20x64-0078D6.svg)](cmake/win64/CMakeLists.txt)
[![Audio: WASAPI](https://img.shields.io/badge/Audio-WASAPI-success.svg)](libs/backends/wasapi/README.md)
[![UI: Direct2D](https://img.shields.io/badge/UI-Direct2D%20%2B%20DirectWrite-purple.svg)](win_ardour/README.md)
[![Project site](https://img.shields.io/badge/site-bezko.github.io%2Fairdour-25d9bc)](https://bezko.github.io/airdour/)

---

## Table of Contents

1. [Architecture & System Overview](#architecture--system-overview)
2. [Core Innovations & Technical Differentiators](#core-innovations--technical-differentiators)
3. [Subsystems](#subsystems)
   - [Direct2D Native GUI Shell (`win_ardour`)](#1-direct2d-native-gui-shell-win_ardour)
   - [WASAPI Backend (`libs/backends/wasapi`)](#2-wasapi-backend-libsbackendswasapi)
   - [Headless Engine Core (`hardour` / `libardour`)](#3-headless-engine-core-hardour--libardour)
   - [DSP Engine & Audio Effects](#4-dsp-engine--audio-effects)
   - [Editing, Time-Stretching & Pitch-Shifting](#5-editing-time-stretching--pitch-shifting)
   - [Real-time Analysis & Metering](#6-real-time-analysis--metering)
4. [Keyboard Shortcuts Reference](#keyboard-shortcuts-reference)
5. [CLI Flags & Smoke Test Harness](#cli-flags--smoke-test-harness)
6. [Building from Source](#building-from-source)
   - [Prerequisites](#prerequisites)
   - [Dependency Setup via vcpkg](#dependency-setup-via-vcpkg)
   - [UI-Only Build (CMake)](#ui-only-build-cmake)
   - [Full Engine Release Build (Waf + CMake)](#full-engine-release-build-waf--cmake)
7. [Packaging & CI/CD Pipeline](#packaging--cicd-pipeline)
8. [Technical Constraints & Design Decisions](#technical-constraints--design-decisions)
9. [Repository Layout](#repository-layout)
10. [License & Acknowledgments](#license--acknowledgments)

---

## Architecture & System Overview

Airdour decouples the mature, sample-accurate, 64-bit floating-point audio processing engine of Ardour from legacy POSIX/GTK dependencies on Windows. By replacing GTK2 and X11 shims with native Windows API primitives (**Direct2D 1.0**, **DirectWrite**, **MMCSS Pro Audio**, and **WASAPI**), Airdour achieves immediate startup, minimal working set memory overhead, and smooth 60 FPS DPI-aware rendering.

```mermaid
flowchart TD
    subgraph UI ["User Interface Layer (win_ardour)"]
        HWND["Win32 Shell Window (shell_window.cc)"]
        D2D["Direct2D Render Target (Hardware Accelerated)"]
        DW["DirectWrite Typography Engine"]
        Panels["Editor Lanes · Piano Roll · Mixer Strips · Meter Panels"]
        HWND --> D2D
        HWND --> DW
        D2D --> Panels
    end

    subgraph Control ["Input"]
        KB["Win32 Keyboard & Mouse Input"]
    end

    subgraph Host ["Engine Host Interface (win_ardour/engine_host.cc)"]
        EH["EngineHost Adapter"]
        Snap["Atomic Lock-Free UI Snapshot (EngineSnapshot)"]
        RT_Queue["Thread-Safe Command Queue"]
        EH --> Snap
        Snap --> Panels
        KB --> RT_Queue
    end

    subgraph CoreEngine ["Ardour Core Engine (libardour / libpbd)"]
        Session["Session State & Graph"]
        AudioTrack["Audio Tracks & Playlists"]
        MidiTrack["MIDI Tracks & Evoral Sequences"]
        TempoMap["TempoMap & Metric Grid"]
        ExportHandler["ExportHandler (Headless Master WAV Bounce)"]
        Session --> AudioTrack
        Session --> MidiTrack
        Session --> TempoMap
        Session --> ExportHandler
    end

    subgraph AudioBackends ["Audio & MIDI I/O Layer"]
        WASAPI["WASAPI Backend, shared by default (wasapi_backend.cc)"]
        WinMME["WinMME MIDI Subsystem (winmmemidi_io.cc)"]
        Dummy["Dummy Audio Backend (Headless / Testing)"]
        MMCSS["MMCSS Pro Audio Thread Priority"]
        WASAPI --> MMCSS
    end

    subgraph DSP ["Plugin & Signal Processing Layer"]
        ACE["Headless ACE LV2 Plugins\n(Reverb, EQ, Comp, Gate, Limiter, Delay, Synth)"]
        VST3["VST3 Host (DSP-only scan and process)"]
        VST2["VST2-64 in process · VST2-32 bridge"]
        RB["RubberBand (Time-Stretch & Pitch-Shift)"]
    end

    RT_Queue --> Session
    EH --> Session
    Session --> AudioBackends
    AudioTrack --> DSP
    MidiTrack --> DSP
```

### Threading Architecture

1. **UI Thread (Main STA)**: Executes the Win32 message loop (`wmain` in [main.cc](win_ardour/main.cc)), handles keyboard/mouse interaction, and issues Direct2D drawing commands in [shell_window.cc](win_ardour/shell_window.cc). **Never** executes blocking disk I/O or DSP code.
2. **Realtime Audio Thread**: Driven by WASAPI event-driven process buffers (`wait_for_period` in [wasapi_backend.cc](libs/backends/wasapi/wasapi_backend.cc)). Boosted to Windows MMCSS `"Pro Audio"` category with `AVRT_PRIORITY_HIGH`. Responsible for audio sample deinterleaving, format conversion (float32/int16/int24/int32), buffer routing, and DSP plugin processing.
3. **MIDI Processing**: Real-time MIDI events are gathered asynchronously via Windows Multimedia Extensions (`winmm.dll`) and ingested into the audio cycle buffer with microsecond timestamps.
4. **Analysis & Background Workers**: Peak calculation for wave strips (`AudioRegion::read_peaks`), session autosave threads, and RubberBand offline stretches operate in background worker threads without interrupting audio stream continuity.

---

## Core Innovations & Technical Differentiators

| Feature | Upstream Ardour (Windows) | Airdour |
| :--- | :--- | :--- |
| **Windowing & GUI Toolkit** | GTK2 + X11 compatibility shims + Cairo | **Native Win32 + Direct2D + DirectWrite** |
| **Audio Backend** | PortAudio / ASIO third-party drivers required | **Native WASAPI** (shared by default; optional exclusive; no driver install) |
| **MIDI Subsystem** | PortAudio MIDI Bridge / ALSA-layer emulation | **Native WinMME subsystem** ([winmmemidi_io.cc](libs/backends/portaudio/winmmemidi_io.cc)) |
| **Toolchain & Compiler** | MinGW / MSYS2 / GCC cross-compilation | **Visual Studio 2022 (MSVC / cl.exe) + CMake + Waf** |
| **Package Format** | Installer requiring extensive GTK runtime | **Self-contained portable ZIP** and an NSIS installer |

---

## Subsystems

### 1. Direct2D Native GUI Shell (`win_ardour`)

Located in [`win_ardour/`](win_ardour/README.md), the user interface is completely written in C++17 utilizing Windows Direct2D and DirectWrite.

- **High-DPI Awareness**: Per-Monitor V2 DPI scaling (`GetDpiForWindow` / `ID2D1Factory`). Layout dimensions, fonts, and controls dynamically adjust across display migrations.
- **Stacked Track Editor**: Multi-lane audio timeline rendering region waveforms directly from mipmapped audio peak caches (`AudioRegion::read_peaks`).
- **Piano-Roll MIDI Editor**: Note sequencing lane (C3–B4) supporting click-drag note creation, length trimming, velocity dragging, multi-note selection, clipboard operations, and QWERTY step-entry.
- **Mixer Strips**: Vertical channel strips featuring level faders, pan azimuth knobs, aux send controls, peak-hold clip indicators, Mute, Solo, and Record-Arm toggles.
- **Transient Overlays**: Dismissible startup splash, interactive shortcuts reference overlay (`F1` / `?`), Preferences modal, Session History navigator, and Recent Sessions drawer.

### 2. WASAPI Backend (`libs/backends/wasapi`)

Located in [`libs/backends/wasapi/`](libs/backends/wasapi/README.md), implementing the full `ARDOUR::AudioBackend` interface.

- **Shared by default**: Opens `AUDCLNT_SHAREMODE_SHARED`, so other programs can play at the same time. **Exclusive render is optional** (Preferences Shared/Exclusive, `AudioBackend::set_exclusive_mode`, or `AIRDOUR_WASAPI_EXCLUSIVE=1`). If the device or format refuses exclusive, the backend opens shared. Capture stays shared.
- **Period Laddering & Rate Adaptation**: Dynamically negotiates formats using `IMMDeviceEnumerator` and `IAudioClient3::InitializeSharedAudioStream`. Snaps to hardware engine period fundamentals (e.g. 128, 240, 256, or 480 samples).
- **Float & Integer Conversion**: Optimized SIMD-compatible sample format translation routines for IEEE 32-bit float, 16-bit PCM, 24-bit PCM, and 32-bit PCM.
- **WinMME MIDI Driver**: Native Win32 MIDI input and output ports with millisecond-accurate timestamping, GS Wavetable routing, and loopback compatibility.

### 3. Headless Engine Core (`hardour` / `libardour`)

The Ardour DAW engine can run fully decoupled from any GUI:
- **`ExportHandler`**: Headless master-bus bounce to 16-bit WAV with TPDF dither (`D_Tri`), plus stem export (`airdour_win_shell`: `Ctrl+E` or `--smoke-export`).

### 4. DSP Engine & Audio Effects

Airdour embeds native, headless LV2 audio processing plugins pre-wired into session tracks:

- **Audio 1 — ACE Reverb**: High-density stereo algorithmic reverb featuring direct wet/dry **Blend** control and bypass toggling from the mixer pane.
- **Audio 2 — ACE Compressor**: Studio compressor with adjustable threshold, compression ratio, attack, release, and sidechain input.
- **Audio 3 — ACE EQ**: 3-band parametric equalizer featuring Low, Mid, and High shelf/peak gain controls.
- **Audio 4 — ACE Delay**: Tempo-synchronized delay line with Feedback and Time sliders.
- **Audio 5 — ACE Limiter**: Brickwall peak limiter with ceiling control and fast lookahead release.
- **Audio 6 — ACE Gate**: Noise gate with configurable threshold and hold parameters.
- **MIDI 1 — ACE Reasonable Synth**: Built-in LV2 synthesizer that converts MIDI note sequences to audible audio out of the box with zero external sound fonts or plugins required.
- **VST3 Host Architecture**: Integrated VST3 plugin scanning (`VST3_SUPPORT`) for standard Windows system paths (`C:\Program Files\Common Files\VST3`) and custom `VST3_PATH` locations.
- **Also in the shell**: more than one MIDI track, a per-track and master insert rack, in-process VST2-64, and out-of-process VST2-32 (`ardour-vst2-bridge-x86.exe`). Current behavior is in [`win_ardour/README.md`](win_ardour/README.md).

### 5. Editing, Time-Stretching & Pitch-Shifting

The editing engine provides instant non-destructive audio manipulation:
- **Non-destructive Region Editing**: Region trimming, body moving, splitting (`S`), fades and crossfades (`F`), region duplicate (`Ctrl+D`), and deletion (`Delete`).
- **RubberBand Integration**: Headless pitch-shifting (`R` / `Shift+R` semitones) and time-stretching (`T` / `Shift+T` 1.5× / 0.75×) via the integrated RubberBand library.
- **Clean-up & Mastering Operations**: Normalize to -1 dBFS (`N`), reverse playback (`V`), silence stripping at -40 dBFS threshold (`I`), track bounce-in-place (`B`), and track freezing (`Z`).

### 6. Real-time Analysis & Metering

Hardware-accelerated visual meters rendered via Direct2D:
- **Spectrum Analyzer (`Y`)**: Multi-band Goertzel filter bank analyzing spectral energy distributions in real time on the master bus.
- **Phase Correlation & Goniometer (`X`)**: Real-time Lissajous stereo phase goniometer and numerical correlation meter ([-1.0, +1.0]) for mono compatibility checking.
- **Loudness Meter (`J`)**: Integrated ITU-R BS.1770 K-weighted approximate LUFS meter.

---

## Keyboard Shortcuts Reference

Press `F1` or `?` in the shell. `ShellWindow::draw_help_overlay` in [`win_ardour/shell_window.cc`](win_ardour/shell_window.cc) is the binding list. A short subset is on the [project site](https://bezko.github.io/airdour/#keys).

---

## CLI Flags & Smoke Test Harness

The `airdour_win_shell.exe` executable doubles as an automated verification test harness. It accepts automated execution flags for CI/CD and unattended profiling:

```text
Usage: airdour_win_shell.exe [OPTIONS]
```

### General Configuration Flags
- `--backend [WASAPI|Dummy]`: Select audio backend (defaults to `WASAPI` if available, else `Dummy`).
- `--period N`: Override audio buffer size (e.g. `--period 256` or `--period 480`).
- `--session <PATH>`: Open an existing session directory on startup.
- `--timeout N`: Automatically exit the process after `N` seconds with exit code `0`.
- `--no-engine`: Launch the Direct2D UI shell in standalone mode without starting `libardour`.
- `--no-splash`: Suppress the startup splash screen.

### Automated Smoke Test Flags

`--smoke-<name>` is not a closed list. The set is whatever `ShellWindow::run_smoke_check` / `has_smoke` dispatch in [`win_ardour/shell_window.cc`](win_ardour/shell_window.cc). The rows below are examples. Run a flag with `--timeout 3` for a headless pass:

| Flag | Subsystem Tested | Success Criteria |
| :--- | :--- | :--- |
| `--smoke-import` | Audio File Ingestion | Generates and imports a test WAV file into track 0. |
| `--smoke-record` | Realtime Recording | Arms track 0, records ~1.2s of audio, and computes region peaks. |
| `--smoke-save-load` | Persistence | Imports audio, writes session XML to disk, re-reads, and verifies peaks. |
| `--smoke-export` | Master WAV Bounce | Renders master bus to `export/airdour_smoke_export.wav` via `ExportHandler`. |
| `--smoke-undo` | Transaction Engine | Performs edit → undo (track empty) → redo → verify region restoration. |
| `--smoke-loop` | Transport Ranges | Configures [0.5s – 2.5s] loop boundaries and checks transport clamping. |
| `--smoke-midi` | WinMME Subsystem | Enumerates hardware/virtual MIDI endpoints and initializes MIDI strip. |
| `--smoke-midi-edit` | Piano-Roll Model | Adds notes, verifies selection, deletes notes, routes to Reasonable Synth. |
| `--smoke-midi-quantize`| MIDI Snap Grid | Validates note alignment to beat/bar subdivisions. |
| `--smoke-midi-transpose`| MIDI Transpose | Verifies ±1 and ±12 semitone note transformation. |
| `--smoke-plugin` | LV2 Plugin Host | Inserts ACE Reverb into Audio 1, toggles bypass, sets wet/dry blend. |
| `--smoke-vst3` | VST3 Host Engine | Scans standard system VST3 directories and attaches plugin to Audio 2. |
| `--smoke-mute-solo` | Mixer Strip Control | Toggles Mute and Solo states on Audio 1 and MIDI 1 tracks. |
| `--smoke-stretch` | RubberBand Stretch | Performs 1.5× offline audio time-stretch. |
| `--smoke-pitch` | RubberBand Pitch | Performs ±1 semitone pitch shifting with duration preserved. |
| `--smoke-reverse` | Waveform Reversal | Reverses audio samples within region boundaries. |
| `--smoke-normalize` | Audio Gain Normalization | Scans peak amplitudes and normalizes region to -1 dBFS. |
| `--smoke-strip-silence`| Silence Detection | Splits audio regions at -40 dBFS threshold boundaries. |
| `--smoke-consolidate` | Bounce-in-Place | Renders selection with track gain/plugins directly onto a new audio file. |
| `--smoke-freeze` | Track Freezing | Bounces track audio and disables real-time DSP load. |
| `--smoke-perf` | Performance Profiling | Samples WASAPI callback jitter, CPU %, xrun count, and memory working set. |
| `--smoke-midi-multi` | Multi-MIDI | Creates two MIDI tracks and a note on each. |
| `--smoke-fx-chain` | Insert rack | Exercises per-track and master rack slots. |
| `--smoke-vst2` | VST2-64 | Scans and inserts a VST2 audio effect when the catalog has one. |
| `--smoke-vst2-bridge` | VST2-32 bridge | Opens the x86 bridge when `AIRDOUR_SMOKE_VST2_32` points at an i386 DLL. |
| `--smoke-vst3-instrument` | VST3 instrument | Attaches a catalog instrument to the active MIDI track. |

---

## Building from Source

### Prerequisites

1. **Operating System**: Windows 10 or Windows 11 (64-bit).
2. **Compiler & IDE**: Visual Studio 2022 (MSVC Desktop C++ workload) + Windows 10/11 SDK.
3. **Build Tools**:
   - [CMake](https://cmake.org/) (Version ≥ 3.20)
   - [Python 3.x](https://www.python.org/) (Required for Waf)
   - [Git](https://git-scm.com/)
4. **vcpkg**: Microsoft C++ Library Manager installed at a short path (recommended: `C:\vcpkg`).

---

### Dependency Setup via vcpkg

Airdour provides an automated PowerShell installation script located in [`tools/win-native/Install-HeadlessDeps.ps1`](tools/win-native/Install-HeadlessDeps.ps1).

> [!IMPORTANT]
> Upstream vcpkg supplies modern `glibmm` 2.68+ and `sigc++` 3.x, which are ABI-incompatible with Ardour. Airdour includes custom overlay ports for **`glibmm-2.4`** and **`sigc++-2.0`** in [`tools/win-native/overlays/`](tools/win-native/overlays/README.md).

Run the automated dependency bootstrapper:

```powershell
powershell -ExecutionPolicy Bypass -File tools\win-native\Install-HeadlessDeps.ps1 -VcpkgRoot C:\vcpkg
```

This installs the headless C/C++ dependency tree: `glib`, `libxml2`, `libsndfile`, `libcurl`, `libarchive`, `pthreads4w`, and the custom `glibmm24` / `libsigcpp2` overlays.

---

### UI-Only Build (CMake)

Ninja presets: [`cmake/win64/CMakePresets.json`](cmake/win64/CMakePresets.json). UI-only shell, no `libardour`. CI smoke is `ctest --preset ui` (`airdour_win_shell`).

```cmd
cd cmake\win64
cmake --preset ui
cmake --build --preset ui
ctest --preset ui
cd ..\..
build-ninja-ui\artifacts\airdour_win_shell.exe --timeout 3 --no-engine
```

---

### Full Engine Release Build (Waf + CMake)

```cmd
tools\win-native\configure-waf-release.cmd
tools\win-native\build-waf-release.cmd
cd cmake\win64
cmake --preset engine
cmake --build --preset engine
ctest --preset engine
cd ..\..
build-ninja-engine\artifacts\airdour_win_shell.exe
```

---

## Packaging & CI/CD Pipeline

Airdour includes production-grade packaging automation for distribution:

### 1. Portable Release Zip Generator
The script [`tools/win-native/ci/Package-WinShell-Portable.ps1`](tools/win-native/ci/Package-WinShell-Portable.ps1) stages a zero-install portable folder containing:
- `airdour_win_shell.exe`
- All required dynamic libraries (`ardour.dll`, `pbd.dll`, `temporal.dll`, `ptformat.dll`, vcpkg CRT runtime)
- Panner modules (`pan1in2out.dll`, `panbalance.dll`, `panvbap.dll`)
- Bundled ACE LV2 plugins: Reasonable Synth, Reverb, Compressor, EQ, Delay, Limiter, and Gate
- Self-contained launcher script (`run-airdour.bat`)

```powershell
powershell -File tools\win-native\ci\Package-WinShell-Portable.ps1 -RepoRoot $PWD -OutDir dist -Version "0.1.0" -Mode Engine -Config Release
```

### 2. NSIS Windows Installer
[`tools/win-native/installer/airdour.nsi`](tools/win-native/installer/airdour.nsi) is a small [NSIS](https://nsis.sourceforge.io/) script (open source). [`tools/win-native/ci/Package-WinShell-Nsis.ps1`](tools/win-native/ci/Package-WinShell-Nsis.ps1) compiles it into a 64-bit setup executable (`airdour-*-win64-setup.exe`, or `*-setup-ui.exe` for the UI-only shell). The installer copies the portable tree for the current user, adds a Start menu shortcut, and registers an uninstaller. GitHub Actions publishes that setup executable with each tagged Windows release.
### 3. Inno Setup Windows Installer
The script [`tools/win-native/ci/Package-WinShell-Installer.ps1`](tools/win-native/ci/Package-WinShell-Installer.ps1) and [`tools/win-native/installer/airdour.iss`](tools/win-native/installer/airdour.iss) produce a single-file, 64-bit Windows setup executable (`airdour-v*-setup.exe`) supporting desktop shortcuts and clean uninstallation.

### 4. GitHub Actions CI Matrix
- [`.github/workflows/windows-msvc.yml`](.github/workflows/windows-msvc.yml): Validates MSVC builds on every pull request using `windows-2022`.
- [`.github/workflows/windows-release.yml`](.github/workflows/windows-release.yml): Triggers automated artifact generation and GitHub Release publication on git tags (`v*`).

---

## Technical Constraints & Design Decisions

> [!NOTE]
> **WASAPI.** Shared mode is the default, so other programs can keep playing. Exclusive render is optional and falls back to shared when the device or format refuses it. No ASIO driver.

> [!NOTE]
> **UI.** The shell is Win32 + Direct2D. Cairo is an optional blit for waves, spectrum, and automation (`AIRDOUR_HAVE_CAIRO`), not the product canvas. GTK2 is not the Windows UI.

> [!IMPORTANT]
> **Windows Synchronization Primitives**  
> Under MSVC and Windows SRWLOCK semantics, calling `process_lock().trylock()` from a thread that already holds ownership causes an immediate deadlock (unlike POSIX recursive mutexes). Synchronization in [wasapi_backend.cc](libs/backends/wasapi/wasapi_backend.cc) is specifically structured to eliminate recursive lock acquisition on Windows audio cycles.

---

## Repository Layout

```text
airdour/
├── cmake/
│   └── win64/                    # CMake build spine for MSVC / Windows SDK
│       └── CMakeLists.txt        # Definitions for wasapi_io_smoke and win_shell
├── doc/                          # Technical diagrams, Doxygen specs, and architecture notes
├── headless/                     # Headless command-line Ardour runner (hardour)
├── libs/
│   ├── ardour/                   # Core DAW engine library (libardour)
│   ├── backends/
│   │   ├── dummy/                # Null audio driver for testing
│   │   └── wasapi/               # Native Windows WASAPI (shared default) & WinMME MIDI backend
│   ├── evoral/                   # Event graph and MIDI timeline representation
│   ├── midi++2/                  # MIDI transport and message processing
│   ├── pbd/                      # Platform abstraction, threading, and base data structures
│   └── temporal/                 # Musical tempo maps, timecode, and sample-to-beat conversion
├── tools/
│   └── win-native/               # Windows build scripts, vcpkg manifests, overlays, and CI
│       ├── ci/                   # PowerShell scripts for portable zip and installer generation
│       ├── installer/            # NSIS installer script (airdour.nsi); optional Inno .iss
│       ├── overlays/             # vcpkg port overlays for glibmm-2.4 and sigc++-2.0
│       └── Install-HeadlessDeps.ps1
├── win_ardour/                   # Hardware-accelerated Win32 + Direct2D / DirectWrite GUI
│   ├── engine_host.cc/.h         # Bridge connecting Direct2D UI to libardour Session
│   ├── main.cc                   # Win32 application entry point and smoke test dispatcher
│   ├── mixer_strip.cc/.h         # Direct2D channel strips, faders, meters, and FX slots
│   ├── shell_window.cc/.h        # Top-level window management, DPI scaling, and paint loop
│   ├── transport_bar.cc/.h       # Transport buttons, clock display, and CPU load meter
│   └── wave_strip.cc/.h          # Direct2D waveform rendering and piano-roll lane
├── waf                           # Waf build tool wrapper
└── wscript                       # Root Waf build graph configuration
```

---

## License & Acknowledgments

- **Airdour** is distributed under the terms of the **GNU General Public License Version 2 (GPL-2.0)**. See [COPYING](COPYING) for complete license details.
- Airdour is based on the [Ardour](https://ardour.org/) project created by **Paul Davis** and the international Ardour community.
- Direct2D, DirectWrite, WASAPI, and WinMME integrations developed by the Airdour contributors.
