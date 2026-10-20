/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Direct2D approximate LUFS (K-weighted, UI-rate). Labeled ≈.
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

class LoudnessPanel
{
public:
	LoudnessPanel ();

	void layout (const D2D1_RECT_F& bounds);
	void set_visible (bool on) { _visible = on; }
	bool visible () const { return _visible; }

	/** Momentary / integrated LUFS (may be −∞ as −120). */
	void set_values (float momentary_lufs, float integrated_lufs);
	void set_label (const wchar_t* label);

	bool hit (float x, float y) const;

	void draw (ID2D1RenderTarget* rt,
	           ID2D1SolidColorBrush* brand,
	           ID2D1SolidColorBrush* body,
	           ID2D1SolidColorBrush* accent,
	           ID2D1SolidColorBrush* pane,
	           ID2D1SolidColorBrush* edge,
	           IDWriteTextFormat* fmt,
	           float dpi_scale) const;

	const D2D1_RECT_F& bounds () const { return _bounds; }

private:
	D2D1_RECT_F _bounds;
	float       _momentary;
	float       _integrated;
	wchar_t     _label[48];
	bool        _visible;
};
