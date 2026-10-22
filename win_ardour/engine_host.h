/*
 * Copyright (C) 2026 Airdour contributors
 *
 * UI-thread engine + minimal Session attach for win_ardour.
 * Peak meters, transport, record, import, undo, loop, markers, MIDI.
 */

#pragma once

#include <cstdint>
#include <string>
#include <chrono>
#include <vector>

struct EngineMarkerSnap {
	float        sec;
	wchar_t      name[24];
	bool         locked; /* Location::locked — blocks move */
	bool         cd; /* Location::is_cd_marker */
	bool         scene;
	int32_t      program;
	int32_t      bank;
	int32_t      channel; /* MIDISceneChange channel 0..15, or -1 */
	bool         cue; /* Location::is_cue_marker */
	int32_t      cue_id; /* TriggerBox cue slot when cue */
	bool         section; /* Location::is_section */
};

struct EngineRangeSnap {
	float        start_sec;
	float        end_sec;
	wchar_t      name[24];
};

struct EngineSkipSnap {
	float        start_sec;
	float        end_sec;
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
	static constexpr int kMeters = 6; /* max audio tracks (Editor lanes / meters) */
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
	static constexpr int kMaxHistory = 8;
	static constexpr int kHistoryNameLen = 40;
	int          undo_list_count; /* recent undo names, newest first */
	wchar_t      undo_list_name[kMaxHistory][kHistoryNameLen];
	wchar_t      next_undo_name[kHistoryNameLen];
	wchar_t      next_redo_name[kHistoryNameLen];
	bool         has_selection; /* Editor region selection */
	int          selection_track; /* 0..kMeters-1, or -1 */
	float        selection_start_sec;
	float        selection_end_sec;
	float        selection_fade_in_sec; /* AudioRegion fade-in length */
	float        selection_fade_out_sec;
	bool         selection_sync_marked; /* Region::sync_marked */
	float        selection_sync_sec; /* absolute session time of sync point */
	bool         selection_muted; /* Region::muted() */
	float        selection_gain_db; /* AudioRegion scale_amplitude as dB */
	bool         selection_locked; /* Region::locked() — blocks move/trim */
	bool         selection_position_locked; /* Region::position_locked — blocks move */
	uint32_t     selection_layer; /* Playlist layer (higher = on top) */
	bool         selection_opaque; /* Region::opaque — false = transparent layering */
	static constexpr int kRegionNameLen = 32;
	wchar_t      selection_region_name[kRegionNameLen]; /* selected Region::name */
	int          selection_fade_shape;
	bool         selection_fade_in_active;
	bool         selection_fade_out_active;
	bool         region_envelope_ok; /* selected AudioRegion envelope_active */
	int          region_envelope_points; /* AutomationList size on selection */
	bool         loop_active; /* auto-loop Location exists with positive length */
	bool         loop_playing; /* Session get_play_loop() */
	float        loop_start_sec;
	float        loop_end_sec;
	bool         punch_range_ok; /* auto-punch Location exists with positive length */
	bool         punch_enabled; /* Session punch-in and/or punch-out armed */
	float        punch_start_sec;
	float        punch_end_sec;
	static constexpr int kMaxRanges = 8;
	int          range_count; /* IsRangeMarker locations */
	EngineRangeSnap ranges[kMaxRanges];
	static constexpr int kMaxSkips = 8;
	int          skip_count; /* IsSkip range locations (transport skip) */
	EngineSkipSnap skips[kMaxSkips];
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
	bool         track_comp_ok; /* ACE Compressor on Audio 2 (track 1) */
	bool         track_comp_bypassed;
	float        track_comp_threshold_db; /* −60..0 dBFS */
	float        track_comp_ratio; /* 1..20 */
	float        track_comp_threshold_norm; /* 0..1 UI */
	float        track_comp_ratio_norm; /* 0..1 UI */
	bool         track_comp_sidechain; /* SC enabled + wired from src */
	int          track_comp_sidechain_src; /* source track index, or −1 */
	bool         track_eq_ok; /* ACE EQ on Audio 3 (track 2) */
	bool         track_eq_bypassed;
	float        track_eq_gain_l_db; /* Gain L −20..+20 */
	float        track_eq_gain_m_db; /* Gain 2 −20..+20 */
	float        track_eq_gain_h_db; /* Gain H −20..+20 */
	float        track_eq_gain_l_norm; /* 0..1 UI (0.5 = 0 dB) */
	float        track_eq_gain_m_norm;
	float        track_eq_gain_h_norm;
	bool         track_delay_ok; /* ACE Delay on Audio 4 (track 3) */
	bool         track_delay_bypassed;
	float        track_delay_time_ms; /* Time 1..8000 ms */
	float        track_delay_feedback_pct; /* Feedback 0..100 % */
	float        track_delay_time_norm; /* 0..1 UI (log) */
	float        track_delay_feedback_norm; /* 0..1 UI */
	bool         track_limiter_ok; /* ACE Limiter on Audio 5 (track 4) */
	bool         track_limiter_bypassed;
	float        track_limiter_ceiling_db; /* Ceiling −20..0 dB */
	float        track_limiter_release_ms; /* Release 1..2000 ms */
	float        track_limiter_ceiling_norm; /* 0..1 UI */
	float        track_limiter_release_norm; /* 0..1 UI */
	bool         track_gate_ok; /* ACE Gate on Audio 6 (track 5) */
	bool         track_gate_bypassed;
	float        track_gate_threshold_db; /* Threshold −60..0 dB */
	float        track_gate_hold_ms; /* Hold 1..500 ms */
	float        track_gate_threshold_norm; /* 0..1 UI */
	float        track_gate_hold_norm; /* 0..1 UI */
	/* Spectrum analyzer (UI-rate Goertzel bars; no RT alloc). */
	static constexpr int kSpectrumBands = 32;
	bool         spectrum_enabled;
	bool         spectrum_ok; /* last update produced usable bands */
	bool         spectrum_master; /* true = mix Audio 1..N; false = selected/Audio1 */
	int          spectrum_track; /* source track index, or −1 when master */
	float        spectrum_band[kSpectrumBands]; /* 0..1 display levels */
	wchar_t      spectrum_label[32];
	/* Phase correlation / goniometer (UI-rate L/R window; no RT alloc). */
	static constexpr int kGonioPts = 64;
	bool         correlation_enabled;
	bool         correlation_ok;
	bool         correlation_master; /* true = mix Audio 1..N L/R; false = selected */
	int          correlation_track;
	float        correlation; /* −1..+1 Pearson L/R */
	float        gonio_x[kGonioPts]; /* L (−1..+1) */
	float        gonio_y[kGonioPts]; /* R (−1..+1) */
	wchar_t      correlation_label[32];
	/* Approximate LUFS (UI-rate K-weight / BS.1770-style; not certified). */
	bool         loudness_enabled;
	bool         loudness_ok;
	bool         loudness_master; /* true = mix Audio 1..N; false = selected */
	int          loudness_track;
	float        loudness_momentary; /* ≈ LUFS over ~400 ms window */
	float        loudness_integrated; /* ≈ gated running mean */
	wchar_t      loudness_label[32];
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
	std::wstring track_comp_name; /* e.g. ACE Compressor */
	std::wstring track_eq_name; /* e.g. ACE EQ */
	std::wstring track_delay_name; /* e.g. ACE Delay */
	std::wstring track_limiter_name; /* e.g. ACE Limiter */
	std::wstring track_gate_name; /* e.g. ACE Gate */
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
	bool         track_frozen[kMeters]; /* AudioTrack::freeze_state == Frozen */
	bool         track_muted[kMeters]; /* Route::muted() — includes solo-mute */
	bool         track_soloed[kMeters]; /* Route::soloed() / self_soloed */
	/** Gain AutoState: 0=Off 1=Play 2=Write 3=Touch (Latch unused in shell). */
	int          track_gain_auto_state[kMeters];
	bool         track_solo_isolated[kMeters]; /* SoloIsolateControl self */
	bool         track_solo_safe[kMeters]; /* SoloSafeControl — ignore others' solo */
	static constexpr int kCommentLen = 40;
	wchar_t      track_comment[kMeters][kCommentLen]; /* Route::comment() */
	bool         track_phase_invert[kMeters]; /* PhaseControl any channel inverted */
	float        track_trim[kMeters]; /* TrimAutomation linear coeff (−20..+20 dB) */
	float        track_trim_norm[kMeters]; /* UI 0..1 (0.5 = unity / 0 dB) */
	/** MonitorChoice: 0=Auto 1=Input 2=Disk (MonitorCue unused in shell). */
	int          track_monitor_choice[kMeters];
	float        track_pan[kMeters]; /* pan azimuth 0=L … 0.5=C … 1=R */
	/** Pan AutoState: 0=Off 1=Play 2=Write 3=Touch (Latch unused in shell). */
	int          track_pan_auto_state[kMeters];
	float        track_send[kMeters]; /* aux send gain coeff 0..1 → Aux bus */
	bool         track_send_ok[kMeters]; /* InternalSend present on track */
	/** Send AutoState: 0=Off 1=Play 2=Write 3=Touch (Latch unused in shell). */
	int          track_send_auto_state[kMeters];
	bool         aux_bus_ok; /* Session Aux audio bus exists */
	bool         midi_muted;
	bool         midi_soloed;
	bool         midi_armed; /* MidiTrack rec_enable_control */
	int          midi_program; /* 0..127 Program Change (shell + immediate) */
	int          midi_bank; /* 0..127 Bank Select MSB (CC0); LSB forced 0 */
	bool         snap_enabled; /* Editor snap-to-grid */
	bool         midi_step_enabled; /* Step-record: keys/click place at playhead */
	int          midi_chord_quality; /* 0=off 1=maj (R+4+7) 2=min (R+3+7) */
	bool         midi_arp_enabled; /* live arpeggiator into MidiTrack synth */
	int          midi_arp_mode; /* 0=up 1=down 2=up-down */
	int          midi_arp_pitch; /* currently sounding pitch, or −1 */
	bool         midi_scale_lock; /* constrain step/click roots to scale */
	int          midi_scale_kind; /* 0=major 1=natural minor */
	int          midi_scale_root; /* 0=C .. 11=B */
	int          snap_density; /* 0=bar 1=beat 2=1/2 3=1/4 */
	int          grid_line_count; /* visible grid lines for Editor draw */
	static constexpr int kMaxGridLines = 96;
	float        grid_line_sec[kMaxGridLines];
	bool         grid_line_bar[kMaxGridLines]; /* thicker bar lines */
	float        tempo_bpm; /* Session TempoMap BPM at t=0 (quarter-note) */
	bool         count_in_enabled; /* Session config count-in before record roll */
	bool         click_enabled; /* RC Config clicking — audible metronome */
	int          listen_mode; /* 0=Solo 1=PFL 2=AFL (RC solo-control-is-listen + listen-position) */
	int          audio_track_count; /* Session audio tracks (1..kMeters) */
	int          midi_track_count; /* 0 or 1 in this shell */
	static constexpr int kTrackNameLen = 32;
	wchar_t      track_name[kMeters][kTrackNameLen]; /* Session Route names */
	wchar_t      midi_track_name[kTrackNameLen];
	float        track_gain[kMeters]; /* Route gain_control mirror after reorder */
	bool         track_in_group[kMeters]; /* member of Session RouteGroup */
	int          group_member_count; /* 0 = no shell group */
	bool         group_active; /* RouteGroup::is_active */
	bool         group_relative; /* RouteGroup::is_relative — proportional gain */
	bool         group_shares_mute;
	bool         group_shares_solo;
	bool         group_shares_gain;
	bool         group_shares_recenable;
	bool         group_shares_color;
	uint32_t     group_rgba; /* RouteGroup color RRGGBBAA when shares color */
	bool         group_shares_monitoring;
	static constexpr int kGroupNameLen = 32;
	wchar_t      group_name[kGroupNameLen];
	bool         mixer_narrow; /* shell UI: compact strip width */
	bool         monitor_ok; /* Session monitor_out + MonitorProcessor */
	bool         monitor_dim; /* MonitorProcessor::dim_all */
	float        monitor_dim_db; /* dim_level −20..0 dB (default −12) */
	bool         monitor_mono;
	bool         monitor_cut; /* cut_all = monitor mute */
	float        monitor_solo_boost_db; /* solo_boost_level 0..+10 dB (default 0) */
	float        transport_sec;
	float        transport_speed; /* Session actual_speed (−N..+N); 1 = play */
	int          bbt_bars; /* TempoMap BBT at playhead (1-based) */
	int          bbt_beats;
	int          bbt_ticks;
	int          tc_hours; /* Session timecode at playhead */
	int          tc_minutes;
	int          tc_seconds;
	int          tc_frames;
	/* Preferences (RC Config + WinShellPrefs instant.xml). */
	std::wstring pref_backend; /* "Dummy" or "WASAPI" */
	uint32_t     pref_period_frames; /* 0 = Auto */
	bool         pref_snap_default;
	bool         pref_click_default; /* mirrors Config clicking when saved */
	static constexpr int kPrefsHintLen = 96;
	wchar_t      vst3_path_hint[kPrefsHintLen];
	/* Recent sessions (WinShellRecent instant.xml, beside WinShellPrefs). */
	static constexpr int kMaxRecent = 8;
	static constexpr int kRecentLabelLen = 48;
	static constexpr int kRecentPathLen = 96;
	int          recent_count;
	wchar_t      recent_label[kMaxRecent][kRecentLabelLen]; /* folder basename */
	wchar_t      recent_path[kMaxRecent][kRecentPathLen]; /* truncated display path */
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

