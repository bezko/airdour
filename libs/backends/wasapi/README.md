# WASAPI backend (Airdour)

Native Windows audio backend implementing `ARDOUR::AudioBackend`.

## Scope (v1) — implemented in-tree

- **WASAPI shared mode only** (`AUDCLNT_SHAREMODE_SHARED`)
- Device enumeration (`IMMDeviceEnumerator`, friendly names + ids)
- Mix-format negotiation (`GetMixFormat`); adopt device sample rate
- Period snap to shared engine quantum (`GetDevicePeriod` / `IAudioClient3::GetSharedModeEnginePeriod`)
- Event-driven cycle: `wait_for_period` → `read_capture` → engine process → `write_playback`
- Float convert/mix for IEEE float32, PCM16, PCM24, PCM32
- MMCSS “Pro Audio” on the main process thread (`PBD::MMCSS`)
- Multimedia timer resolution (`PBD::MMTIMERS`)
- System capture/playback ports registered into `PortEngineSharedImpl`

## Out of scope (v1)

- WASAPI exclusive mode
- DirectSound
- ASIO / specialized drivers
- WinMME MIDI (reserved option string; port from `libs/backends/portaudio/winmmemidi_*` next)
- Capture when capture mix rate ≠ render mix rate (skipped with honest fallback)

## Non-Windows / CI

All COM paths are behind `#ifdef _WIN32`. On Linux the module can be parsed; `available()` returns false and `_start()` refuses to run.

## Build

Included when `AIRDOUR_WINDOWS_ONLY` / `--dist-target=msvc|mingw` (backends: `dummy` + `wasapi`).
Links: `ole32`, `winmm`, `uuid`, `avrt`.
