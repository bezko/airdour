/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Direct2D automation curve overlay for one Editor control (not GTK).
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <windows.h>
#include <d2d1.h>

class AutomationLane
{
public:
	static constexpr size_t kMaxColumns = 128;
	static constexpr size_t kMaxPoints  = 64;

	AutomationLane ();

	void set_label (const wchar_t* label);
	void set_curve (const float* y_0_1, size_t n);
	void set_points (const float* x_norm, const float* y_0_1, size_t n);
	void set_playhead_norm (float norm); /* <0 = off-screen */

	bool  hit (const D2D1_RECT_F& bounds, float x, float y) const;
	float value_at_y (const D2D1_RECT_F& bounds, float y) const; /* 1=top … 0=bottom */

	void draw (ID2D1RenderTarget* rt,
	           const D2D1_RECT_F& bounds,
	           ID2D1SolidColorBrush* line,
	           ID2D1SolidColorBrush* fill,
	           ID2D1SolidColorBrush* edge,
	           ID2D1SolidColorBrush* playhead,
	           IDWriteTextFormat* fmt_meta,
	           float dpi_scale) const;

private:
	wchar_t _label[40];
	float   _curve[kMaxColumns];
	size_t  _curve_n;
	float   _pt_x[kMaxPoints];
	float   _pt_y[kMaxPoints];
	size_t  _pt_n;
	float   _playhead_norm;
};