	/** Load WinShellPrefs instant.xml + refresh prefs snapshot fields. */
	void load_shell_prefs ();
	/** Write WinShellPrefs + Config::save_state (clicking). */
	bool save_shell_prefs ();
	void set_pref_backend (const std::string& name); /* Dummy / WASAPI */
	void set_pref_period_frames (uint32_t frames);
	void set_pref_snap_default (bool on);
	/** Click default → Config::set_clicking (+ save on save_shell_prefs). */
	bool set_pref_click_default (bool on);
	void refresh_prefs_snap ();

	/**
	 * Toggle click + snap prefs; save RC + instant; verify reload.
	 * For --smoke-prefs.
	 */
	bool smoke_prefs ();

	/** Load WinShellRecent (sibling of WinShellPrefs in instant.xml). */
	void load_recent_sessions ();
	/** Persist WinShellRecent; does not rewrite WinShellPrefs. */
	bool save_recent_sessions ();
	/** Push path to front of recent list (dedupe); optionally persist. */
	void remember_recent_session (const std::string& session_dir,
	                              const std::string& snapshot = "win_shell",
	                              bool persist = true);
	void clear_recent_sessions (bool persist = true);
	int  recent_count () const;
	/** Copy path/snapshot for index; false if out of range. */
	bool recent_entry (int index, std::string* path, std::string* snapshot) const;
	/** open_session for a remembered index. */
	bool open_recent (int index);
	/**
	 * Remember → save → clobber → reload → open first. For --smoke-recent.
	 */
	bool smoke_recent ();

	/**
	 * Autosave / crash recovery (WinShellRecovery instant.xml + recovery .ardour copy).
	 * Disabled for --timeout smokes via set_autosave_enabled(false).
	 */
	void set_autosave_enabled (bool on);
	bool autosave_enabled () const { return _autosave_enabled; }
	/** Periodic tick; saves when interval elapsed (GetTickCount ms). */
	void autosave_tick (uint32_t now_ms);
	/** Force Session save + write recovery marker / file copy. */
	bool autosave_now ();
	/** True when dirty recovery is newer than last clean open and path exists. */
	bool recovery_offer_available () const;
	bool recovery_offer_path (std::string* path, std::string* snapshot) const;
	/** Open the recovered session path. */
	bool restore_recovery_session ();
	/** Clear dirty flag (user discarded). */
	bool discard_recovery_offer ();
	/** Mark current open as clean (updates last-open FILETIME). */
	bool mark_recovery_clean_open ();
	/**
	 * Autosave → verify marker → discard dirty → verify. For --smoke-recovery.
	 */
	bool smoke_recovery ();

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
	/**
	 * Set every audio + MIDI fader to unity (1.0 linear, NoGroup).
	 * For Alt+click fader / --smoke-fader-reset-all.
	 */
	bool reset_all_faders ();
	/** Drop gains → reset_all → verify ≈1.0. For --smoke-fader-reset-all. */
	bool smoke_fader_reset_all ();
	float gain () const;

	/** Arm / disarm a track for recording (Session rec-enable control). */
	bool set_track_armed (int track, bool armed);
	bool toggle_track_armed (int track);
	/**
	 * Disarm every audio track rec-enable (NoGroup).
	 * For Alt+click Arm / --smoke-disarm-all.
	 */
	bool disarm_all_tracks ();
	/** Arm 0+1 → disarm_all → verify none armed. For --smoke-disarm-all. */
	bool smoke_disarm_all ();
/**
	 * Arm every audio track rec-enable (NoGroup).
	 * For Alt+Shift+click Arm / --smoke-arm-all.
	 */
	bool arm_all_tracks ();
	/** Disarm → arm 0+1 via arm_all → verify. For --smoke-arm-all. */
	bool smoke_arm_all ();

	/**
	 * Route mute/solo (audio tracks 0..kMeters-1).
	 * Uses Controllable::UseGroup when the track is in an active sharing group.
	 */
	bool set_track_muted (int track, bool muted);
	bool toggle_track_muted (int track);
	bool set_track_soloed (int track, bool soloed);
	bool toggle_track_soloed (int track);

	/**
	 * Solo-isolate a track (SoloIsolateControl) so upstream solo doesn't mute it.
	 * For Shift+click Solo / --smoke-solo-isolate.
	 */
	bool set_track_solo_isolated (int track, bool isolated);
	bool toggle_track_solo_isolated (int track = 0);
	/** Off → on → off on Audio 1. */
	bool smoke_solo_isolate ();
	/**
	 * Solo-safe a track (SoloSafeControl) so other solos don't mute it.
	 * For Ctrl+click Solo / --smoke-solo-safe.
	 */
	bool set_track_solo_safe (int track, bool safe);
	bool toggle_track_solo_safe (int track = 0);
	/** Off → on → off on Audio 1. */
	bool smoke_solo_safe ();
/**
	 * Invert polarity on every channel of an audio track (PhaseControl).
	 * For Alt+Shift+I / Alt+click strip label / --smoke-phase-invert.
	 */
	bool set_track_phase_invert (int track, bool invert);
	bool toggle_track_phase_invert (int track = 0);
	/** Off → on → off on Audio 1. For --smoke-phase-invert. */
	bool smoke_phase_invert ();

