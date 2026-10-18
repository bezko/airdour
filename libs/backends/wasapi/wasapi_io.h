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
 * WASAPI shared-mode I/O (AUDCLNT_SHAREMODE_SHARED only).
 *
 * Real COM lives behind _WIN32. Non-Windows builds keep honest stubs so
 * headers parse and the backend module can still be syntax-checked.
 */
class WasapiIO
{
public:
	struct DeviceInfo {
		std::string id;   /* IMMDevice id (empty = default sentinel on stubs) */
		std::string name; /* friendly name shown in UI */
		bool        is_input;
		bool        is_output;
		bool        is_default_input;
		bool        is_default_output;
		bool        available;
	};

	WasapiIO ();
	~WasapiIO ();

	int enumerate_devices (std::vector<DeviceInfo>& devices);

	/** Resolve UI name or raw id to an IMMDevice id; empty string = system default. */
	std::string resolve_device_id (const std::string& name_or_id, bool want_input) const;

	/**
	 * Open shared-mode render (+ optional capture).
	 * Adopts the device mix format sample rate (shared-mode requirement).
	 * Snaps frames_per_period to the shared engine quantum for laptop stability.
	 */
	int open (const std::string& output_name_or_id,
	          const std::string& input_name_or_id,
	          float             sample_rate_hint,
	          uint32_t          frames_per_period);

	void close ();

	bool     is_open () const { return _open; }
	float    sample_rate () const { return _sample_rate; }
	uint32_t period_frames () const { return _period_frames; }
	uint32_t capture_channels () const { return _capture_channels; }
	uint32_t playback_channels () const { return _playback_channels; }
	uint32_t mix_bits () const { return _mix_bits; }
	bool     mix_is_float () const { return _mix_is_float; }
	uint32_t default_period_frames () const { return _default_period_frames; }
	uint32_t min_period_frames () const { return _min_period_frames; }

	/** Laptop-oriented period list derived from the last open/enum quantum. */
	void recommended_buffer_sizes (std::vector<uint32_t>& sizes) const;

	/**
	 * Event-driven shared-mode cycle (call in order each period):
	 *   wait_for_period() → read_capture() → (engine process) → write_playback()
	 * Buffers are float32 interleaved.
	 * @return 1 on event, 0 on timeout, <0 on error
	 */
	int wait_for_period ();
	/** @return frames written into capture buffer, or <0 on error */
	int read_capture (float* interleaved_capture, uint32_t max_frames);
	/**
	 * Push one period of interleaved playback.
	 * @return frames written, -2 if short write (underrun-ish), <0 on hard error
	 */
	int write_playback (const float* interleaved_playback, uint32_t max_frames);

private:
	bool     _open;
	float    _sample_rate;
	uint32_t _period_frames;
	uint32_t _default_period_frames;
	uint32_t _min_period_frames;
	uint32_t _capture_channels;
	uint32_t _playback_channels;
	uint32_t _mix_bits;
	bool     _mix_is_float;

	mutable std::vector<DeviceInfo> _cached_devices;

#ifdef _WIN32
	struct ComState;
	ComState* _com;
#endif
};
