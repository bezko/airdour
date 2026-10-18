# Airdour Windows-native build (MSVC)

This Linux cloud checkout **cannot** produce a running Windows binary. Use a
Windows 10/11 x64 machine (or cloud Windows agent) with the steps below.

## Locked audio target

- **WASAPI shared mode only** — implemented in `libs/backends/wasapi/`
- **Not** WASAPI exclusive (v1)
- **Not** DirectSound
- **Not** ASIO / specialized drivers
- PortAudio is **not** on the default Windows-only product path

## Prerequisites

1. Visual Studio 2022 (Desktop C++, MSVC v143) + Windows 10/11 SDK (x64)
2. Python 3.x (for waf) **or** CMake ≥ 3.20 (`cmake/win64/`)
3. Dependency prefix built for MSVC (glib/glibmm, libxml2, libsndfile, sigc++,
   libcurl, libarchive, rubberband, vamp; GTK2 stack for GUI later)
4. **No** Steinberg ASIO SDK

## waf

```bat
set PKG_CONFIG_PATH=C:\path\to\deps\lib\pkgconfig
python waf configure --dist-target=msvc --windows-only --prefix=C:\airdour-build
python waf build -j %NUMBER_OF_PROCESSORS%
```

Default backends: **dummy** + **wasapi**.

## Bring-up checklist (WASAPI shared — code is in-tree)

1. [ ] MSVC link `libpbd` → `libardour` → `hardour`
2. [ ] Dummy backend processes a session headless
3. [ ] Load `wasapi_audiobackend` DLL; enumerate devices
4. [ ] Open Default shared-mode render (+ capture); confirm mix format / period snap
5. [ ] Play/record a tone; measure glitch rate vs period (256/512/1024) on a cheap laptop
6. [ ] MMCSS + timer resolution active (check with debugger / tracing)
7. [ ] Port WinMME MIDI into this backend; drop any PortAudio bridge

## Success for this phase

- Shared-mode device opens and exchanges audio through `ARDOUR::AudioBackend`
- No exclusive / DirectSound / ASIO code required