	/** MIDI track mute/solo (Session MidiTrack as Route). */
	bool set_midi_muted (bool muted);
	bool toggle_midi_muted ();
	bool set_midi_soloed (bool soloed);
	bool toggle_midi_soloed ();
	/**
	 * Mute every audio track + MIDI (Controllable::NoGroup).
	 * For Alt+Shift+M / Alt+click Mute / --smoke-mute-all.
	 */
	bool mute_all_tracks ();
	/** Unmute → mute_all → every audio + MIDI muted. */
	bool smoke_mute_all ();
	/**
	 * Solo every audio track + MIDI (Controllable::NoGroup).
	 * For Alt+Shift+U / Alt+Shift+click Solo / --smoke-solo-all.
	 */
	bool solo_all_tracks ();
	/** Unsolo → solo_all → every audio + MIDI soloed. */
	bool smoke_solo_all ();
	/** MIDI track rec-enable (Arm on MIDI mixer strip). */
	bool set_midi_armed (bool armed);
	bool toggle_midi_armed ();
	/** Off → on → off. For --smoke-midi-arm. */
	bool smoke_midi_arm ();

	/**
	 * Clear solo on every audio track + MIDI (Controllable::NoGroup).
	 * For Alt+U / --smoke-clear-solos.
	 */
	bool clear_all_solos ();

	/**
	 * Clear mute on every audio track + MIDI (Controllable::NoGroup).
	 * For Alt+M / --smoke-clear-mutes.
	 */
	bool clear_all_mutes ();

	/**
	 * Solo only this audio track (or MIDI): clear every other solo first.
	 * Controllable::NoGroup so group peers do not re-solo. For Alt+click Solo.
	 */
	bool exclusive_solo_track (int track);
	bool exclusive_solo_midi ();

	/**
	 * Route::comment on audio tracks (0..kMeters-1).
	 * For Alt+Shift+N / Alt+Shift+click label / --smoke-track-comment.
	 */
	bool set_track_comment (int track, const char* utf8);
	bool clear_track_comment (int track);
	/** Empty ↔ "note" on the given track. */
	bool toggle_track_comment (int track = 0);
	/** Set "smoke" → verify → clear on Audio 1. */
	bool smoke_track_comment ();

	/**
	 * Solo vs listen: 0=Solo, 1=PFL, 2=AFL (RC Config).
	 * For Alt+Shift+L / --smoke-listen-mode.
	 */
	bool set_listen_mode (int mode_0_solo_1_pfl_2_afl);
	bool cycle_listen_mode ();
	/** Solo → PFL → AFL → Solo. For --smoke-listen-mode. */
	bool smoke_listen_mode ();

	/**
	 * Mute/solo Audio 1 + MIDI round-trip; clear. For --smoke-mute-solo.
	 */
	bool smoke_mute_solo ();
	/** Solo Audio 1 + MIDI → clear_all_solos → none soloed. */
	bool smoke_clear_solos ();
	/** Mute Audio 1 + MIDI → clear_all_mutes → none muted. */
	bool smoke_clear_mutes ();
	/** Solo Audio1+2+MIDI → exclusive Audio1 → only Audio1 soloed. */
	bool smoke_exclusive_solo ();

	/**
	 * Strip preamp trim (Route::trim_control / TrimAutomation).
	 * linear: 0.1..10 (−20..+20 dB); norm UI: 0..1 (0.5 = unity).
	 * For drag / dbl-click / Alt+Shift+T / --smoke-strip-trim.
	 */
	bool  set_track_trim (int track, float linear);
	bool  set_track_trim_norm (int track, float norm_0_1);
	float track_trim (int track) const;
	float track_trim_norm (int track) const;

	/** Unity → −6 dB → +6 dB → unity on Audio 1. For --smoke-strip-trim. */
	bool smoke_strip_trim ();

	/** Pan azimuth 0=L … 0.5=C … 1=R via Route::pan_azimuth_control. */
	bool  set_track_pan (int track, float pan_0_1);
	float track_pan (int track) const;
	/**
	 * Pan AutoState: 0=Off 1=Play 2=Write 3=Touch.
	 * --smoke-pan-auto-state / Alt+Shift+Y (Pan lane).
	 */
	bool set_track_pan_auto_state (int track, int state_0_off_1_play_2_write_3_touch);
	bool cycle_track_pan_auto_state (int track = 0);
	/**
	 * Center every audio track pan (0.5, NoGroup).
	 * For Alt+click pan / --smoke-pan-reset-all.
	 */
	bool reset_all_pans ();
	/** Pan L/R then reset_all → verify center. For --smoke-pan-reset-all. */
	bool smoke_pan_reset_all ();

	/** Pan L → C → R on Audio 1. For --smoke-pan. */
	bool smoke_pan ();
	/** Cycle Off→Play→Write→Touch→Off on Audio 1 pan. For --smoke-pan-auto-state. */
	bool smoke_pan_auto_state ();

	/** Aux send level 0..1 (gain coeff) via InternalSend → Aux bus. */
	bool  set_track_send (int track, float send_0_1);
	float track_send (int track) const;
	/**
	 * Aux-send AutoState: 0=Off 1=Play 2=Write 3=Touch.
	 * --smoke-send-auto-state / Alt+Shift+Y (Send lane).
	 */
	bool set_track_send_auto_state (int track, int state_0_off_1_play_2_write_3_touch);
	bool cycle_track_send_auto_state (int track = 0);
	/**
	 * Zero every audio track aux send (NoGroup).
	 * For Alt+click send / --smoke-send-reset-all.
	 */
	bool reset_all_sends ();
	/** Raise sends → reset_all → verify ≈0. For --smoke-send-reset-all. */
	bool smoke_send_reset_all ();

	/** Send 0 → 0.75 → 0.25 on Audio 1. For --smoke-send. */
	bool smoke_send ();
	/** Cycle Off→Play→Write→Touch→Off on Audio 1 send. For --smoke-send-auto-state. */
	bool smoke_send_auto_state ();

	/**
	 * Reset one audio strip: fader → unity, pan → center, aux send → 0.
	 * For Alt+dbl-click mixer / Alt+Shift+R / --smoke-strip-mixer-reset.
	 */
	bool reset_strip_mixer (int track = 0);
	/** Dirty gain/pan/send then reset_strip → verify. */
	bool smoke_strip_mixer_reset ();

	/**
	 * Track fader AutomationList (GainAutomation). Enable Play, write points,
	 * sample curve for Editor automation lane.
	 */
	bool ensure_track_gain_automation (int track = 0);
	bool write_track_gain_automation (int track, float time_sec, float value_0_1);
	bool clear_track_gain_automation (int track = 0);
	/**
	 * Gain AutoState: 0=Off 1=Play 2=Write 3=Touch.
	 * Alt+Shift+Y (Gain lane) / --smoke-auto-state.
	 */
	bool set_track_gain_auto_state (int track, int state_0_off_1_play_2_write_3_touch);
	bool cycle_track_gain_auto_state (int track = 0);
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
	/** Cycle Off→Play→Write→Touch→Off on Audio 1 gain. For --smoke-auto-state. */
	bool smoke_auto_state ();

	/**
	 * Track pan azimuth AutomationList (PanAzimuthAutomation). Editor lane
	 * when shell cycles Gain→Pan→Send (Shift+click lane / Alt+Shift+A).
	 */
	bool ensure_track_pan_automation (int track = 0);
	bool write_track_pan_automation (int track, float time_sec, float value_0_1);
	bool clear_track_pan_automation (int track = 0);
	bool fill_pan_automation_curve (int track,
	                                float view_start_sec,
	                                float view_span_sec,
	                                float* y_0_1,
	                                size_t n);
	int  fill_pan_automation_points (int track,
	                                 float view_start_sec,
	                                 float view_span_sec,
	                                 float* x_norm,
	                                 float* y_0_1,
	                                 int max_n);
	/** Clear → L/C/R points → Play. For --smoke-pan-automation. */
	bool smoke_pan_automation ();

	/**
	 * Aux send level AutomationList (BusSendLevel via InternalSend → Aux).
	 * Editor lane when shell cycles Gain→Pan→Send.
	 */
	bool ensure_track_send_automation (int track = 0);
	bool write_track_send_automation (int track, float time_sec, float value_0_1);
	bool clear_track_send_automation (int track = 0);
	bool fill_send_automation_curve (int track,
	                                 float view_start_sec,
	                                 float view_span_sec,
	                                 float* y_0_1,
	                                 size_t n);
	int  fill_send_automation_points (int track,
	                                  float view_start_sec,
	                                  float view_span_sec,
	                                  float* x_norm,
	                                  float* y_0_1,
	                                  int max_n);
	/** Clear → 0/0.75/0.25 points → Play. For --smoke-send-automation. */
	bool smoke_send_automation ();
	/**
	 * Gain + pan + send automation smokes (unify lane). For --smoke-auto-lane-unify.
	 */
	bool smoke_auto_lane_unify ();
	/** Cycle AutoState for lane_kind 0=Gain 1=Pan 2=Send. */
	bool cycle_track_auto_state_for_lane (int track, int lane_kind);
	/**
	 * For each lane kind: Off then cycle ×4 asserting snap. For --smoke-auto-state-unify.
	 */
	bool smoke_auto_state_unify ();
	/**
	 * Clear Audio 0+1; write gain auto on Audio 2 (index 1); verify Audio 1
	 * empty and Audio 2 eval. For --smoke-auto-lane-focus (focused-track lane).
	 */
	bool smoke_auto_lane_focus ();

