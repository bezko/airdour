/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Direct2D transport: grouped primary transport, edit/MIDI rows, a responsive
 * clock + shuttle strip, file actions and a popup overflow menu.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <vector>

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include "ui_draw.h"

enum class TransportHit {
	None,
	Overflow,
	Play,
	Stop,
	Rec,
	Import,
	Save,
	Open,
	Export,
	Loop,
	Punch,
	Mark,
	Split,
	Fade,
	Stretch,
	PitchUp,
	PitchDown,
	Reverse,
	Normalize,
	StripSilence,
	Consolidate,
	Freeze,
	Snap,
	Quantize,
	MidiTransUp,
	MidiTransDown,
	MidiVelUp,
	MidiVelDown,
	MidiLenUp,
	MidiLenDown,
	MidiHumanize,
	MidiStep,
	MidiChord,
	MidiArp,
	MidiScale,
	MidiPanic,
	MidiPgmUp,
	MidiPgmDown,
	MidiBankUp,
	MidiBankDown,
	CountIn,
	Click,
	TempoDown,
	TempoUp,
	AddAudio,
	AddMidi,
	RemoveTrack,
	TrackUp,
	TrackDown,
	Group,
	Shuttle,
	FitSession,
	FitSelection,
	FitRegions,
	Help,
	Prefs,
	About,
	Spectrum,
	Phase,
	Loudness
};

class TransportBar
{
public:
	TransportBar ();

	void layout (const D2D1_RECT_F& bounds, float dpi_scale,
	             float viewport_bottom = 0.f);

	void set_playing (bool playing) { _playing = playing; }
	bool playing () const { return _playing; }

	void set_recording (bool recording) { _recording = recording; }
	bool recording () const { return _recording; }

	void set_record_armed (bool armed) { _record_armed = armed; }
	bool record_armed () const { return _record_armed; }

	void set_loop_playing (bool on) { _loop_playing = on; }
	bool loop_playing () const { return _loop_playing; }

	void set_loop_active (bool on) { _loop_active = on; }
	bool loop_active () const { return _loop_active; }

	void set_punch_enabled (bool on) { _punch_enabled = on; }
	bool punch_enabled () const { return _punch_enabled; }

	void set_snap_enabled (bool on) { _snap_enabled = on; }
	bool snap_enabled () const { return _snap_enabled; }

	/** 0=bar 1=beat 2=1/2 3=1/4 — short glyph on Snap btn when enabled. */
	void set_snap_density (int density_0_to_3);
	int  snap_density () const { return _snap_density; }
	/** Button caption: "Snap" when off, else Bar/Beat/1/2/1/4. */
	const wchar_t* snap_btn_label () const;
	bool snap_btn_center (float* out_x, float* out_y) const;

	void set_midi_step_enabled (bool on) { _midi_step_enabled = on; }
	bool midi_step_enabled () const { return _midi_step_enabled; }

	/** 0=off 1=maj 2=min — Chord button label + lit when ≠0. */
	void set_midi_chord_quality (int q) { _midi_chord_quality = q; }
	int  midi_chord_quality () const { return _midi_chord_quality; }

	/** Arp: lit when enabled; label Up/Dn/UD from mode. */
	void set_midi_arp_enabled (bool on) { _midi_arp_enabled = on; }
	bool midi_arp_enabled () const { return _midi_arp_enabled; }
	void set_midi_arp_mode (int mode) { _midi_arp_mode = mode; }
	int  midi_arp_mode () const { return _midi_arp_mode; }

	/** Scale lock: lit when on; label shows root (+m for minor). */
	void set_midi_scale_lock (bool on) { _midi_scale_lock = on; }
	bool midi_scale_lock () const { return _midi_scale_lock; }
	void set_midi_scale_kind (int kind) { _midi_scale_kind = kind; }
	int  midi_scale_kind () const { return _midi_scale_kind; }
	void set_midi_scale_root (int root) { _midi_scale_root = root; }
	int  midi_scale_root () const { return _midi_scale_root; }

	void set_freeze_active (bool on) { _freeze_active = on; }
	bool freeze_active () const { return _freeze_active; }

	void set_count_in_enabled (bool on) { _count_in_enabled = on; }
	bool count_in_enabled () const { return _count_in_enabled; }

	void set_click_enabled (bool on) { _click_enabled = on; }
	bool click_enabled () const { return _click_enabled; }
	/** Linear click gain (1.0 = unity); used for Click button caption. */
	void set_click_gain (float g) { _click_gain = g; }
	float click_gain () const { return _click_gain; }

	void set_tempo_bpm (float bpm) { _tempo_bpm = bpm; }
	float tempo_bpm () const { return _tempo_bpm; }

	void set_group_active (bool on) { _group_active = on; }
	bool group_active () const { return _group_active; }
	void set_group_relative (bool on) { _group_relative = on; }
	bool group_relative () const { return _group_relative; }

	void set_spectrum_enabled (bool on) { _spectrum_enabled = on; }
	bool spectrum_enabled () const { return _spectrum_enabled; }

	void set_correlation_enabled (bool on) { _correlation_enabled = on; }
	bool correlation_enabled () const { return _correlation_enabled; }

	void set_loudness_enabled (bool on) { _loudness_enabled = on; }
	bool loudness_enabled () const { return _loudness_enabled; }

	/** Audio callback DSP load 0..100 (WASAPI / AudioEngine). */
	void set_dsp_load_pct (float pct) { _dsp_load_pct = pct; }
	float dsp_load_pct () const { return _dsp_load_pct; }

