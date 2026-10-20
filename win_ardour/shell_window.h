/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Win32 + Direct2D + DirectWrite shell: transport, multi-track editor,
 * mixer faders + meters, record, import, undo, loop, markers.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <string>

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include "about_panel.h"
#include "automation_lane.h"
#include "engine_host.h"
#include "history_panel.h"
#include "mixer_strip.h"
#include "prefs_panel.h"
#include "recent_panel.h"
#include "recovery_panel.h"
#include "monitor_strip.h"
#include "correlation_panel.h"
#include "loudness_panel.h"
#include "spectrum_panel.h"
#include "transport_bar.h"
#include "wave_strip.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "comdlg32.lib")

class ShellWindow
{
public:
	static constexpr int kTracksMax = 6; /* matches EngineSnapshot::kMeters */
	static constexpr int kMixerStripsMax = 7; /* up to 6 audio + 1 MIDI */
	/* Legacy aliases used by older loops — prefer n_audio_lanes(). */
	static constexpr int kTracks = kTracksMax;
	static constexpr int kMixerStrips = kMixerStripsMax;

	ShellWindow ();
	~ShellWindow ();

	bool create (HINSTANCE instance, int show_cmd);
	/** Show brief branded splash (ignored if already dismissed / --no-splash). */
	void show_splash ();
	void dismiss_splash ();
	bool splash_visible () const { return _about.splash_visible (); }
	/**
	 * When smoke_timeout_secs > 0, create Session/PeakMeter but do not auto-roll
	 * (avoids intermittent AVs under Dummy/WASAPI during unattended --timeout).
	 * smoke_* flags exercise paths without changing the plain --timeout
	 * exit-0 contract when those flags are off.
	 */
	bool attach_engine (const std::string& backend_name,
	                    unsigned smoke_timeout_secs = 0,
	                    bool smoke_import = false,
	                    bool smoke_record = false,
	                    bool smoke_save_load = false,
	                    bool smoke_export = false,
	                    bool smoke_undo = false,
	                    bool smoke_loop = false,
	                    bool smoke_midi = false,
	                    bool smoke_midi_edit = false,
	                    bool smoke_plugin = false,
	                    bool smoke_perf = false,
	                    const std::string& open_session_path = std::string (),
	                    unsigned period_frames = 0,
	                    bool smoke_vst3 = false,
	                    bool smoke_mute_solo = false,
	                    bool smoke_pan = false,
	                    bool smoke_send = false,
	                    bool smoke_automation = false,
	                    bool smoke_edit = false,
	                    bool smoke_snap = false,
	                    bool smoke_punch = false,
	                    bool smoke_tempo = false,
	                    bool smoke_click = false,
	                    bool smoke_tracks = false,
	                    bool smoke_rename = false,
	                    bool smoke_group = false,
	                    bool smoke_monitor = false,
	                    bool smoke_locate = false,
	                    bool smoke_section_rename = false,
	                    bool smoke_zoom = false,
	                    bool smoke_history = false,
	                    bool smoke_prefs = false,
	                    bool smoke_recent = false,
	                    bool smoke_recovery = false,
	                    bool smoke_stretch = false,
	                    bool smoke_pitch = false,
	                    bool smoke_reverse = false,
	                    bool smoke_normalize = false,
	                    bool smoke_strip_silence = false,
	                    bool smoke_consolidate = false,
	                    bool smoke_freeze = false,
	                    bool smoke_comp = false,
	                    bool smoke_eq = false,
	                    bool smoke_delay = false,
	                    bool smoke_limiter = false,
	                    bool smoke_gate = false,
	                    bool smoke_spectrum = false,
	                    bool smoke_correlation = false,
	                    bool smoke_loudness = false,
	                    bool smoke_midi_quantize = false,
	                    bool smoke_midi_transpose = false,
	                    bool smoke_midi_velocity = false,
	                    bool smoke_midi_length = false,
	                    bool smoke_midi_clipboard = false,
	                    bool smoke_midi_humanize = false,
	                    bool smoke_midi_step = false,
	                    bool smoke_midi_chord = false,
	                    bool smoke_midi_arp = false,
	                    bool smoke_midi_scale = false,
	                    bool smoke_midi_cc = false,
	                    bool smoke_midi_bend = false,
	                    bool smoke_midi_pressure = false);
	int  run_message_loop (unsigned timeout_secs);

