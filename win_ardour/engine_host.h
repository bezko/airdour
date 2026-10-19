/*
 * Copyright (C) 2026 Airdour contributors
 *
 * UI-thread engine + minimal Session attach for win_ardour.
 * Peak meters, transport, record, import, undo, loop, markers, MIDI.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct EngineMarkerSnap {
	float        sec;
	wchar_t      name[24];
};

struct EngineMidiDeviceSnap {
	wchar_t name[64];
	bool    input; /* true = capture / in; false = playback / out (best-effort) */
};

struct EngineMidiNoteSnap {
	int32_t id;
	float   start_sec;
	float   length_sec;
	uint8_t pitch;
	uint8_t velocity;
	bool    selected;
};

struct EngineSnapshot {
	static constexpr int kMeters = 3;
	static constexpr int kMaxMarkers = 8;
	static constexpr int kMaxMidiDevices = 16;
	static constexpr int kMaxMidiNotes = 96;

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
	float        selection_fade_in_sec; /* AudioRegion fade-in length */
	float        selection_fade_out_sec;
	bool         loop_active; /* auto-loop Location exists with positive length */
	bool         loop_playing; /* Session get_play_loop() */
	float        loop_start_sec;
	float        loop_end_sec;
	int          marker_count;
	EngineMarkerSnap markers[kMaxMarkers];
	bool         midi_ok; /* backend MIDI option active / ports present */
	bool         midi_track_ok; /* Session has at least one MidiTrack */
	bool         midi_region_ok; /* playlist has a MidiRegion for editing */
	bool         midi_synth_ok; /* in-graph soft synth on MIDI track */
	bool         midi_synth_to_master; /* synth audio routed toward master */
	bool         midi_out_connected; /* track output → physical MIDI (e.g. GS) */
	bool         track_fx_ok; /* ACE effect inserted on Audio 1 (track 0) */
	bool         track_fx_bypassed; /* Processor hard-bypass (inactive) */
	float        track_fx_param; /* ACE Reverb Blend 0..1 */
	float        midi_activity; /* 0..1 decaying activity indicator */
	int          midi_device_count;
	EngineMidiDeviceSnap midi_devices[kMaxMidiDevices];
	int          midi_note_count;
	EngineMidiNoteSnap midi_notes[kMaxMidiNotes];
	int32_t      midi_selected_note_id; /* Evoral id, or -1 */
	std::wstring midi_option; /* e.g. WinMME / Dummy loopback */
	std::wstring midi_synth_name; /* e.g. ACE Reasonable Synth */
	std::wstring midi_out_name; /* connected playback port display name */
	std::wstring track_fx_name; /* e.g. ACE Reverb on track 0 */
	std::wstring track_fx_param_name; /* e.g. Blend */
	std::wstring backend;
	std::wstring session_path; /* current session folder */
	std::wstring export_path; /* last exported WAV (UTF-16 display) */
	uint32_t     sample_rate;
	uint32_t     period_frames;
	float        dsp_load_pct; /* 0..100 from AudioEngine */
	uint64_t     callback_last_us;
	uint64_t     callback_max_us;
	uint64_t     callback_avg_us;
	uint64_t     callback_count;
	uint64_t     xrun_count; /* Session + backend underruns */
	float        gain; /* track 0 stub/session gain */
	float        meter_peak[kMeters]; /* linear 0..1 */
	bool         meter_clip[kMeters]; /* ≥0 dBFS / PeakMeter MeterMaxPeak */
	bool         track_has_regions[kMeters];
	bool         track_armed[kMeters];
	bool         track_muted[kMeters]; /* Route::muted() — includes solo-mute */
	bool         track_soloed[kMeters]; /* Route::soloed() / self_soloed */
	float        track_pan[kMeters]; /* pan azimuth 0=L … 0.5=C … 1=R */
	float        track_send[kMeters]; /* aux send gain coeff 0..1 → Aux bus */
	bool         track_send_ok[kMeters]; /* InternalSend present on track */
	bool         aux_bus_ok; /* Session Aux audio bus exists */
	bool         midi_muted;
	bool         midi_soloed;
	bool         snap_enabled; /* Editor snap-to-grid */
	int          snap_density; /* 0=bar 1=beat 2=1/2 3=1/4 */
	int          grid_line_count; /* visible grid lines for Editor draw */
	static constexpr int kMaxGridLines = 96;
	float        grid_line_sec[kMaxGridLines];
	bool         grid_line_bar[kMaxGridLines]; /* thicker bar lines */
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

