/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Direct2D transport: Play / Stop / Rec / Import / Save / Open / Export.
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
	Export
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
	bool        _playing;
	bool        _recording;
	bool        _record_armed;
};