	HWND hwnd () const { return _hwnd; }

private:
	enum class DragMode {
		None,
		WavePending, /* click may become select or pan */
		WavePan,
		Fader,
		FxParam, /* drag Mixer FX param 1 (Blend / Comp thr / EQ L / Delay time / Limiter ceil / Gate thr) */
		FxParam2, /* drag Mixer FX param 2 (Comp ratio / EQ mid / Delay fb / Limiter release / Gate hold) */
		FxParam3, /* drag Mixer FX param 3 (EQ High) */
		Pan, /* drag Mixer pan azimuth */
		Send, /* drag Mixer aux send level */
		AutoWrite, /* click/drag Editor automation lane */
		MidiCcWrite, /* click/drag MIDI CC7 automation lane */
		FadeIn, /* drag selection fade-in handle */
		FadeOut, /* drag selection fade-out handle */
		TrimFront, /* drag selected region left edge */
		TrimEnd, /* drag selected region right edge */
		RegionMovePending, /* click on selection body; may become move */
		RegionMove, /* drag selected region body */
		MidiDraw, /* click/drag on MIDI piano-roll to create a note */
		MidiVelocity, /* drag velocity lane → set selected/all note velocity */
		MidiTrimStart, /* drag note left edge → trim start */
		MidiTrimEnd, /* drag note right edge → trim end */
		TrackReorderPending, /* mixer label click; may become reorder drag */
		TrackReorder, /* drag mixer strip label to reorder */
		RulerScrub, /* drag editor ruler → continuous locate */
		Shuttle /* drag transport Scrub strip → varispeed */
	};

	static constexpr uint8_t kMidiPitchLo = 48; /* C3 */
	static constexpr uint8_t kMidiPitchHi = 71; /* B4 */
	static constexpr int     kMidiPitchRows = (int)(kMidiPitchHi - kMidiPitchLo + 1);

	enum class SmokeRecordPhase {
		Off,
		NeedArm,
		ArmWait,
		Rolling,
		FinalizeWait,
		Done
	};

	struct EditorLane {
		WaveStrip   strip;
		D2D1_RECT_F bounds;
		wchar_t     label[32];
	};

	static LRESULT CALLBACK wnd_proc (HWND, UINT, WPARAM, LPARAM);
	LRESULT handle_message (UINT msg, WPARAM wparam, LPARAM lparam);

