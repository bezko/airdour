/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "automation_lane.h"
#include "cairo_wave_blit.h"

#include <algorithm>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <dwrite.h>

AutomationLane::AutomationLane ()
	: _curve_n (0)
	, _pt_n (0)
	, _playhead_norm (-1.f)
{
	wcsncpy_s (_label, L"Gain", _TRUNCATE);
	for (size_t i = 0; i < kMaxColumns; ++i) {
		_curve[i] = 0.5f;
	}
	for (size_t i = 0; i < kMaxPoints; ++i) {
		_pt_x[i] = 0.f;
		_pt_y[i] = 0.5f;
	}
}

void
AutomationLane::set_label (const wchar_t* label)
{
	if (label && label[0]) {
		wcsncpy_s (_label, label, _TRUNCATE);
	} else {
		wcsncpy_s (_label, L"Gain", _TRUNCATE);
	}
}

void
AutomationLane::set_curve (const float* y_0_1, size_t n)
{
	_curve_n = 0;
	if (!y_0_1 || n == 0) {
		return;
	}
	if (n > kMaxColumns) {
		n = kMaxColumns;
	}
	for (size_t i = 0; i < n; ++i) {
		_curve[i] = std::max (0.f, std::min (1.f, y_0_1[i]));
	}
	_curve_n = n;
}

void
AutomationLane::set_points (const float* x_norm, const float* y_0_1, size_t n)
{
	_pt_n = 0;
	if (!x_norm || !y_0_1 || n == 0) {
		return;
	}
	if (n > kMaxPoints) {
		n = kMaxPoints;
	}
	for (size_t i = 0; i < n; ++i) {
		_pt_x[i] = std::max (0.f, std::min (1.f, x_norm[i]));
		_pt_y[i] = std::max (0.f, std::min (1.f, y_0_1[i]));
	}
	_pt_n = n;
}

void
AutomationLane::set_playhead_norm (float norm)
{
	_playhead_norm = norm;
}

bool
AutomationLane::hit (const D2D1_RECT_F& bounds, float x, float y) const
{
	return x >= bounds.left && x <= bounds.right
	    && y >= bounds.top && y <= bounds.bottom;
}

float
AutomationLane::value_at_y (const D2D1_RECT_F& bounds, float y) const
{
	float h = bounds.bottom - bounds.top;
	if (h < 1.f) {
		return 0.5f;
	}
	float t = (bounds.bottom - y) / h;
	return std::max (0.f, std::min (1.f, t));
}

void
AutomationLane::draw (ID2D1RenderTarget* rt,
                       const D2D1_RECT_F& bounds,
                       ID2D1SolidColorBrush* line,
                       ID2D1SolidColorBrush* fill,
                       ID2D1SolidColorBrush* edge,
                       ID2D1SolidColorBrush* playhead,
                       IDWriteTextFormat* fmt_meta,
                       float dpi_scale) const
{
	if (!rt) {
		return;
	}

	float w = bounds.right - bounds.left;
	float h = bounds.bottom - bounds.top;
	if (w < 8.f || h < 8.f) {
		return;
	}

	rt->FillRectangle (bounds, edge);
	rt->DrawRectangle (bounds, edge, 1.f);

	if (fmt_meta && fill) {
		D2D1_RECT_F lab = D2D1::RectF (
			bounds.left + 4.f * dpi_scale, bounds.top + 1.f * dpi_scale,
			bounds.left + 72.f * dpi_scale, bounds.top + 16.f * dpi_scale);
		fill->SetOpacity (0.85f);
		rt->DrawTextW (_label, (UINT32)wcslen (_label), fmt_meta, lab, fill);
		fill->SetOpacity (1.f);
	}

	/* 0 dB / mid reference */
	if (fill) {
		float mid_y = bounds.bottom - 0.5f * h;
		fill->SetOpacity (0.25f);
		rt->DrawLine (D2D1::Point2F (bounds.left, mid_y),
		              D2D1::Point2F (bounds.right, mid_y),
		              fill, 1.f);
		fill->SetOpacity (1.f);
	}

	/* Prefer Cairo AA polyline + dots (canvas-style); D2D fallback. */
	bool cairo_ok = false;
	if (CairoWaveBlit::available () && (_curve_n >= 2 || _pt_n > 0)) {
		cairo_ok = CairoWaveBlit::blit_automation_curve (
			rt, bounds,
			_curve_n >= 2 ? _curve : nullptr, _curve_n,
			_pt_n > 0 ? _pt_x : nullptr,
			_pt_n > 0 ? _pt_y : nullptr, _pt_n,
			dpi_scale,
			0.30f, 0.75f, 0.95f);
	}
	if (!cairo_ok && _curve_n >= 2 && line) {
		float col_w = w / (float)_curve_n;
		D2D1_POINT_2F prev = D2D1::Point2F (
			bounds.left + col_w * 0.5f,
			bounds.bottom - _curve[0] * h);
		for (size_t i = 1; i < _curve_n; ++i) {
			D2D1_POINT_2F cur = D2D1::Point2F (
				bounds.left + ((float)i + 0.5f) * col_w,
				bounds.bottom - _curve[i] * h);
			rt->DrawLine (prev, cur, line, std::max (1.5f, 1.8f * dpi_scale));
			prev = cur;
		}
	}

	if (!cairo_ok && _pt_n > 0 && line) {
		float r = std::max (2.5f, 3.f * dpi_scale);
		for (size_t i = 0; i < _pt_n; ++i) {
			float px = bounds.left + _pt_x[i] * w;
			float py = bounds.bottom - _pt_y[i] * h;
			rt->FillEllipse (D2D1::Ellipse (D2D1::Point2F (px, py), r, r), line);
		}
	}

	if (playhead && _playhead_norm >= 0.f && _playhead_norm <= 1.f) {
		float px = bounds.left + _playhead_norm * w;
		float thick = std::max (1.5f, 2.f * dpi_scale);
		rt->FillRectangle (
			D2D1::RectF (px - thick * 0.5f, bounds.top, px + thick * 0.5f, bounds.bottom),
			playhead);
	}
}
