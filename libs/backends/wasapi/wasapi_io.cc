/*
 * Copyright (C) 2026 Airdour contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "wasapi_io.h"

WasapiIO::WasapiIO ()
	: _open (false)
	, _sample_rate (48000.f)
	, _period_frames (512)
	, _capture_channels (0)
	, _playback_channels (0)
	, _mix_bits (32)
	, _mix_is_float (true)
#ifdef PLATFORM_WINDOWS
	, _com (0)
#endif
{
}

WasapiIO::~WasapiIO ()
{
	close ();
}

int
WasapiIO::enumerate_devices (std::vector<DeviceInfo>& devices)
{
	devices.clear ();

#ifndef PLATFORM_WINDOWS
	return -1;
#else
	/*
	 * Windows agent TODO:
	 *  CoInitializeEx
	 *  CoCreateInstance(MMDeviceEnumerator)
	 *  EnumAudioEndpoints(eRender|eCapture, DEVICE_STATE_ACTIVE)
	 *  GetDefaultAudioEndpoint for default flags
	 *  IPropertyStore → PKEY_Device_FriendlyName
	 *
	 * Shared mode only — do not open exclusive test streams here.
	 */
	return -1; /* not yet implemented on this checkout */
#endif
}

int
WasapiIO::open (const std::string& /*output_device_id*/,
                const std::string& /*input_device_id*/,
                float    sample_rate,
                uint32_t frames_per_period)
{
#ifndef PLATFORM_WINDOWS
	(void)sample_rate;
	(void)frames_per_period;
	return -1;
#else
	/*
	 * Windows agent TODO (shared mode):
	 *  1. Activate IAudioClient on output (and input if duplex)
	 *  2. GetMixFormat → store channel count / float vs PCM
	 *  3. GetDevicePeriod / GetSharedModeEnginePeriod → clamp frames_per_period
	 *  4. Initialize(..., AUDCLNT_SHAREMODE_SHARED, EVENTCALLBACK, ...)
	 *  5. GetService IAudioRenderClient / IAudioCaptureClient
	 *  6. SetEventHandle; Start()
	 *
	 * Do NOT call Initialize with AUDCLNT_SHAREMODE_EXCLUSIVE.
	 */
	_sample_rate = sample_rate;
	_period_frames = frames_per_period;
	_open = false;
	return -1; /* stub: needs Windows SDK build */
#endif
}

void
WasapiIO::close ()
{
#ifdef PLATFORM_WINDOWS
	/* Release clients, free mix WAVEFORMATEX, CoTaskMemFree, CoUninitialize as needed */
#endif
	_open = false;
	_capture_channels = 0;
	_playback_channels = 0;
}

int
WasapiIO::wait_and_transfer (float* /*interleaved_capture*/,
                             float* /*interleaved_playback*/,
                             uint32_t /*max_frames*/)
{
	if (!_open) {
		return -1;
	}
#ifdef PLATFORM_WINDOWS
	/*
	 * Wait on event → GetCurrentPadding → GetBuffer/ReleaseBuffer
	 * Convert device mix format ↔ float32 interleaved for the backend.
	 * Keep convert/mix to one stage for laptop CPU.
	 */
#endif
	return -1;
}