	bool init_d2d ();
	void discard_device_resources ();
	bool ensure_device_resources ();
	void on_resize (UINT width, UINT height);
	void on_dpi_changed (UINT dpi, const RECT* suggested);
	void on_paint ();
	void on_tick ();
	void layout_panes ();
	void draw_editor_pane ();
	void draw_automation_lane ();
	void refresh_automation_lane ();
	void write_automation_at (float x, float y);
	void draw_midi_cc_lane ();
	void refresh_midi_cc_lane ();
	void write_midi_cc_at (float x, float y);
	void cycle_midi_cc_controller ();
	const wchar_t* midi_cc_label () const;
	void draw_mixer_pane ();
	void draw_help_overlay ();
	void toggle_help ();
	void toggle_prefs ();
	void toggle_about ();
	void sync_prefs_from_engine ();
	void apply_prefs_hit (PrefsHit hit);
	void transport_play ();
	void transport_stop ();
	void transport_toggle ();
	void transport_toggle_record ();
	void prompt_import_audio ();
	void do_save_session ();
	void prompt_open_session ();
	void toggle_recent_menu ();
	void sync_recent_from_engine ();
	void apply_recent_hit (RecentHit hit);
	void open_recent_at (int index);
	void maybe_show_recovery_prompt ();
	void apply_recovery_hit (RecoveryHit hit);
	void prompt_export_wav ();
	void do_undo ();
	void do_redo ();
	void do_undo_to_index (int index);
	void sync_history_from_engine ();
	void do_delete_selection ();
	void do_split_at_playhead ();
	void do_apply_crossfade ();
	void do_stretch_selection (float time_ratio = 1.5f);
	void do_pitch_shift_selection (float semitones = 1.f);
	void do_reverse_selection ();
	void do_normalize_selection (float target_dbfs = -1.f);
	void do_strip_silence_selection ();
	void do_consolidate_selection ();
	void do_toggle_freeze_track ();
	void sync_transport_freeze_from_engine ();
	void do_toggle_snap ();
	void do_cycle_snap_density ();
	void do_quantize_midi_notes ();
	void do_transpose_midi_notes (int semitones);
	void do_nudge_midi_velocity (int delta);
	void do_nudge_midi_length (float delta_sec);
	void do_copy_midi_notes ();
	void do_cut_midi_notes ();
	void do_paste_midi_notes ();
	void do_humanize_midi_notes (float strength = 1.f);
	void do_toggle_midi_step ();
	void do_cycle_midi_chord ();
	void do_toggle_midi_arp ();
	void do_cycle_midi_arp_mode ();
	void do_toggle_midi_scale ();
	void do_cycle_midi_scale_kind ();
	void do_cycle_midi_scale_root ();
	void sync_scale_to_transport ();
	void sync_arp_held_from_step_keys ();
	void do_step_input_pitch (uint8_t pitch, bool advance = true);
	void do_step_piano_key_down (uint8_t pitch);
	void do_step_piano_key_up (uint8_t pitch);
	void clear_step_piano_held ();
	/** Map computer key → MIDI pitch when step-record is on; −1 if not a piano key. */
	int  step_piano_pitch_for_key (WPARAM wparam) const;
	void do_toggle_loop ();
	void do_toggle_punch ();
	void do_toggle_count_in ();
	void do_toggle_click ();
	void do_nudge_tempo (float delta);
	void do_add_audio_track ();
	void do_add_midi_track ();
	void do_remove_track ();
	void do_move_audio_track (int delta);
	void prompt_rename_track (int audio_index, bool midi);
	void do_rename_focused_track ();
	void do_toggle_group_pick (int audio_index);
	void do_apply_or_clear_group ();
	void do_toggle_monitor_dim ();
	void do_toggle_monitor_mono ();
	void do_toggle_monitor_cut ();
	void sync_monitor_from_engine ();
	int  n_audio_lanes () const;
	int  n_mixer_strips () const;
	int  midi_mixer_index () const;
	void sync_track_labels_from_engine ();
	void sync_mixer_gains_from_engine ();
	void sync_mixer_group_from_engine ();
	void draw_editor_grid ();
	void draw_punch_overlay ();
	void do_add_marker_at_playhead ();
	void do_remove_marker_at_playhead ();
	void do_rename_section_at_playhead ();
	void sync_mixer_arm_from_engine ();
	void sync_mixer_mute_solo_from_engine ();
	void sync_mixer_pan_from_engine ();
	void sync_mixer_send_from_engine ();
	void sync_mixer_fx_from_engine ();
	void sync_transport_loop_from_engine ();
	void sync_transport_punch_from_engine ();
	void sync_transport_tempo_from_engine ();
	void sync_transport_clock_from_engine ();
	void apply_shared_view (int source_lane);
	void reset_shared_view ();
	void fit_shared_range (float start_sec, float end_sec);
	void do_fit_session ();
	void do_fit_selection ();
	void do_fit_all_regions ();
	void do_scroll_editor (float delta_widths);
	void refresh_editor_view_after_nav ();
	void apply_region_selection_to_lanes ();
	void apply_loop_to_lanes ();
	void draw_editor_ruler ();
	void draw_midi_lane ();
	bool hit_editor_ruler (float x, float y) const;
	bool hit_midi_lane (float x, float y) const;
	bool hit_midi_velocity_lane (float x, float y) const;
	D2D1_RECT_F midi_piano_rect () const;
	D2D1_RECT_F midi_velocity_rect () const;
	int  midi_velocity_at_y (float y) const;
	uint8_t midi_pitch_at_y (float y) const;
	float midi_time_at_x (float x) const;
	int  hit_editor_lane (float x, float y) const;
	int  hit_mixer_fader (float x, float y) const;
	int  hit_mixer_arm (float x, float y) const;
	int  hit_mixer_mute (float x, float y) const;
	int  hit_mixer_solo (float x, float y) const;
	int  hit_mixer_pan (float x, float y) const;
	int  hit_mixer_send (float x, float y) const;
	int  hit_mixer_meter (float x, float y) const;
	int  hit_mixer_fx (float x, float y) const;
	int  hit_mixer_fx_param (float x, float y) const;
	int  hit_mixer_fx_param2 (float x, float y) const;
	int  hit_mixer_fx_param3 (float x, float y) const;
	int  hit_mixer_label (float x, float y) const;
	int  hit_editor_lane_label (float x, float y) const;
	void client_from_lparam (LPARAM lparam, float* out_x, float* out_y) const;
	bool on_mouse_wheel (WPARAM wparam, LPARAM lparam);
	bool on_mouse_down (LPARAM lparam);
	bool on_mouse_move (LPARAM lparam);
	void on_mouse_up ();
	float stub_meter_peak (int track, float dt) const;
	void tick_smoke_record (DWORD now);

