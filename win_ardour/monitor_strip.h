/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Compact D2D monitor strip: Dim / Mono / Mute (cut) for Session MonitorProcessor.
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

enum class MonitorHit {
	None,
	Dim,
	Mono,
	Mute
};

class MonitorStrip
{
public:
	MonitorStrip ();

	void layout (const D2D1_RECT_F& bounds, float dpi_scale);

	void set_available (bool ok) { _available = ok; }
	bool available () const { return _available; }

	void set_dim (bool on) { _dim = on; }
	bool dim () const { return _dim; }
	void set_mono (bool on) { _mono = on; }
	bool mono () const { return _mono; }
	void set_mute (bool on) { _mute = on; }
	bool mute () const { return _mute; }

	MonitorHit hit_test (float x, float y) const;

	void draw (ID2D1RenderTarget* rt,
	           ID2D1SolidColorBrush* brand,
	           ID2D1SolidColorBrush* body,
	           ID2D1SolidColorBrush* accent,
	           ID2D1SolidColorBrush* pane,
	           ID2D1SolidColorBrush* edge,
	           IDWriteTextFormat* fmt_title,
	           IDWriteTextFormat* fmt_meta,
	           float dpi_scale) const;

	const D2D1_RECT_F& bounds () const { return _strip; }

private:
	D2D1_RECT_F _strip;
	D2D1_RECT_F _dim_btn;
	D2D1_RECT_F _mono_btn;
	D2D1_RECT_F _mute_btn;
	bool        _available;
	bool        _dim;
	bool        _mono;
	bool        _mute;
};
