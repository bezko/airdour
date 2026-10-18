/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Direct2D peak meter. Stub-fed until Session PeakMeter is reachable.
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

	/** Push instantaneous peak 0..1 (UI thread). */
	void set_peak (float peak);
	/** Decay toward silence when idle. */
	void tick (float dt_sec);

	float level () const { return _level; }
	float peak_hold () const { return _hold; }

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
};
