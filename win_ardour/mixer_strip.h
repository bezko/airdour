/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Direct2D mixer track strip with vertical fader (not GTK).
 * Gain is a UI stub (0..1 linear) until Session routes exist.
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

class MixerStrip
{
public:
	MixerStrip ();

	void layout (const D2D1_RECT_F& pane, float dpi_scale);

	/** Linear gain 0..1 (unity ≈ 1). */
	void  set_gain (float g);
	float gain () const { return _gain; }

	bool hit_fader (float x, float y) const;
	/** Map pointer Y into gain; returns true if changed. */
	bool drag_to (float y);

	void draw (ID2D1RenderTarget* rt,
	           ID2D1SolidColorBrush* brand,
	           ID2D1SolidColorBrush* body,
	           ID2D1SolidColorBrush* accent,
	           ID2D1SolidColorBrush* pane,
	           ID2D1SolidColorBrush* edge,
	           IDWriteTextFormat* fmt_title,
	           IDWriteTextFormat* fmt_meta,
	           float dpi_scale) const;

private:
	D2D1_RECT_F _strip;
	D2D1_RECT_F _track; /* fader groove */
	float       _gain;  /* 0..1 */
};