	void set_clock_bbt (int bars, int beats, int ticks)
	{
		_bbt_bars = bars;
		_bbt_beats = beats;
		_bbt_ticks = ticks;
	}

	void set_clock_timecode (int h, int m, int s, int f)
	{
		_tc_hours = h;
		_tc_minutes = m;
		_tc_seconds = s;
		_tc_frames = f;
	}

	/** Shuttle knob visual −1..+1 (0 = stopped / center). */
	void set_shuttle_norm (float n) { _shuttle_norm = n; }
	float shuttle_norm () const { return _shuttle_norm; }

	/** Map pointer x on shuttle strip to speed −8..+8 (center deadzone). */
	float shuttle_speed_at_x (float x) const;

	TransportHit hit_test (float x, float y) const;

	/** Pointer feedback (hover highlight, pressed look, tooltip text). */
	bool update_hover (float x, float y);
	bool clear_hover ();
	void set_pressed (TransportHit hit) { _pressed = hit; }
	void clear_pressed () { _pressed = TransportHit::None; }
	TransportHit hovered_hit () const { return _hover; }
	bool hover_tooltip (wchar_t* buf, size_t n) const;

	/** Popup overflow menu state (owned by the bar, driven by the shell). */
	bool overflow_open () const { return _overflow_open; }
	void set_overflow_open (bool on);
	bool menu_contains (float x, float y) const;

	void draw (UiDraw& ui, float playhead_sec) const;
	/** Popup layer; call after the rest of the window so the menu sits on top. */
	void draw_overlay (UiDraw& ui) const;

	const D2D1_RECT_F& bounds () const { return _bounds; }
	const D2D1_RECT_F& shuttle_btn () const { return _shuttle_btn; }
	const D2D1_RECT_F& open_btn () const { return _open_btn; }
	const D2D1_RECT_F& overflow_btn () const { return _overflow_btn; }

	/** Number of action rows laid out (2 when the compact edit/MIDI rows fit). */
	int rows () const { return _rows.empty () ? 1 : 2; }
	size_t file_button_count () const { return _files.size (); }
	bool file_button_rect (size_t index, D2D1_RECT_F* out) const;

private:
	enum class Group {
		Edit,
		Midi,
		Track,
		View,
		File,
		App
	};

	struct HitSlot {
		TransportHit hit;
		D2D1_RECT_F  rect;
	};

	struct MenuRow {
		bool         heading;
		const wchar_t* text;
		TransportHit hit;
		D2D1_RECT_F  rect;
	};

	struct MenuGroup {
		Group                    group;
		const wchar_t*           title;
		std::vector<TransportHit> items;
	};

	void clear_layout ();
	void add_hit (TransportHit hit, const D2D1_RECT_F& r);
	void build_menu (std::vector<MenuGroup>& groups, float viewport_bottom);

	const wchar_t* glyph_for (TransportHit hit) const;
	bool is_latched (TransportHit hit) const;
	ui::ColorToken latch_color (TransportHit hit) const;
	void label_for (TransportHit hit, wchar_t* buf, size_t n) const;
	void row_label_for (TransportHit hit, wchar_t* buf, size_t n) const;

	void draw_primary (UiDraw& ui, TransportHit hit,
	                   const D2D1_RECT_F& r) const;
	void draw_row (UiDraw& ui, TransportHit hit,
	               const D2D1_RECT_F& r) const;
	void draw_file (UiDraw& ui, TransportHit hit,
	                const D2D1_RECT_F& r) const;
	void draw_clock (UiDraw& ui, float playhead_sec) const;
	void draw_menu (UiDraw& ui) const;

	D2D1_RECT_F _bounds;
	D2D1_RECT_F _clock_bounds;
	D2D1_RECT_F _dsp_bounds;
	D2D1_RECT_F _shuttle_btn;
	D2D1_RECT_F _tempo_dn_btn;
	D2D1_RECT_F _tempo_up_btn;
	D2D1_RECT_F _open_btn;
	D2D1_RECT_F _overflow_btn;
	D2D1_RECT_F _menu_bounds;
	bool        _overflow_open;
	float       _dpi_scale;
	TransportHit _hover;
	TransportHit _pressed;
	float        _hover_x;
	float        _hover_y;

	std::vector<HitSlot> _primary;
	std::vector<HitSlot> _rows;
	std::vector<HitSlot> _files;
	std::vector<HitSlot> _hit_rects;
	std::vector<MenuRow> _menu_rows;

	bool        _playing;
	bool        _recording;
	bool        _record_armed;
	bool        _loop_playing;
	bool        _loop_active;
	bool        _punch_enabled;
	bool        _snap_enabled;
	int         _snap_density; /* 0=bar 1=beat 2=1/2 3=1/4 */
	bool        _midi_step_enabled;
	int         _midi_chord_quality;
	bool        _midi_arp_enabled;
	int         _midi_arp_mode;
	bool        _midi_scale_lock;
	int         _midi_scale_kind;
	int         _midi_scale_root;
	bool        _freeze_active;
	bool        _count_in_enabled;
	bool        _click_enabled;
	float       _click_gain;
	bool        _group_active;
	bool        _group_relative;
	bool        _spectrum_enabled;
	bool        _correlation_enabled;
	bool        _loudness_enabled;
	float       _dsp_load_pct;
	float       _tempo_bpm;
	float       _shuttle_norm;
	int         _bbt_bars;
	int         _bbt_beats;
	int         _bbt_ticks;
	int         _tc_hours;
	int         _tc_minutes;
	int         _tc_seconds;
	int         _tc_frames;
};
