# WASAPI backend (Airdour)

Native Windows audio backend for Airdour. Implements `ARDOUR::AudioBackend`.

## Scope (v1)

- **WASAPI shared mode only** (`AUDCLNT_SHAREMODE_SHARED`)
- Optimize for cheap laptops / stock Windows drivers:
  - buffer & period sizing from the shared engine quantum
  - mix-format negotiation (`IAudioClient::GetMixFormat`)
  - tight float convert/mix path (minimize copies)
  - event-driven callback + MMCSS
- **WinMME MIDI** (port from `libs/backends/portaudio/winmmemidi_*`)

## Out of scope (v1)

- WASAPI exclusive mode (optional later only if asked)
- DirectSound
- ASIO / specialized drivers
- PortAudio (temporary bridge elsewhere until this backend runs)

## First implementation order

1. Device enumeration (`IMMDeviceEnumerator`)
2. Shared-mode `IAudioClient` open with mix format
3. Event callback thread → Ardour `process_callback`
4. Period/buffer mapping exposed to the engine dialog
5. Capture + render duplex; measure latency/CPU
6. Attach WinMME MIDI
7. Delete PortAudio from the Windows product path

## Build

Included when `AIRDOUR_WINDOWS_ONLY` / `--dist-target=msvc|mingw` and `wasapi` is in `BACKENDS`.
Requires Windows SDK (`mmdeviceapi.h`, `audioclient.h`). Links: `ole32`, `winmm`.
