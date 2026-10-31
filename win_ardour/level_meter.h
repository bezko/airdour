/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Direct2D peak meter with brief peak-hold and clip (≥0 dBFS) LED.
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

class LevelMeter
{
public:
	LevelMeter ();

	void layout (const D2D1_RECT_F& bounds);

	/** Push instantaneous peak 0..1 (UI thread). peak≥~1 → clip latch. */
	void set_peak (float peak);
	/** Sticky clip from PeakMeter MeterMaxPeak / ≥0 dBFS. */
	void set_clipped (bool clipped);
	/** Clear clip LED (and forget latch). */
	void clear_clip ();
	/** Decay toward silence when idle; age peak-hold / clip hold. */
	void tick (float dt_sec);

	float level () const { return _level; }
	float peak_hold () const { return _hold; }
	bool  clipped () const { return _clipped; }

	bool hit (float x, float y) const;
	/** Top clip-LED band only (not full meter body). */
	bool hit_clip_led (float x, float y) const;
	void set_clip_led_height (float h_px);

	void draw (ID2D1RenderTarget* rt,
	           ID2D1SolidColorBrush* fill,
	           ID2D1SolidColorBrush* edge,
	           ID2D1SolidColorBrush* hot,
	           float dpi_scale) const;

	const D2D1_RECT_F& bounds () const { return _bounds; }

private:
	D2D1_RECT_F _bounds;
	float       _level;
	float       _hold;
	float       _hold_age;
	bool        _clipped;
	float       _clip_age;
	float       _led_h; /* px; 0 → fraction of meter height */
};
