/*
 * Copyright (C) 2026 Airdour contributors
 *
 * UI-thread engine + minimal Session attach for win_ardour.
 * Peak meters, transport, record, import, undo, loop, markers.
 */

#pragma once

#include <cstdint>
#include <string>

struct EngineMarkerSnap {
	float        sec;
	wchar_t      name[24];
};

struct EngineSnapshot {
	static constexpr int kMeters = 3;
	static constexpr int kMaxMarkers = 8;

	bool         available; /* compiled with AIRDOUR_WIN_SHELL_WITH_ENGINE */
	bool         running;
	bool         session_ok;
	bool         transport_rolling;
	bool         meters_real; /* PeakMeter (not stub) */
	bool         waves_from_session; /* Editor peaks from playlist (may still be empty) */
	bool         session_record_armed; /* Session record enable (may not be rolling yet) */
	bool         session_recording; /* actively capturing */
	bool         session_saved; /* last save_state succeeded */
	bool         export_ok; /* last export produced a file */
	uint32_t     undo_depth;
	uint32_t     redo_depth;
	bool         has_selection; /* Editor region selection */
	int          selection_track; /* 0..kMeters-1, or -1 */
	float        selection_start_sec;
	float        selection_end_sec;
	bool         loop_active; /* auto-loop Location exists with positive length */
	bool         loop_playing; /* Session get_play_loop() */
	float        loop_start_sec;
	float        loop_end_sec;
	int          marker_count;
	EngineMarkerSnap markers[kMaxMarkers];
	std::wstring backend;
	std::wstring session_path; /* current session folder */
	std::wstring export_path; /* last exported WAV (UTF-16 display) */
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

	/** Persist current Session state (regions, routes) to disk. */
	bool save_session ();

	/**
	 * Close current Session (leak until process exit) and open an existing
	 * session folder. snapshot_name defaults to "win_shell".
	 */
	bool open_session (const std::string& session_dir, const std::string& snapshot_name = "win_shell");

	/** Save then reopen the current session path (smoke / Ctrl+S round-trip). */
	bool reload_session ();

	/**
	 * Bounce session (master bus) to a WAV file for [start,end] samples.
	 * If outfile empty, writes under session export/ as airdour_export.wav.
	 * If end<=start, uses playlist extent or a short fallback window.
	 */
	bool export_session_wav (const std::string& outfile = std::string (),
	                         int64_t start_sample = 0,
	                         int64_t end_sample = 0);

	/** Convenience: export after ensuring content; for --smoke-export. */
	bool smoke_export_wav (std::string* out_path = nullptr);

	/** Session history undo/redo (Ctrl+Z / Ctrl+Y). */
	bool undo (uint32_t n = 1);
	bool redo (uint32_t n = 1);

	/**
	 * Hit-test playlist at time_sec on track; select topmost region if any.
	 * Updates snapshot selection fields for Editor highlight.
	 */
	bool select_region_at (int track, float time_sec);
	void clear_region_selection ();

	/** Remove selected region via reversible command; peaks refresh on next poll. */
	bool delete_selected_region ();

	/**
	 * Import a test tone, undo it (regions gone), redo (regions back), undo again.
	 * For --smoke-undo. Returns false if history/import path fails.
	 */
	bool smoke_undo ();

	/** Create/update Session auto-loop Location [start_sec, end_sec]. */
	bool set_loop_range (float start_sec, float end_sec);
	/** Set loop start or end (keeps the other; creates a 1s default span if needed). */
	bool set_loop_start (float start_sec);
	bool set_loop_end (float end_sec);
	bool set_loop_play (bool enable);
	bool toggle_loop_play ();

	/** Add IsMark at time_sec (name auto if empty). */
	bool add_marker_at (float time_sec, const std::string& name = std::string ());
	/** Remove nearest IsMark within slop_sec of time_sec. */
	bool remove_marker_near (float time_sec, float slop_sec = 0.35f);
	/** Locate transport to time_sec (optional snap to nearest mark within slop). */
	bool locate_to (float time_sec, bool snap_to_marker = false, float slop_sec = 0.35f);

	/**
	 * Set loop 0.5–2.5s, add two marks, remove one; verify snapshot.
	 * Does not enable loop-play (avoids forced roll during --timeout).
	 */
	bool smoke_loop_markers ();

	std::string session_dir () const;
	std::string last_export_path () const;

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
	void refresh_history_flags ();
	void refresh_selection_snap ();
	void refresh_locations_snap ();
	void collect_tracks ();
	void update_session_path_snap ();
	bool compute_export_range (int64_t* start_sample, int64_t* end_sample) const;

	bool        _started;
	std::string _backend_name;
	std::string _last_export_path;
	float       _gain;
	float       _track_gain[EngineSnapshot::kMeters];
	EngineSnapshot _snap;

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE
	class Impl;
	Impl* _impl;
#endif
};