	/**
	 * MidiModel CC AutomationList (region automation, BeatTime). Default CC7
	 * (volume) on channel 0. Values stored 0..127; API uses 0..1 for the D2D lane.
	 */
	static constexpr int kMidiCcVolume = 7;
	bool ensure_midi_cc_automation (int cc = kMidiCcVolume, int channel = 0);
	bool write_midi_cc_automation (float time_sec, float value_0_1,
	                               int cc = kMidiCcVolume, int channel = 0);
	bool clear_midi_cc_automation (int cc = kMidiCcVolume, int channel = 0);
	bool fill_midi_cc_automation_curve (float view_start_sec,
	                                    float view_span_sec,
	                                    float* y_0_1,
	                                    size_t n,
	                                    int cc = kMidiCcVolume,
	                                    int channel = 0);
	int  fill_midi_cc_automation_points (float view_start_sec,
	                                     float view_span_sec,
	                                     float* x_norm,
	                                     float* y_0_1,
	                                     int max_n,
	                                     int cc = kMidiCcVolume,
	                                     int channel = 0);
	/** Clear → 3 CC7 points → Play; also verifies CC1. For --smoke-midi-cc. */
	bool smoke_midi_cc ();

	/**
	 * MidiModel pitch-bend AutomationList (0..16383; UI maps 0..1, center=0.5).
	 */
	bool ensure_midi_bend_automation (int channel = 0);
	bool write_midi_bend_automation (float time_sec, float value_0_1, int channel = 0);
	bool clear_midi_bend_automation (int channel = 0);
	bool fill_midi_bend_automation_curve (float view_start_sec,
	                                      float view_span_sec,
	                                      float* y_0_1,
	                                      size_t n,
	                                      int channel = 0);
	int  fill_midi_bend_automation_points (float view_start_sec,
	                                       float view_span_sec,
	                                       float* x_norm,
	                                       float* y_0_1,
	                                       int max_n,
	                                       int channel = 0);
	/** Clear → 3 bend points → Play. For --smoke-midi-bend. */
	bool smoke_midi_bend ();

	/**
	 * MidiModel channel-pressure AutomationList (0..127; UI maps 0..1).
	 */
	bool ensure_midi_pressure_automation (int channel = 0);
	bool write_midi_pressure_automation (float time_sec, float value_0_1, int channel = 0);
	bool clear_midi_pressure_automation (int channel = 0);
	bool fill_midi_pressure_automation_curve (float view_start_sec,
	                                          float view_span_sec,
	                                          float* y_0_1,
	                                          size_t n,
	                                          int channel = 0);
	int  fill_midi_pressure_automation_points (float view_start_sec,
	                                           float view_span_sec,
	                                           float* x_norm,
	                                           float* y_0_1,
	                                           int max_n,
	                                           int channel = 0);
	/** Clear → 3 pressure points → Play. For --smoke-midi-pressure. */
	bool smoke_midi_pressure ();

	/**
	 * MidiModel polyphonic aftertouch (note-pressure) AutomationList for one
	 * note id (default C4=60). Values 0..127; UI maps 0..1. F8 lane / PolyAT.
	 */
	static constexpr int kMidiPolyAtNote = 60; /* C4 */
	bool ensure_midi_note_pressure_automation (int note = kMidiPolyAtNote,
	                                           int channel = 0);
	bool write_midi_note_pressure_automation (float time_sec, float value_0_1,
	                                          int note = kMidiPolyAtNote,
	                                          int channel = 0);
	bool clear_midi_note_pressure_automation (int note = kMidiPolyAtNote,
	                                          int channel = 0);
	bool fill_midi_note_pressure_automation_curve (float view_start_sec,
	                                               float view_span_sec,
	                                               float* y_0_1,
	                                               size_t n,
	                                               int note = kMidiPolyAtNote,
	                                               int channel = 0);
	int  fill_midi_note_pressure_automation_points (float view_start_sec,
	                                                float view_span_sec,
	                                                float* x_norm,
	                                                float* y_0_1,
	                                                int max_n,
	                                                int note = kMidiPolyAtNote,
	                                                int channel = 0);
	/** Clear → 3 PolyAT·C4 points → Play. For --smoke-midi-note-pressure. */
	bool smoke_midi_note_pressure ();

	/** Clear PeakMeter max-peak / clip latch on Audio track (0..kMeters-1). */
	bool clear_track_meter_clip (int track);
	/**
	 * Clear clip latch on every audio track PeakMeter.
	 * For Alt+click meter / --smoke-clear-meter-clips.
	 */
	bool clear_all_meter_clips ();
	/** Mark snap clips then clear_all; verify none remain. */
	bool smoke_clear_meter_clips ();

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
	/** Bounce route outs to WAV. track_index −1 = master_out. */
	bool export_route_index_wav (int track_index,
	                             const std::string& outfile,
	                             int64_t start_sample,
	                             int64_t end_sample);
	/**
	 * Export each audio track's output to a separate WAV under folder
	 * (default: session export/stems/). Returns count of stems written.
	 * For Shift+Export / --smoke-export-stems.
	 */
	bool export_stems (const std::string& folder = std::string (),
	                   int* out_count = nullptr);
	/** Import tones on Audio 1+2, export stems, verify ≥2 WAVs. */
	bool smoke_export_stems (int* out_count = nullptr);

	/** Convenience: export after ensuring content; for --smoke-export. */
	bool smoke_export_wav (std::string* out_path = nullptr);

	/** Session history undo/redo (Ctrl+Z / Ctrl+Y). */
	bool undo (uint32_t n = 1);
	bool redo (uint32_t n = 1);
	/**
	 * Jump by undoing (index+1) steps. index 0 = newest (next_undo).
	 * Honest API: Session only exposes undo(n), not random-access restore.
	 */
	bool undo_to_index (int index);

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

	/**
	 * Time-stretch selected AudioRegion by time_ratio (pitch unchanged).
	 * Prefer Session RBStretch (RubberBand). If RB unavailable, falls back to
	 * region scale (set_length) — length change without resampling.
	 * Default 1.5; use e.g. 0.75 to compress. Peaks refresh via region flags.
	 */
	bool stretch_selected (float time_ratio = 1.5f);

	/**
	 * Pitch-shift selected AudioRegion by semitones (duration held).
	 * Session Pitch / RubberBand; pitch_fraction = 2^(st/12). Default ±1.
	 */
	bool pitch_shift_selected (float semitones = 1.f);

	/**
	 * Reverse selected AudioRegion via Session Reverse Filter.
	 * Duration/position held; peaks refresh after playlist replace.
	 */
	bool reverse_selected ();

	/**
	 * Normalize selected AudioRegion to target_dbfs (default −1 dBFS).
	 * Uses AudioRegion::maximum_amplitude + normalize (scale_amplitude).
	 * Peaks refresh via scale applied in read_peaks.
	 */
	bool normalize_selected (float target_dbfs = -1.f);

	/**
	 * Strip silence from selected AudioRegion (find_silence + StripSilence Filter).
	 * Threshold dBFS (default −40); min silence / fade in seconds.
	 * Quiet spans removed; audible parts become one or more regions.
	 */
	bool strip_silence_selected (float threshold_dbfs = -40.f,
	                             float min_silence_sec = 0.05f,
	                             float fade_sec = 0.005f);

	/**
	 * Consolidate (bounce in place) a track time range to one new region.
	 * Track::bounce_range (no processing) → cut range → add discrete copy.
	 */
	bool consolidate_range (int track, float start_sec, float end_sec);

	/** Consolidate selected region extent (selection_start..end) in place. */
	bool consolidate_selected ();

	/**
	 * Freeze audio track: bounce session range with FX → frozen playlist,
	 * deactivate freezable processors (AudioTrack::freeze_me).
	 */
	bool freeze_track (int track);
	/** Restore pre-freeze playlist + processor state (AudioTrack::unfreeze). */
	bool unfreeze_track (int track);
	/** Freeze if thawed; unfreeze if Frozen. */
	bool toggle_freeze_track (int track);
	bool track_is_frozen (int track) const;

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

	/**
	 * Set Session TempoMap BPM (quarter note) at the map origin.
	 * Grid snap/lines follow TempoMap after update. Clamped ~20..400.
	 */
	bool set_tempo_bpm (float bpm);
	/** Nudge current BPM by delta (e.g. ±1 / ±5). */
	bool nudge_tempo_bpm (float delta);

	/**
	 * Session config count-in: when enabled, 1–2 bar click count-in before
	 * record roll (Rec armed + Play, or punch+record). Plain Play skips it.
	 */
	bool set_count_in (bool enable);
	bool toggle_count_in ();

