/*
 * Copyright (C) 2026 Airdour contributors
 *
 * UI-thread engine + minimal Session attach for win_ardour.
 * Peak meters, transport, record arm, and WAV import for Editor waves.
 */

#pragma once

#include <cstdint>
#include <string>

struct EngineSnapshot {
	static constexpr int kMeters = 3;

	bool         available; /* compiled with AIRDOUR_WIN_SHELL_WITH_ENGINE */
	bool         running;
	bool         session_ok;
	bool         transport_rolling;
	bool         meters_real; /* PeakMeter (not stub) */
	bool         waves_from_session; /* Editor peaks from playlist (may still be empty) */
	bool         session_record_armed; /* Session record enable (may not be rolling yet) */
	bool         session_recording; /* actively capturing */
	std::wstring backend;
	uint32_t     sample_rate;
	uint32_t     period_frames;
	float        gain; /* track 0 stub/session gain */
	float        meter_peak[kMeters]; /* linear 0..1 */
	bool         track_has_regions[kMeters];
	bool         track_armed[kMeters];
	float        transport_sec;
	std::wstring detail;
};

class EngineHost
{
public:
	EngineHost ();
	~EngineHost ();

	EngineHost (const EngineHost&) = delete;
	EngineHost& operator= (const EngineHost&) = delete;

	void prepare (const std::string& backend_name);

	/** Start backend + scaffold minimal Session (tracks + demo tones). */
	bool start ();
	bool start (const std::string& backend_name);

	/** Tear down Session + stop backend I/O (UI thread). */
	void stop ();

	/** Stop backend I/O only (no Session/Graph join). For timeout smoke. */
	void stop_backend_only ();

	/** Session transport roll / stop (keeps engine running). */
	void transport_play ();
	void transport_stop ();

	void poll ();

	void  set_gain (float linear);
	void  set_track_gain (int track, float linear);
	float gain () const;

	/** Arm / disarm a track for recording (Session rec-enable control). */
	bool set_track_armed (int track, bool armed);
	bool toggle_track_armed (int track);

	/** Session-level record enable (maybe_enable_record / disable_record). */
	bool set_session_record (bool enable);
	bool toggle_session_record ();

	/**
	 * Import a WAV (or other libsndfile) path onto a track playlist at time 0.
	 * Builds peakfile so Editor waveforms appear without DEMO_TONES.
	 */
	bool import_audio_file (int track, const std::string& path);

	/**
	 * Write a short PCM WAV under the session dir and import it (safe path for
	 * smoke / no file dialog). Returns false if Session is missing.
	 */
	bool import_test_tone (int track, float freq_hz = 220.f, float seconds = 2.f);

	EngineSnapshot snapshot () const;

	/**
	 * Fill peak/RMS columns for one track from Session playlist regions.
	 * Returns true if the track has at least one audio region (even if outside view).
	 * When false or on failure, peaks/rms are zeroed (empty lane).
	 */
	bool fill_track_waveform (int track,
	                          float view_start_sec,
	                          float view_span_sec,
	                          float* peaks,
	                          float* rms,
	                          size_t n_columns);

	static bool is_linked ();

private:
	void refresh_snapshot ();
	bool ensure_session ();
	void destroy_session ();
	void read_meters ();
	void refresh_region_flags ();
	void refresh_record_flags ();

	bool        _started;
	std::string _backend_name;
	float       _gain;
	float       _track_gain[EngineSnapshot::kMeters];
	EngineSnapshot _snap;

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE
	class Impl;
	Impl* _impl;
#endif
};
