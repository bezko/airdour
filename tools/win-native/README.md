# Airdour Windows-native build (MSVC)

This Linux cloud checkout **cannot** produce a running Windows binary. Use a
Windows 10/11 x64 machine (or cloud Windows agent) with the steps below.

## Locked audio target

- **WASAPI shared mode only** — tune period/buffer, mix format, convert/mix path
- **Not** WASAPI exclusive (v1)
- **Not** DirectSound
- **Not** ASIO / specialized drivers

Backend sources: `libs/backends/wasapi/`

## Prerequisites

1. Visual Studio 2022 (Desktop C++, MSVC v143) + Windows 10/11 SDK (x64)
2. Python 3.x (for waf) **or** CMake ≥ 3.20 for the spine under `cmake/win64/`
3. Dependency prefix built for MSVC (glib/glibmm, libxml2, libsndfile, sigc++,
   libcurl, libarchive, rubberband, vamp, GTK2 stack for GUI later).
   PortAudio is **optional** (`--with-portaudio-bridge`) and should be removed
   once WASAPI shared I/O works.
4. **No** Steinberg ASIO SDK

## waf (preferred short-term — already knows `--dist-target=msvc`)

```bat
set PKG_CONFIG_PATH=C:\path\to\deps\lib\pkgconfig
python waf configure --dist-target=msvc --windows-only --prefix=C:\airdour-build
python waf build -j %NUMBER_OF_PROCESSORS%
```

Default backends with `--windows-only` / `msvc`: **dummy** + **wasapi**.

First milestone: link `hardour` (headless) with **dummy**, then load **wasapi**
and open the default shared-mode endpoint.

## CMake spine

```bat
cmake -S cmake\win64 -B build-win64 -G "Visual Studio 17 2022" -A x64 ^
  -DAIRDOUR_DEPS_ROOT=C:\path\to\deps
cmake --build build-win64
```

Today this only documents the slice graph; wire real `add_library` targets as
each lib links cleanly.

## WASAPI shared bring-up checklist

1. Enumerate endpoints (`IMMDeviceEnumerator`)
2. `GetMixFormat` + `Initialize(..., AUDCLNT_SHAREMODE_SHARED, EVENTCALLBACK, …)`
3. Align Ardour period with `GetDevicePeriod` / `GetSharedModeEnginePeriod`
4. Event thread + MMCSS (`libs/pbd/windows_mmcss.cc`)
5. Single-stage float convert/mix; measure glitch rate vs CPU on a cheap laptop
6. Port WinMME MIDI from `libs/backends/portaudio/winmmemidi_*`
7. Delete PortAudio from the product path

## Success for this phase

- MSVC build of libpbd → libardour → hardour
- Dummy backend processes a session headless
- WASAPI shared device opens and exchanges audio (even before GUI)