	/**
	 * RC Config clicking — Session metronome on ClickIO → physical outs.
	 * Audible during play/record when enabled (unless click-record-only).
	 */
	bool set_clicking (bool enable);
	bool toggle_clicking ();

	/** Count audio regions on a track playlist (smoke / diagnostics). */
	int track_region_count (int track) const;

	/**
	 * Import tone → split → crossfade → trim → move; verify lengths/position.
	 * For --smoke-edit.
	 */
	bool smoke_edit ();

	/**
	 * Import tone → select → stretch 1.5×; verify length ~1.5× source.
	 * For --smoke-stretch.
	 */
	bool smoke_stretch ();

	/**
	 * Import tone → select → pitch +1 semitone; verify duration held.
	 * For --smoke-pitch.
	 */
	bool smoke_pitch ();

	/**
	 * Import tone → select → reverse; verify duration held.
	 * For --smoke-reverse.
	 */
	bool smoke_reverse ();

	/**
	 * Import tone → select → normalize −1 dBFS; verify scale_amplitude.
	 * For --smoke-normalize.
	 */
	bool smoke_normalize ();

	/**
	 * Import gated tone (silence gaps) → strip silence → ≥2 regions.
	 * For --smoke-strip-silence.
	 */
	bool smoke_strip_silence ();

	/**
	 * Import → split → consolidate range covering both → 1 region.
	 * For --smoke-consolidate.
	 */
	bool smoke_consolidate ();

	/**
	 * Import → freeze track 0 → Frozen + frozen playlist → unfreeze.
	 * For --smoke-freeze.
	 */
	bool smoke_freeze ();

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
	/**
	 * Import tone; verify undo list names; undo_to_index(0); redo.
	 * For --smoke-history.
	 */
	bool smoke_history ();

	/** Create/update Session auto-loop Location [start_sec, end_sec]. */
	bool set_loop_range (float start_sec, float end_sec);
	/** Set loop start or end (keeps the other; creates a 1s default span if needed). */
	bool set_loop_start (float start_sec);
	bool set_loop_end (float end_sec);
	bool set_loop_play (bool enable);
	bool toggle_loop_play ();

	/**
	 * Create/update Session IsAutoPunch Location [start_sec, end_sec].
	 * When enabling punch with no range, copies loop range if present.
	 */
	bool set_punch_range (float start_sec, float end_sec);
	bool set_punch_start (float start_sec);
	bool set_punch_end (float end_sec);
	/** Copy current auto-loop range into punch (creates punch if needed). */
	bool copy_loop_to_punch ();
	/** Arm/disarm Session punch-in + punch-out (record only inside range). */
	bool set_punch_enabled (bool enable);
	bool toggle_punch_enabled ();

	/** Add IsMark at time_sec (name auto if empty). */
	bool add_marker_at (float time_sec, const std::string& name = std::string ());
	/** Remove nearest IsMark within slop_sec of time_sec. */
	bool remove_marker_near (float time_sec, float slop_sec = 0.35f);
	/** Locate transport to time_sec (optional snap to nearest mark within slop). */
	bool locate_to (float time_sec, bool snap_to_marker = false, float slop_sec = 0.35f);
	/** Locate transport to selected region start / exclusive end. */
	bool locate_to_selection_start ();
	bool locate_to_selection_end ();

bool locate_next_marker ();
	bool locate_prev_marker ();
	bool smoke_marker_navigate ();
	bool rename_marker_near (float time_sec, const std::string& name,
	                         float slop_sec = 0.35f);
	bool smoke_marker_rename ();
	bool set_marker_locked_near (float time_sec, bool locked, float slop_sec = 0.35f);
	bool toggle_marker_locked_near (float time_sec, float slop_sec = 0.35f);
	bool smoke_marker_lock ();
	bool set_marker_cd_near (float time_sec, bool cd, float slop_sec = 0.35f);
	bool toggle_marker_cd_near (float time_sec, float slop_sec = 0.35f);
	bool smoke_cd_marker ();
	bool add_range_marker (float start_sec, float end_sec,
	                       const std::string& name = std::string ());
	bool add_range_marker_from_selection ();
	bool remove_range_marker_near (float time_sec, float slop_sec = 0.35f);
	bool smoke_range_marker ();
	bool set_selection_sync_at (float time_sec);
	bool set_selection_sync_at_playhead ();
	bool clear_selection_sync ();
	bool smoke_region_sync ();
	bool add_scene_marker_at (float time_sec, int program = 0,
	                         const std::string& name = std::string (),
	                         int bank = 0, int channel = 0);
	bool remove_scene_marker_near (float time_sec, float slop_sec = 0.35f);
	bool cycle_scene_channel_near (float time_sec, float slop_sec = 0.35f);
	bool smoke_scene_channel ();
	bool add_cue_marker_at (float time_sec, int32_t cue_id = 1,
	                        const std::string& name = std::string ());
	bool remove_cue_marker_near (float time_sec, float slop_sec = 0.35f);
	bool locate_next_cue ();
	bool locate_prev_cue ();
	bool smoke_cue_navigate ();
	bool add_section_marker_at (float time_sec,
	                           const std::string& name = std::string ());
	bool remove_section_marker_near (float time_sec, float slop_sec = 0.35f);
	bool rename_section_near (float time_sec, const std::string& name,
	                          float slop_sec = 0.35f);
	bool smoke_section_rename ();
	bool locate_punch_in ();
	bool locate_punch_out ();
	bool smoke_punch_navigate ();
	bool locate_session_start ();
	bool locate_session_end ();
	bool smoke_session_extent_navigate ();
	bool locate_loop_start ();
	bool locate_loop_end ();
	bool smoke_loop_navigate ();
	bool smoke_zoom_punch ();
	bool smoke_zoom_loop ();
	bool smoke_zoom_selection ();
	bool nudge_playhead (int direction);
	bool smoke_playhead_nudge ();
	bool smoke_zoom_session ();
	bool smoke_zoom_regions ();
	bool smoke_center_playhead ();
	bool smoke_zoom_playhead ();
	bool smoke_scroll_playhead ();

bool add_skip_range (float start_sec, float end_sec, const std::string& name = std::string ());
	bool add_skip_range_from_selection ();
	bool copy_selection_to_loop ();
	bool copy_selection_to_punch ();
	bool cycle_region_selection (int direction = 1, int track = -1);
	bool cycle_selected_fade_shape ();
	bool cycle_selected_region_layer ();
	bool duplicate_selected_region ();
	bool ensure_selected_region_envelope ();
	bool export_selection_wav (const std::string& outfile = std::string ());
	bool fill_selected_region_envelope_curve (float* y_0_1, size_t n);
	bool lower_selected_region (bool to_bottom = false);
	bool nudge_selected_by_sec (float delta_sec);
	bool nudge_selected_fade_in_sec (float delta_sec);
	bool nudge_selected_fade_out_sec (float delta_sec);
	bool nudge_selected_region_envelope_mid (float delta_coeff);
	bool nudge_selection_gain_db (float delta_db);
	bool raise_selected_region (bool to_top = false);
	bool remove_skip_range_near (float time_sec, float slop_sec = 0.35f);
	float selected_region_envelope_mid () const;
	bool set_selected_fade_active (bool active);
	bool set_selected_region_envelope_mid (float coeff_0_1);
	bool set_selected_region_name (const std::string& name);
	bool set_selection_gain_db (float gain_db);
	bool set_selection_locked (bool locked);
	bool set_selection_muted (bool muted);
	bool set_selection_opaque (bool opaque);
	bool set_selection_position_locked (bool locked);
	bool smoke_clear_selection ();
	bool smoke_click_toggle ();
	bool smoke_count_in ();
	bool smoke_crossfade_handles ();
	bool smoke_crossfade_shape ();
	bool smoke_export_selection (std::string* out_path = nullptr);
	bool smoke_fade_active ();
	bool smoke_fade_nudge ();
	bool smoke_fade_shape_expand ();
	bool smoke_grid_lines ();
	bool smoke_loop_selection ();
	bool smoke_punch_selection ();
	bool smoke_punch_toggle ();
	bool smoke_region_caption ();
	bool smoke_region_cycle ();
	bool smoke_region_duplicate ();
	bool smoke_region_envelope ();
	bool smoke_region_envelope_drag ();
	bool smoke_region_envelope_nudge ();
	bool smoke_region_layer ();
	bool smoke_region_layer_cycle ();
	bool smoke_region_lock ();
	bool smoke_region_mute_gain ();
	bool smoke_region_nudge ();
	bool smoke_region_opaque ();
	bool smoke_region_position_lock ();
	bool smoke_region_rename ();
	bool smoke_skip_range ();
	bool smoke_snap_density ();
	bool smoke_snap_toggle ();
	bool smoke_tempo_nudge ();
	bool toggle_selected_fade_active ();
	bool toggle_selection_locked ();
	bool toggle_selection_muted ();
	bool toggle_selection_opaque ();
	bool toggle_selection_position_locked ();
	/**
	 * Continuous scrub locate (no mark snap). Same Session request_locate path.
	 * Used while dragging the editor ruler.
	 */
	bool scrub_to (float time_sec);
	/**
	 * Varispeed / shuttle via Session::request_transport_speed.
	 * Non-zero speed also requests roll; 0 stops. Honest MMC shuttle path.
	 */
	bool set_transport_speed (double speed);
	bool stop_shuttle ();

