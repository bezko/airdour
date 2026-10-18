/*
 * Copyright (C) 2026 Airdour contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * Thin WASAPI shared-mode I/O layer.
 *
 * v1: AUDCLNT_SHAREMODE_SHARED only. Exclusive / DirectSound / ASIO are
 * intentionally absent.
 *
 * Real COM objects are compiled only under PLATFORM_WINDOWS; elsewhere this
 * is a no-op stub so Linux inventory builds can parse headers.
 */
class WasapiIO
{
public:
	struct DeviceInfo {
		std::string id;   /* IMMDevice id */
		std::string name; /* friendly name */
		bool        is_default_input;
		bool        is_default_output;
		bool        available;
	};

	WasapiIO ();
	~WasapiIO ();

	/** Enumerate active endpoints (eRender / eCapture). */
	int enumerate_devices (std::vector<DeviceInfo>& devices);

	/**
	 * Open shared-mode duplex (or render-only / capture-only).
	 * Uses GetMixFormat + Initialize(AUDCLNT_SHAREMODE_SHARED).
	 * @param frames_per_period preferred period; may be adjusted to engine quantum
	 */
	int open (const std::string& output_device_id,
	          const std::string& input_device_id,
	          float             sample_rate,
	          uint32_t          frames_per_period);

	void close ();

	bool     is_open () const { return _open; }
	float    sample_rate () const { return _sample_rate; }
	uint32_t period_frames () const { return _period_frames; }
	uint32_t capture_channels () const { return _capture_channels; }
	uint32_t playback_channels () const { return _playback_channels; }

	/** Device mix format bits (float32 expected on modern Windows). */
	uint32_t mix_bits () const { return _mix_bits; }
	bool     mix_is_float () const { return _mix_is_float; }

	/**
	 * Blocking wait for next shared-mode packet (event callback path).
	 * Fills interleaved capture and consumes interleaved playback.
	 * Returns frames processed, or <0 on error.
	 */
	int wait_and_transfer (float* interleaved_capture,
	                       float* interleaved_playback,
	                       uint32_t max_frames);

private:
	bool     _open;
	float    _sample_rate;
	uint32_t _period_frames;
	uint32_t _capture_channels;
	uint32_t _playback_channels;
	uint32_t _mix_bits;
	bool     _mix_is_float;

#ifdef PLATFORM_WINDOWS
	/* Opaque COM holders — defined in wasapi_io.cc */
	struct ComState;
	ComState* _com;
#endif
};