	/** Request WASAPI/Dummy period frames before start (0 = backend Auto). */
	void set_period_frames (uint32_t frames);

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
	bool  set_midi_gain (float linear);
	float gain () const;

	/** Arm / disarm a track for recording (Session rec-enable control). */
	bool set_track_armed (int track, bool armed);
	bool toggle_track_armed (int track);

	/** Route mute/solo (audio tracks 0..kMeters-1). Controllable::NoGroup. */
	bool set_track_muted (int track, bool muted);
	bool toggle_track_muted (int track);
	bool set_track_soloed (int track, bool soloed);
	bool toggle_track_soloed (int track);

	/** MIDI track mute/solo (Session MidiTrack as Route). */
	bool set_midi_muted (bool muted);
	bool toggle_midi_muted ();
	bool set_midi_soloed (bool soloed);
	bool toggle_midi_soloed ();

	/**
	 * Mute/solo Audio 1 + MIDI round-trip; clear. For --smoke-mute-solo.
	 */
	bool smoke_mute_solo ();

	/** Pan azimuth 0=L … 0.5=C … 1=R via Route::pan_azimuth_control. */
	bool  set_track_pan (int track, float pan_0_1);
	float track_pan (int track) const;

	/** Pan L → C → R on Audio 1. For --smoke-pan. */
	bool smoke_pan ();

	/** Aux send level 0..1 (gain coeff) via InternalSend → Aux bus. */
	bool  set_track_send (int track, float send_0_1);
	float track_send (int track) const;

	/** Send 0 → 0.75 → 0.25 on Audio 1. For --smoke-send. */
	bool smoke_send ();

	/**
	 * Track fader AutomationList (GainAutomation). Enable Play, write points,
	 * sample curve for Editor automation lane.
	 */
	bool ensure_track_gain_automation (int track = 0);
	bool write_track_gain_automation (int track, float time_sec, float value_0_1);
	bool clear_track_gain_automation (int track = 0);
	/** Fill y_0_1[n] by evaluating the list across [view_start, view_start+span]. */
	bool fill_gain_automation_curve (int track,
	                                 float view_start_sec,
	                                 float view_span_sec,
	                                 float* y_0_1,
	                                 size_t n);
	/** Discrete points in view; returns count written (≤ max_n). */
	int  fill_gain_automation_points (int track,
	                                  float view_start_sec,
	                                  float view_span_sec,
	                                  float* x_norm,
	                                  float* y_0_1,
	                                  int max_n);
	/** Clear → 3 points → Play. For --smoke-automation. */
	bool smoke_automation ();

	/** Clear PeakMeter max-peak / clip latch on Audio track (0..kMeters-1). */
	bool clear_track_meter_clip (int track);

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

	/** Split playlist region(s) at time_sec on track (−1 = selection track or 0). */
	bool split_at (float time_sec, int track = -1);
	/** Split at current transport playhead. */
	bool split_at_playhead (int track = -1);

	/** Set fade-in/out length (seconds) on selected AudioRegion; activates fades. */
	bool set_selected_fade_in_sec (float sec);
	bool set_selected_fade_out_sec (float sec);
	/**
	 * Apply ~80 ms fades on selection; if an adjacent region exists, set
	 * complementary fade so the abut/overlap reads as a crossfade.
	 */
	bool apply_selected_crossfade (float length_sec = 0.08f);

	/**
	 * Trim selected region front/end to absolute time (seconds).
	 * Uses Region::trim_front / trim_end; peaks refresh via refresh_region_flags.
	 */
	bool trim_selected_front_to (float time_sec);
	bool trim_selected_end_to (float time_sec);
	/** Move selected region so its start is at time_sec (Region::set_position). */
	bool move_selected_to (float time_sec);

	/** Snap-to-grid: TempoMap beat/bar (fallback fixed 120 BPM). */
	void set_snap_enabled (bool on);
	bool snap_enabled () const;
	void toggle_snap ();
	/** Cycle density: bar → beat → 1/2 → 1/4 → bar. */
	void cycle_snap_density ();
	void set_snap_density (int density_0_to_3);
	int  snap_density () const;
	/** Snap time_sec to current grid when enabled; identity when off. */
	float snap_time (float time_sec) const;
	/** Fill snapshot grid_line_* for Editor draw over [view_start, view_start+span]. */
	void refresh_grid_lines (float view_start_sec, float view_span_sec);

	/** Count audio regions on a track playlist (smoke / diagnostics). */
	int track_region_count (int track) const;