	/**
	 * Locate 1.0→2.5s; shuttle +2→0; verify BBT/TC clock fields.
	 * For --smoke-locate. Does not leave transport rolling.
	 */
	bool smoke_locate ();
	/**
	 * Import → select → locate start/end; verify transport_sec.
	 * For --smoke-locate-selection.
	 */
	bool smoke_locate_selection ();

	/**
	 * Session range (current_start..current_end) or playlist fallback, seconds.
	 * Used by Editor zoom-fit session.
	 */
	bool session_extent_sec (float* start_sec, float* end_sec) const;
	/**
	 * Union of all audio (+ MIDI) region extents across tracks, seconds.
	 * Used by Editor zoom-fit all regions.
	 */
	bool regions_extent_sec (float* start_sec, float* end_sec) const;

	/**
	 * Ensure tone on Audio 0; verify session + regions extents.
	 * Shell applies WaveStrip fit_* and checks view window. For --smoke-zoom.
	 */
	bool smoke_zoom ();

	/**
	 * Set loop 0.5–2.5s, add two marks, remove one; verify snapshot.
	 * Does not enable loop-play (avoids forced roll during --timeout).
	 */
	bool smoke_loop_markers ();

	/**
	 * Set punch 0.5–2.5s, enable punch-in/out, verify snapshot; disable.
	 * For --smoke-punch. Does not roll transport.
	 */
	bool smoke_punch ();

	/**
	 * Set BPM 60 then 140; verify snap beat spacing follows TempoMap;
	 * toggle count-in; restore 120 + count-in off. For --smoke-tempo.
	 */
	bool smoke_tempo ();

	/**
	 * Toggle Config clicking off→on; verify snapshot + Config.
	 * For --smoke-click. Does not roll transport.
	 */
	bool smoke_click ();

	/** Create one more Audio track (up to kMeters). Aux send attached. */
	bool add_audio_track ();
	/** Create MIDI track if missing (ensure_midi_track). */
	bool add_midi_track ();
	/**
	 * Remove audio track at index. Keeps ≥1 audio.
	 * If require_empty_or_selected: only when empty or currently selected.
	 */
	bool remove_audio_track (int index, bool require_empty_or_selected = true);
	/** Remove selected audio, else last empty audio; keeps ≥1. */
	bool remove_selected_or_empty_audio_track ();
	/** Remove MIDI track when present (clears piano-roll state). */
	bool remove_midi_track ();

	/**
	 * Add audio 3→4→5, remove empty back; MIDI remove/add; restore ≥3.
	 * For --smoke-tracks.
	 */
	bool smoke_tracks ();

	/** Rename Session Track (fails if rec-armed or empty name). */
	bool set_track_name (int track, const std::string& name);
	bool set_midi_track_name (const std::string& name);
	/**
	 * Swap presentation_order with neighbour (delta −1 = up, +1 = down).
	 * collect_tracks re-sorts so Editor/Mixer follow Session order.
	 */
	bool move_audio_track (int index, int delta);
	bool move_audio_track_up (int index) { return move_audio_track (index, -1); }
	bool move_audio_track_down (int index) { return move_audio_track (index, +1); }

	/**
	 * Rename Audio 0 → Kick; swap 0↔1; verify names/order; restore.
	 * For --smoke-rename.
	 */
	bool smoke_rename ();

	/**
	 * Create/replace the shell Session RouteGroup ("Group 1") from audio
	 * indices (≥2). Shares mute + solo + gain + rec-enable when active.
	 * indices (≥2). Shares mute + solo + gain + color when active.
	 * indices (≥2). Shares mute + solo + gain + monitoring when active.
	 * Honest scope: one group, no VCA/subgroup.
	 */
	bool create_route_group (const int* indices, int n);
	/** Remove shell RouteGroup members (and drop empty group). */
	bool clear_route_group ();
	bool set_route_group_active (bool enable);
	bool toggle_route_group_active ();
	/** Relative (proportional) gain linking. Alt+Shift+G / --smoke-group-relative. */
	bool set_route_group_relative (bool enable);
	bool toggle_route_group_relative ();
	/** Group color (RRGGBBAA). Alt+Shift+C / --smoke-group-color. */
	bool set_route_group_rgba (uint32_t rgba);
	bool cycle_route_group_color ();

	/**
	 * Group Audio 0+1; mute/solo/gain propagate; clear. For --smoke-group.
	 */
	bool smoke_group ();
	/**
	 * Group Audio 0+1; arm propagates via UseGroup; clear isolates.
	 * For --smoke-group-recenable.
	 */
	bool smoke_group_recenable ();
/**
	 * Group; enable relative; NoGroup seed 0.40/0.20; UseGroup ×2 on Audio 1
	 * → peers 0.80/0.40. For --smoke-group-relative.
	 */
	bool smoke_group_relative ();
	/** Create group; cycle palette; restore. For --smoke-group-color. */
	bool smoke_group_color ();
/**
	 * Group Audio 0+1; input-monitor choice propagates via UseGroup; clear
	 * isolates. For --smoke-group-monitoring.
	 */
	bool smoke_group_monitoring ();

	/**
	 * Track input monitor choice (MonitorControl): Auto / Input / Disk.
	 * Uses UseGroup so RouteGroup monitoring share propagates peers.
	 */
	bool set_track_monitor_choice (int track, int choice_0_auto_1_in_2_disk);
	bool cycle_track_monitor_choice (int track = 0);

	/**
	 * Ensure Session monitor bus + MonitorProcessor (add_monitor_section).
	 * Honest GTK MonitorSection equivalent for dim / mono / cut.
	 */
	bool ensure_monitor_section ();
	bool set_monitor_dim (bool enable);
	bool toggle_monitor_dim ();
	/** Dim attenuation dB (−20..0). For Shift+click Dim / Alt+Shift+D. */
	bool set_monitor_dim_level_db (float db);
	/** Cycle −20 → −12 → −6 → −20. */
	bool cycle_monitor_dim_level ();
	bool set_monitor_mono (bool enable);
	bool toggle_monitor_mono ();
	bool set_monitor_cut (bool enable); /* mute / cut_all */
	bool toggle_monitor_cut ();
	/** Solo-boost dB (0..+10). For Boost click / Alt+Shift+K. */
	bool set_monitor_solo_boost_db (float db);
	/** Cycle 0 → +3 → +6 → 0. */
	bool cycle_monitor_solo_boost ();

	/**
	 * Ensure monitor; dim→mono→cut on then off. For --smoke-monitor.
	 */
	bool smoke_monitor ();
	/** −20 → −12 → −6 → restore −12. For --smoke-monitor-dim-level. */
	bool smoke_monitor_dim_level ();

	/**
	 * Shell mixer strip width: narrow (compact) vs equal-fill.
	 * For Alt+Shift+W / --smoke-narrow-mixer.
	 */
	bool set_mixer_narrow (bool narrow);
	bool toggle_mixer_narrow ();
	bool smoke_narrow_mixer ();
	/** 0 → +3 → +6 → restore 0. For --smoke-solo-boost. */
	bool smoke_solo_boost ();

	/** Refresh BBT + timecode clock fields from Session transport. */
	void refresh_clock_snap ();

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
	 * Bypass (or activate) every shell ACE insert: Reverb/Comp/EQ/Delay/Limiter/Gate.
	 * For Alt+B / Alt+click FX / --smoke-fx-bypass-all.
	 */
	bool set_all_fx_bypassed (bool bypassed);
	/** Ensure inserts → bypass all → verify → activate all. */
	bool smoke_fx_bypass_all ();
	/**
	 * Activate every shell ACE insert (Reverb…Gate).
	 * For Alt+Shift+B / --smoke-fx-activate-all.
	 */
	bool smoke_fx_activate_all ();

	/**
	 * Set ACE Reverb Blend (wet mix) 0..1 via PluginInsert AutomationControl.
	 * UI-thread safe Controllable::set_value.
	 */
	bool set_track_fx_param (int track, float value_0_1);
	float track_fx_param (int track = 0) const;

	/**
	 * Reset mixer FX param 1..3 on track to insert defaults (Blend 0.3, EQ 0.5, …).
	 * For double-click FX param / --smoke-fx-param-reset.
	 */
	bool reset_track_fx_param (int track, int which_1_to_3);

	/**
	 * Insert ACE Compressor (bundled LV2, DSP-only) on an audio track pre-fader.
	 * Default track 1 = Audio 2. Idempotent when already present.
	 */
	bool insert_track_comp (int track = 1);
	bool toggle_track_comp_bypass (int track = 1);
	bool set_track_comp_bypassed (int track, bool bypassed);
	/** Threshold dBFS (−60..0) or normalized 0..1 for D2D slider. */
	bool set_track_comp_threshold_db (int track, float db);
	bool set_track_comp_threshold_norm (int track, float norm_0_1);
	/** Ratio 1..20 or normalized 0..1. */
	bool set_track_comp_ratio (int track, float ratio);
	bool set_track_comp_ratio_norm (int track, float norm_0_1);
	/**
	 * Optional sidechain: enable SC pin + connect src track output → comp SC in.
	 * Falls back to SC off if pin/connect fails.
	 */
	bool set_track_comp_sidechain (int track, int src_track, bool enable);

