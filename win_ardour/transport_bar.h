/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Direct2D transport: Play / Stop / Rec / Loop / Punch / Mark / Split / Fade /
 * Snap / Count / Click / Tempo± / Import / Save / Open / Export.
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
	Snap,
	CountIn,
	Click,
	TempoDown,
	TempoUp
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

	void set_count_in_enabled (bool on) { _count_in_enabled = on; }
	bool count_in_enabled () const { return _count_in_enabled; }

	void set_click_enabled (bool on) { _click_enabled = on; }
	bool click_enabled () const { return _click_enabled; }

	void set_tempo_bpm (float bpm) { _tempo_bpm = bpm; }
	float tempo_bpm () const { return _tempo_bpm; }

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
	D2D1_RECT_F _snap_btn;
	D2D1_RECT_F _count_btn;
	D2D1_RECT_F _click_btn;
	D2D1_RECT_F _tempo_dn_btn;
	D2D1_RECT_F _tempo_up_btn;
	bool        _playing;
	bool        _recording;
	bool        _record_armed;
	bool        _loop_playing;
	bool        _loop_active;
	bool        _punch_enabled;
	bool        _snap_enabled;
	bool        _count_in_enabled;
	bool        _click_enabled;
	float       _tempo_bpm;
};
