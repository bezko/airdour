# WASAPI backend (Airdour)

Native Windows audio backend implementing `ARDOUR::AudioBackend`.

## Scope (v1) — implemented in-tree

- **WASAPI shared mode only** (`AUDCLNT_SHAREMODE_SHARED`)
- Device enumeration (`IMMDeviceEnumerator`, friendly names + ids)
- Mix-format negotiation (`GetMixFormat`); adopt device sample rate
- Period via `IAudioClient3::InitializeSharedAudioStream` when available (else classic `Initialize`)
- Auto ladder tries low shared periods (128 / 240 / 256 / …) then falls back toward ~10 ms
- Snaps to `GetSharedModeEnginePeriod` fundamental; respects per-device min/max
- Event-driven cycle: `wait_for_period` → `read_capture` → engine process → `write_playback`
- Float convert/mix for IEEE float32, PCM16, PCM24, PCM32
- MMCSS “Pro Audio” **HIGH** on the main process thread (`PBD::MMCSS`)
- Default **Auto** (0); on many laptops the shared engine is locked to **480** / 10 ms
- Shell override: `--period N` → `set_buffer_size` before start
- `--smoke-perf` reports callback µs vs period
- DSP timing starts **after** `wait_for_period` (Release ~400 µs / 10 ms @ Session+ACE)
- Multimedia timer resolution (`PBD::MMTIMERS`)
- System capture/playback ports registered into `PortEngineSharedImpl`
- **WinMME MIDI** (`../portaudio/winmmemidi_*`) — option `WinMME`

## Out of scope (v1)

- WASAPI exclusive mode
- DirectSound
- ASIO / specialized drivers
- PortAudio MIDI bridge
- Capture when capture mix rate ≠ render mix rate (skipped with honest fallback)

## Non-Windows / CI

All COM paths are behind `#ifdef _WIN32`. On Linux the module can be parsed; `available()` returns false and `_start()` refuses to run.

## Build

Included when `AIRDOUR_WINDOWS_ONLY` / `--dist-target=msvc|mingw` (backends: `dummy` + `wasapi`).
Links: `ole32`, `winmm`, `uuid`, `avrt`.