	/**
	 * Insert ACE EQ (bundled LV2, DSP-only) on an audio track pre-fader.
	 * Default track 2 = Audio 3. Idempotent when already present.
	 * D2D exposes Gain L / Gain 2 / Gain H (−20..+20 dB).
	 */
	bool insert_track_eq (int track = 2);
	bool toggle_track_eq_bypass (int track = 2);
	bool set_track_eq_bypassed (int track, bool bypassed);
	bool set_track_eq_gain_l_db (int track, float db);
	bool set_track_eq_gain_m_db (int track, float db);
	bool set_track_eq_gain_h_db (int track, float db);
	bool set_track_eq_gain_l_norm (int track, float norm_0_1);
	bool set_track_eq_gain_m_norm (int track, float norm_0_1);
	bool set_track_eq_gain_h_norm (int track, float norm_0_1);

	/**
	 * Insert ACE Delay (bundled LV2, DSP-only) on an audio track pre-fader.
	 * Default track 3 = Audio 4. Idempotent when already present.
	 * D2D exposes Time (1..8000 ms) + Feedback (0..100 %).
	 */
	bool insert_track_delay (int track = 3);
	bool toggle_track_delay_bypass (int track = 3);
	bool set_track_delay_bypassed (int track, bool bypassed);
	bool set_track_delay_time_ms (int track, float ms);
	bool set_track_delay_feedback_pct (int track, float pct);
	bool set_track_delay_time_norm (int track, float norm_0_1);
	bool set_track_delay_feedback_norm (int track, float norm_0_1);

	/**
	 * Insert ACE Limiter (bundled LV2, DSP-only) on an audio track pre-fader.
	 * Default track 4 = Audio 5. Idempotent when already present.
	 * D2D exposes Ceiling (−20..0 dB) + Release (1..2000 ms).
	 */
	bool insert_track_limiter (int track = 4);
	bool toggle_track_limiter_bypass (int track = 4);
	bool set_track_limiter_bypassed (int track, bool bypassed);
	bool set_track_limiter_ceiling_db (int track, float db);
	bool set_track_limiter_release_ms (int track, float ms);
	bool set_track_limiter_ceiling_norm (int track, float norm_0_1);
	bool set_track_limiter_release_norm (int track, float norm_0_1);
	bool insert_track_gate (int track = 5);
	bool toggle_track_gate_bypass (int track = 5);
	bool set_track_gate_bypassed (int track, bool bypassed);
	bool set_track_gate_threshold_db (int track, float db);
	bool set_track_gate_hold_ms (int track, float ms);
	bool set_track_gate_threshold_norm (int track, float norm_0_1);
	bool set_track_gate_hold_norm (int track, float norm_0_1);

	/**
	 * Ensure ACE Reverb on Audio 1; verify on route; toggle bypass; set Blend.
	 * For --smoke-plugin. Hardware not required.
	 */
	bool smoke_plugin_insert ();
	/** Nudge Audio1 Blend → reset_track_fx_param → ~0.3. For --smoke-fx-param-reset. */
	bool smoke_fx_param_reset ();

	/**
	 * Insert ACE Comp on Audio 2; set threshold/ratio; optional SC from Audio 1.
	 * For --smoke-comp.
	 */
	bool smoke_comp ();

	/**
	 * Insert ACE EQ on Audio 3; set Gain L / Gain 2 / Gain H.
	 * For --smoke-eq.
	 */
	bool smoke_eq ();

	/**
	 * Insert ACE Delay on Audio 4; set Time / Feedback.
	 * For --smoke-delay.
	 */
	bool smoke_delay ();

	/**
	 * Insert ACE Limiter on Audio 5; set Ceiling / Release.
	 * For --smoke-limiter.
	 */
	bool smoke_limiter ();
	/**
	 * Insert ACE Gate on Audio 6; set Threshold −35 dB / Hold 80 ms.
	 * For --smoke-gate.
	 */
	bool smoke_gate ();

	/**
	 * Enable spectrum, analyze a synthetic tone (+ playlist window when available).
	 * For --smoke-spectrum.
	 */
	bool smoke_spectrum ();
	/** Toggle UI-rate spectrum analyzer (Spec / Y). */
	bool set_spectrum_enabled (bool on);
	bool toggle_spectrum_enabled ();
	bool spectrum_enabled () const { return _snap.spectrum_enabled; }
	/** false = selected/Audio1; true = mix of audio tracks (master view). */
	bool set_spectrum_master (bool master);
	bool toggle_spectrum_master ();

	/**
	 * Enable phase correlation; verify mono (+1) and anti-phase (−1) synthetics.
	 * For --smoke-correlation.
	 */
	bool smoke_correlation ();
	/** Toggle UI-rate phase / goniometer (Phase / X). */
	bool set_correlation_enabled (bool on);
	bool toggle_correlation_enabled ();
	bool correlation_enabled () const { return _snap.correlation_enabled; }
	bool set_correlation_master (bool master);
	bool toggle_correlation_master ();

	/**
	 * Enable approx LUFS; verify synthetic tone yields plausible momentary.
	 * For --smoke-loudness.
	 */
	bool smoke_loudness ();
	/** Toggle UI-rate approximate LUFS (LUFS / J). */
	bool set_loudness_enabled (bool on);
	bool toggle_loudness_enabled ();
	bool loudness_enabled () const { return _snap.loudness_enabled; }
	bool set_loudness_master (bool master);
	bool toggle_loudness_master ();

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

	/**
	 * Quantize selected MIDI notes (or all notes if none selected) to the
	 * current snap grid density. Forces grid math even when Snap UI is off.
	 * Rebuilds the MIDI region (Windows-safe; no NoteDiff WriterLock).
	 */
	bool quantize_midi_notes ();

	/**
	 * Transpose selected MIDI notes (or all if none selected) by semitones.
	 * Pitch clamped to 0..127. Rebuilds the MIDI region (Windows-safe).
	 */
	bool transpose_midi_notes (int semitones);

	/**
	 * Nudge velocity of selected notes (or all if none selected) by delta.
	 * Clamped 1..127. Rebuilds the MIDI region (Windows-safe).
	 */
	bool nudge_midi_velocity (int delta);
	/** Set absolute velocity for selected notes (or all if none selected). */
	bool set_midi_velocity (int velocity);

	/**
	 * Nudge length of selected notes (or all if none selected) by delta_sec.
	 * Min length 0.05s. When snap is on, ends are snapped after nudge.
	 * Rebuilds the MIDI region (Windows-safe).
	 */
	bool nudge_midi_length (float delta_sec);
	/** Trim start of selected note(s); snaps when snap is on. Min length 0.05s. */
	bool set_midi_note_start (float start_sec);
	/** Trim end of selected note(s); snaps when snap is on. Min length 0.05s. */
	bool set_midi_note_end (float end_sec);
	/** Grid unit in seconds for current snap density (120 BPM fallback). */
	float snap_unit_sec () const;

	/**
	 * Copy selected MIDI notes to the in-app clipboard (relative timing kept).
	 * Returns false if nothing is selected.
	 */
	bool copy_selected_midi_notes ();
	/** Copy then remove selected notes (Windows-safe rebuild). */
	bool cut_selected_midi_notes ();
	/**
	 * Paste clipboard notes. at_sec < 0 → transport playhead.
	 * Cluster earliest note anchors at at_sec; snaps origin when Snap is on.
	 */
	bool paste_midi_notes (float at_sec = -1.f);
	bool has_midi_clipboard () const;

	/**
	 * Humanize selected notes (or all if none selected): bounded random
	 * timing (±20ms @ strength 1) and velocity (±8 @ strength 1) jitter.
	 * strength clamped to 0.1..2. seed 0 → nondeterministic; else fixed.
	 * Rebuilds the MIDI region (Windows-safe).
	 */
	bool humanize_midi_notes (float strength = 1.f, uint32_t seed = 0);

	/** Step-record mode: computer keys / API place notes at playhead. */
	void set_midi_step_enabled (bool on);
	bool midi_step_enabled () const;
	bool toggle_midi_step_enabled ();
	/**
	 * Chord quality for step/click: 0=off (single pitch), 1=major triad,
	 * 2=minor triad. Cycle via UI / F5. Honest: only maj/min triads — no
	 * 7ths, inversions, or scale-aware voicings.
	 */
	void set_midi_chord_quality (int quality);
	int  midi_chord_quality () const;
	int  cycle_midi_chord_quality ();
	/**
	 * Scale lock for step/click: when on, the input root is snapped to the
	 * nearest degree of major/natural-minor with midi_scale_root (0=C..11=B).
	 * Chord mode still builds fixed maj/min intervals from that snapped root
	 * (chord tones may leave the scale). Does not rewrite existing notes.
	 */
	void set_midi_scale_lock (bool on);
	bool midi_scale_lock () const;
	bool toggle_midi_scale_lock ();
	void set_midi_scale_kind (int kind); /* 0=major 1=minor */
	int  midi_scale_kind () const;
	int  cycle_midi_scale_kind ();
	void set_midi_scale_root (int root); /* 0..11 */
	int  midi_scale_root () const;
	int  cycle_midi_scale_root ();
	/** Snap pitch to nearest scale degree when lock is on; else unchanged. */
	uint8_t snap_pitch_to_scale (uint8_t pitch) const;
	/**
	 * Place a note (or maj/min triad when chord quality ≠ off) at the
	 * transport playhead (length ≈ snap grid unit × 0.9). When advance is
	 * true, move playhead one grid step after placing. advance=false supports
	 * multi-key latch (place several pitches at the same time, advance later).
	 * WinMME hardware note-on is not polled here.
	 */
	bool step_input_midi_note (uint8_t pitch, uint8_t velocity = 100,
	                           bool advance = true);
	/** Advance (or retreat) playhead by one snap grid step without a note. */
	bool step_advance (int steps = 1);

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

