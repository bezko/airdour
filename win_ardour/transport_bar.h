/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Direct2D transport: Play / Stop / Rec / Loop / Punch / Mark / Split / Fade /
 * Stretch / Pitch± / Reverse / Norm / Sil / Cons / Frz / Snap / Count / Click / Tempo± / Shuttle / Import / Save / Open / Export.
 * Clock shows MM:SS + BBT + timecode + BPM.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

enum class TransportHit {
	None,
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

	void layout (const D2D1_RECT_F& bounds, float dpi_scale);

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

	void set_freeze_active (bool on) { _freeze_active = on; }
	bool freeze_active () const { return _freeze_active; }

	void set_count_in_enabled (bool on) { _count_in_enabled = on; }
	bool count_in_enabled () const { return _count_in_enabled; }

	void set_click_enabled (bool on) { _click_enabled = on; }
	bool click_enabled () const { return _click_enabled; }

	void set_tempo_bpm (float bpm) { _tempo_bpm = bpm; }
	float tempo_bpm () const { return _tempo_bpm; }

	void set_group_active (bool on) { _group_active = on; }
	bool group_active () const { return _group_active; }

	void set_spectrum_enabled (bool on) { _spectrum_enabled = on; }
	bool spectrum_enabled () const { return _spectrum_enabled; }

	void set_correlation_enabled (bool on) { _correlation_enabled = on; }
	bool correlation_enabled () const { return _correlation_enabled; }

	void set_loudness_enabled (bool on) { _loudness_enabled = on; }
	bool loudness_enabled () const { return _loudness_enabled; }

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

	void draw (ID2D1RenderTarget* rt,
	           ID2D1SolidColorBrush* brand,
	           ID2D1SolidColorBrush* body,
	           ID2D1SolidColorBrush* accent,
	           ID2D1SolidColorBrush* pane,
	           ID2D1SolidColorBrush* edge,
	           IDWriteTextFormat* fmt,
	           float playhead_sec,
	           float dpi_scale) const;

	const D2D1_RECT_F& bounds () const { return _bounds; }
	const D2D1_RECT_F& shuttle_btn () const { return _shuttle_btn; }
	const D2D1_RECT_F& open_btn () const { return _open_btn; }

private:
	D2D1_RECT_F _bounds;
	D2D1_RECT_F _play_btn;
	D2D1_RECT_F _stop_btn;
	D2D1_RECT_F _rec_btn;
	D2D1_RECT_F _import_btn;
	D2D1_RECT_F _save_btn;
	D2D1_RECT_F _open_btn;
	D2D1_RECT_F _export_btn;
	D2D1_RECT_F _loop_btn;
	D2D1_RECT_F _punch_btn;
	D2D1_RECT_F _mark_btn;
	D2D1_RECT_F _split_btn;
	D2D1_RECT_F _fade_btn;
	D2D1_RECT_F _stretch_btn;
	D2D1_RECT_F _pitch_up_btn;
	D2D1_RECT_F _pitch_dn_btn;
	D2D1_RECT_F _reverse_btn;
	D2D1_RECT_F _normalize_btn;
	D2D1_RECT_F _strip_silence_btn;
	D2D1_RECT_F _consolidate_btn;
	D2D1_RECT_F _freeze_btn;
	D2D1_RECT_F _snap_btn;
	D2D1_RECT_F _quantize_btn;
	D2D1_RECT_F _count_btn;
	D2D1_RECT_F _click_btn;
	D2D1_RECT_F _tempo_dn_btn;
	D2D1_RECT_F _tempo_up_btn;
	D2D1_RECT_F _add_audio_btn;
	D2D1_RECT_F _add_midi_btn;
	D2D1_RECT_F _remove_track_btn;
	D2D1_RECT_F _track_up_btn;
	D2D1_RECT_F _track_dn_btn;
	D2D1_RECT_F _group_btn;
	D2D1_RECT_F _shuttle_btn;
	D2D1_RECT_F _fit_session_btn;
	D2D1_RECT_F _fit_selection_btn;
	D2D1_RECT_F _fit_regions_btn;
	D2D1_RECT_F _help_btn;
	D2D1_RECT_F _prefs_btn;
	D2D1_RECT_F _about_btn;
	D2D1_RECT_F _spectrum_btn;
	D2D1_RECT_F _phase_btn;
	D2D1_RECT_F _loudness_btn;
	bool        _playing;
	bool        _recording;
	bool        _record_armed;
	bool        _loop_playing;
	bool        _loop_active;
	bool        _punch_enabled;
	bool        _snap_enabled;
	bool        _freeze_active;
	bool        _count_in_enabled;
	bool        _click_enabled;
	bool        _group_active;
	bool        _spectrum_enabled;
	bool        _correlation_enabled;
	bool        _loudness_enabled;
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
