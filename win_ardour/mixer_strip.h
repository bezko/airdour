/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Direct2D mixer track strip: arm + fader + level meter (not GTK).
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

#include "level_meter.h"

class MixerStrip
{
public:
	MixerStrip ();

	void set_label (const wchar_t* label);
	/** Optional processor name shown under the track label (e.g. ACE Reverb). */
	void set_fx_label (const wchar_t* label);
	void layout (const D2D1_RECT_F& strip_bounds, float dpi_scale);

	void  set_gain (float g);
	float gain () const { return _gain; }

	void set_armed (bool armed) { _armed = armed; }
	bool armed () const { return _armed; }

	/** True when strip has a non-empty FX label (insert button is live). */
	bool has_fx () const { return _fx_label[0] != L'\0'; }
	bool hit_fx (float x, float y) const;

	void        set_meter_peak (float peak);
	void        meter_tick (float dt);
	LevelMeter& meter () { return _meter; }

	bool hit_fader (float x, float y) const;
	bool hit_arm (float x, float y) const;
	bool drag_to (float y);

	void draw (ID2D1RenderTarget* rt,
	           ID2D1SolidColorBrush* brand,
	           ID2D1SolidColorBrush* body,
	           ID2D1SolidColorBrush* accent,
	           ID2D1SolidColorBrush* pane,
	           ID2D1SolidColorBrush* edge,
	           ID2D1SolidColorBrush* meter_hot,
	           IDWriteTextFormat* fmt_title,
	           IDWriteTextFormat* fmt_meta,
	           float dpi_scale) const;

	const D2D1_RECT_F& bounds () const { return _strip; }

private:
	wchar_t     _label[32];
	wchar_t     _fx_label[40];
	D2D1_RECT_F _strip;
	D2D1_RECT_F _track;
	D2D1_RECT_F _arm_btn;
	D2D1_RECT_F _fx_btn;
	LevelMeter  _meter;
	float       _gain;
	bool        _armed;
};