	/**
	 * Import tone → split → crossfade → trim → move; verify lengths/position.
	 * For --smoke-edit.
	 */
	bool smoke_edit ();

	/**
	 * Enable beat snap; verify 1.1s → ~1.0s; density bar; off → identity.
	 * For --smoke-snap.
	 */
	bool smoke_snap ();

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

	/** Ensure a Session MidiTrack exists (creates one if needed). */
	bool ensure_midi_track ();

	/**
	 * Attach ACE Reasonable Synth (bundled LV2) to MIDI 1 and route audio
	 * toward the master bus so notes are audible via Dummy/WASAPI shared.
	 */
	bool attach_midi_synth ();

	/**
	 * Insert ACE Reverb (bundled LV2, no GTK UI) on an audio track pre-fader.
	 * Default track 0 = Audio 1. Idempotent when already present.
	 */
	bool insert_track_fx (int track = 0);

	/** Toggle Processor activate/deactivate (hard bypass) on Audio FX insert. */
	bool toggle_track_fx_bypass (int track = 0);
	bool set_track_fx_bypassed (int track, bool bypassed);

	/**
	 * Set ACE Reverb Blend (wet mix) 0..1 via PluginInsert AutomationControl.
	 * UI-thread safe Controllable::set_value.
	 */
	bool set_track_fx_param (int track, float value_0_1);
	float track_fx_param (int track = 0) const;

	/**
	 * Ensure ACE Reverb on Audio 1; verify on route; toggle bypass; set Blend.
	 * For --smoke-plugin. Hardware not required.
	 */
	bool smoke_plugin_insert ();

	/**
	 * Scan system VST3 dirs (+ VST3_PATH / AIRDOUR_SMOKE_VST3); list; optionally
	 * insert first usable audio FX on Audio 2. DSP-only (no GTK UI).
	 * Scan-only with VST3=0 still returns true (proves path). For --smoke-vst3.
	 */
	bool smoke_vst3 ();

	/**
	 * Sample WASAPI shared timing + working set while transport rolls briefly.
	 * Prints a one-line PERF summary to stderr. For --smoke-perf.
	 */
	bool smoke_perf (unsigned sample_secs = 4);

	/**
	 * Ensure an empty MidiRegion on the MIDI track playlist (creates if needed).
	 * length_sec is the region span starting at 0.
	 */
	bool ensure_midi_region (float length_sec = 32.f);

	/**
	 * Add a note into the Session MidiModel (persisted with the region).
	 * Returns false if track/region/model unavailable or overlap policy rejects.
	 */
	bool add_midi_note (uint8_t pitch, float start_sec, float length_sec,
	                    uint8_t velocity = 100);

	/** Hit-test piano-roll; selects note under (time,pitch) if any. */
	bool select_midi_note_at (float time_sec, uint8_t pitch, float pitch_slop = 0.f);
	void clear_midi_note_selection ();
	bool delete_selected_midi_note ();

	/** Best-effort connect MidiTrack output to first physical MIDI playback port. */
	bool connect_midi_output ();

	/** Refresh MIDI device list + option + notes into snapshot (safe without hardware). */
	void refresh_midi_snap ();

	/**
	 * Enumerate MIDI devices, ensure MidiTrack, verify midi_ok/midi_track_ok.
	 * Hardware I/O not required. Loopback on Dummy when available.
	 */
	bool smoke_midi ();

	/**
	 * Ensure region, add notes, select+delete one, verify remaining notes in model.
	 * Also attaches Reasonable Synth and checks master routing when available.
	 */
	bool smoke_midi_edit ();

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
	void refresh_mute_solo_flags ();
	void refresh_pan_flags ();
	void refresh_send_flags ();
	bool ensure_aux_sends ();
	void refresh_history_flags ();
	void refresh_selection_snap ();
	void refresh_locations_snap ();
	void collect_tracks ();
	void update_session_path_snap ();
	bool compute_export_range (int64_t* start_sample, int64_t* end_sample) const;
	bool configure_backend_midi ();
	void refresh_midi_notes_snap ();
	void refresh_track_fx_snap ();
	bool ensure_plugins_scanned ();

	bool        _started;
	std::string _backend_name;
	std::string _last_export_path;
	uint32_t    _period_frames; /* 0 = Auto */
	float       _gain;
	float       _track_gain[EngineSnapshot::kMeters];
	EngineSnapshot _snap;

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE
	class Impl;
	Impl* _impl;
#endif
};