	HWND _hwnd;
	UINT _width;
	UINT _height;
	UINT _dpi;
	float _dpi_scale;
	bool _running;
	DWORD _start_tick;
	unsigned _timeout_secs;
	DragMode _drag;
	int _drag_index;
	int _focus_audio_track; /* last mixer/editor label click for rename/reorder */
	bool _group_pick[kTracksMax]; /* Shift+click candidates for Grp */
	float _pan_last_x;
	float _pan_down_x;
	float _pan_down_y;
	float _region_move_grab_offset_sec; /* pointer time − region start at grab */
	float _midi_draw_start_sec;
	uint8_t _midi_draw_pitch;
	int     _midi_vel_drag; /* live preview 1..127 while MidiVelocity drag */
	float   _midi_trim_start_sec; /* live preview while MidiTrim* drag */
	float   _midi_trim_end_sec;
	bool    _step_piano_held[128]; /* multi-key latch while step-record on */
	int     _step_piano_held_count;

	bool  _transport_playing;
	float _playhead_sec;
	DWORD _last_tick_ms;

	SmokeRecordPhase _smoke_record;
	DWORD            _smoke_record_start_ms;
	bool             _smoke_failed; /* any --smoke-* reported FAILED */
	bool             _help_visible;
	bool             _prefs_visible;
	DWORD            _splash_shown_ms; /* 0 = no auto-dismiss timer */

	D2D1_RECT_F _brand_bar;
	D2D1_RECT_F _transport_bounds;
	D2D1_RECT_F _editor_pane;
	D2D1_RECT_F _editor_ruler;
	D2D1_RECT_F _midi_lane;
	D2D1_RECT_F _auto_lane_bounds;
	D2D1_RECT_F _midi_cc_lane_bounds;
	D2D1_RECT_F _mixer_pane;
	D2D1_RECT_F _status_bar;

	ID2D1Factory*             _d2d_factory;
	IDWriteFactory*           _dwrite_factory;
	ID2D1HwndRenderTarget*    _rt;
	ID2D1SolidColorBrush*     _brush_brand;
	ID2D1SolidColorBrush*     _brush_body;
	ID2D1SolidColorBrush*     _brush_accent;
	ID2D1SolidColorBrush*     _brush_pane;
	ID2D1SolidColorBrush*     _brush_pane_edge;
	ID2D1SolidColorBrush*     _brush_peak;
	ID2D1SolidColorBrush*     _brush_rms;
	ID2D1SolidColorBrush*     _brush_playhead;
	ID2D1SolidColorBrush*     _brush_selection;
	ID2D1SolidColorBrush*     _brush_loop;
	ID2D1SolidColorBrush*     _brush_punch;
	ID2D1SolidColorBrush*     _brush_marker;
	ID2D1SolidColorBrush*     _brush_midi;
	ID2D1SolidColorBrush*     _brush_meter_hot;
	ID2D1LinearGradientBrush* _brush_bg;
	IDWriteTextFormat*        _fmt_brand;
	IDWriteTextFormat*        _fmt_body;
	IDWriteTextFormat*        _fmt_meta;
	IDWriteTextFormat*        _fmt_pane;

	float _pulse;
	EngineHost     _engine;
	EngineSnapshot _engine_snap;
	TransportBar   _transport;
	EditorLane     _lanes[kTracks];
	AutomationLane _auto_lane;
	AutomationLane _midi_cc_lane;
	MixerStrip     _mixers[kMixerStrips];
	MonitorStrip   _monitor;
	SpectrumPanel     _spectrum;
	D2D1_RECT_F       _spectrum_bounds;
	CorrelationPanel  _correlation;
	D2D1_RECT_F       _correlation_bounds;
	LoudnessPanel     _loudness;
	D2D1_RECT_F       _loudness_bounds;
	HistoryPanel   _history;
	PrefsPanel     _prefs;
	AboutPanel     _about;
	RecentPanel    _recent;
	RecoveryPanel  _recovery;
	float          _auto_last_write_sec;
	float          _midi_cc_last_write_sec;
	int            _midi_cc_number; /* 7 Vol / 1 Mod / 11 Expr / -1 Bend / -2 Pressure */
};