	/**
	 * Add off-grid notes, quantize to beat grid, verify starts snap.
	 * For --smoke-midi-quantize.
	 */
	bool smoke_midi_quantize ();

	/**
	 * Add notes, transpose +1 then −12, verify pitches. For --smoke-midi-transpose.
	 */
	bool smoke_midi_transpose ();

	/**
	 * Add note, nudge velocity −20, set 64, verify. For --smoke-midi-velocity.
	 */
	bool smoke_midi_velocity ();

	/**
	 * Add fixed-length note, trim end, nudge length, verify. For --smoke-midi-length.
	 */
	bool smoke_midi_length ();

	/**
	 * Add note, copy, paste at 1s, cut paste, verify. For --smoke-midi-clipboard.
	 */
	bool smoke_midi_clipboard ();

	/**
	 * Duplicate selected MIDI notes immediately after the selection span
	 * (copy → paste at selection_end). Leaves originals; selects first new note.
	 * For Ctrl+D / --smoke-midi-duplicate.
	 */
	bool duplicate_selected_midi_notes ();
	/** Add note @0, duplicate → note @length, verify count=2. For --smoke-midi-duplicate. */
	bool smoke_midi_duplicate ();

	/**
	 * Add notes, humanize with fixed seed, verify bounded jitter.
	 * For --smoke-midi-humanize.
	 */
	bool smoke_midi_humanize ();

	/**
	 * Enable step, place two notes via step_input, verify positions + advance.
	 * For --smoke-midi-step.
	 */
	bool smoke_midi_step ();

	/**
	 * Maj triad @0, min triad @beat, Off multi-place without mid-advance.
	 * For --smoke-midi-chord.
	 */
	bool smoke_midi_chord ();

	/**
	 * Live arpeggiator: injects note-on/off into MidiTrack (Reasonable Synth)
	 * at snap-grid rate while transport rolls. Pitch source: shell-held keys,
	 * else notes sharing selected start (chord cluster), else notes under
	 * playhead, else region pitches / default triad. Modes: Up / Down / UpDown.
	 */
	void set_midi_arp_enabled (bool on);
	bool midi_arp_enabled () const;
	bool toggle_midi_arp_enabled ();
	void set_midi_arp_mode (int mode); /* 0=up 1=down 2=up-down */
	int  midi_arp_mode () const;
	int  cycle_midi_arp_mode ();
	/** Optional held pitches from computer-key chord (cleared when count=0). */
	void set_midi_arp_held_pitches (const uint8_t* pitches, int count);
	void clear_midi_arp_held_pitches ();
	/**
	 * Advance one arp step (force=true ignores transport/grid). Used by poll
	 * and --smoke-midi-arp.
	 */
	bool tick_arpeggiator (bool force = false);

	/**
	 * Build chord pitches, arp Up then Down sequences via forced ticks.
	 * For --smoke-midi-arp.
	 */
	bool smoke_midi_arp ();

	/**
	 * C-major lock snaps C#→D; chord maj from snapped root; lock off passthrough.
	 * For --smoke-midi-scale.
	 */
	bool smoke_midi_scale ();

	/**
	 * Preview a pitch through MidiTrack immediate events (Reasonable Synth).
	 * Note-on now; auto note-off ~180ms later via poll. For click audition.
	 */
	bool audition_midi_note (uint8_t pitch, uint8_t velocity = 100);
	/** Force note-off for any pending audition. */
	void silence_midi_audition ();
	/** Add note → select/audition → verify immediate path. --smoke-midi-audition */
	bool smoke_midi_audition ();

	/**
	 * MIDI panic: sustain-off + All Notes Off + Reset Controllers on every
	 * channel of every MidiTrack (Session::midi_panic). Also silences shell arp.
	 * For F9 / Panic button / --smoke-midi-panic.
	 */
	bool midi_panic ();
	/** Ensure MidiTrack, panic, verify immediate events queued. */
	bool smoke_midi_panic ();
	/** Seed note-on → transport_stop → verify Session::midi_panic queued. */
	bool smoke_stop_midi_panic ();

	/**
	 * MIDI bank (CC0 MSB + CC32 LSB=0) + Program Change via immediate events.
	 * Values clamped 0..127. For Pgm± / Bank± / --smoke-midi-program.
	 */
	bool set_midi_program (int program);
	bool nudge_midi_program (int delta);
	int  midi_program () const;
	bool set_midi_bank (int bank);
	bool nudge_midi_bank (int delta);
	int  midi_bank () const;
	/** Apply current bank+program (or explicit args) to MidiTrack. */
	bool send_midi_program_change (int program = -1, int bank = -1);
	/** Set bank+program, verify immediate writes. For --smoke-midi-program. */
	bool smoke_midi_program ();

	/* smoke_midi_cc declared with MIDI CC automation APIs above. */

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
	void refresh_freeze_flags ();
	void refresh_record_flags ();
	void refresh_mute_solo_flags ();
	void refresh_gain_auto_state_flags ();
	void refresh_solo_isolate_flags ();
	void refresh_solo_safe_flags ();
	void refresh_track_comment_flags ();
	void refresh_phase_invert_flags ();
	void refresh_trim_flags ();
	void refresh_listen_mode_flags ();
	void refresh_monitor_choice_flags ();
	bool ensure_freeze_session_range (int track);
	void refresh_pan_flags ();
	void refresh_pan_auto_state_flags ();
	void refresh_send_flags ();
	void refresh_send_auto_state_flags ();
	void refresh_group_flags ();
	void refresh_monitor_flags ();
	bool ensure_aux_sends ();
	void refresh_history_flags ();
	void refresh_selection_snap ();
	void refresh_locations_snap ();
	void refresh_tempo_snap ();
	void collect_tracks ();
	void update_session_path_snap ();
	bool compute_export_range (int64_t* start_sample, int64_t* end_sample) const;
	bool configure_backend_midi ();
	void refresh_midi_notes_snap ();
	void rebuild_arp_pitches ();
	int  arp_pitch_at_step (int step) const;
	bool midi_immediate_note (uint8_t status, uint8_t pitch, uint8_t velocity);
	void arp_silence ();
	void refresh_track_fx_snap ();
	void refresh_track_comp_snap ();
	void refresh_track_eq_snap ();
	void refresh_track_delay_snap ();
	void refresh_track_limiter_snap ();
	void refresh_track_gate_snap ();
	void refresh_spectrum_snap ();
	void refresh_correlation_snap ();
	void refresh_loudness_snap ();
	bool ensure_plugins_scanned ();

	bool        _started;
	std::string _backend_name;
	std::string _last_export_path;
	uint32_t    _period_frames; /* 0 = Auto */
	std::string _pref_backend; /* Dummy / WASAPI — next start if CLI omits --backend */
	uint32_t    _pref_period_frames;
	bool        _pref_snap_default;
	bool        _pref_click_default;
	struct RecentEntry {
		std::string path;
		std::string snapshot;
	};
	std::vector<RecentEntry> _recent;
	void refresh_recent_snap ();
	bool        _autosave_enabled;
	uint32_t    _last_autosave_ms;
	std::string _recovery_path; /* original session dir in marker */
	std::string _recovery_snapshot;
	uint64_t    _recovery_saved_ft;
	uint64_t    _recovery_last_open_ft;
	bool        _recovery_dirty;
	void load_recovery_marker ();
	bool save_recovery_marker ();
	std::string recovery_copy_path () const;
	bool copy_snapshot_to_recovery ();
	uint64_t snapshot_filetime (const std::string& session_dir, const std::string& snap) const;
	float       _gain;
	float       _track_gain[EngineSnapshot::kMeters];
	EngineSnapshot _snap;

	struct MidiClipNote {
		uint8_t pitch;
		uint8_t velocity;
		float   start_rel_sec; /* relative to earliest copied note */
		float   length_sec;
	};
	std::vector<MidiClipNote> _midi_clipboard;

	static constexpr int kMaxArpPitches = 16;
	bool    _arp_enabled;
	int     _arp_mode; /* 0=up 1=down 2=up-down */
	int     _arp_step;
	int     _arp_last_grid;
	void tick_midi_audition ();
	int     _audition_pitch; /* −1 = silent */
	std::chrono::steady_clock::time_point _audition_off_at;
	int     _arp_sounding; /* −1 = silent */
	uint8_t _arp_pitches[kMaxArpPitches];
	int     _arp_pitch_count;
	uint8_t _arp_held[kMaxArpPitches];
	int     _arp_held_count;

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE
	class Impl;
	Impl* _impl;
#endif
};
