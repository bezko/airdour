/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "correlation_panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

CorrelationPanel::CorrelationPanel ()
	: _bounds ()
	, _corr (0.f)
	, _gonio_n (0)
	, _visible (false)
{
	std::memset (_gonio_x, 0, sizeof (_gonio_x));
	std::memset (_gonio_y, 0, sizeof (_gonio_y));
	wcsncpy_s (_label, L"Phase", _TRUNCATE);
}

void
CorrelationPanel::layout (const D2D1_RECT_F& bounds)
{
	_bounds = bounds;
}

void
CorrelationPanel::set_correlation (float r)
{
	if (!(r >= -1.f)) {
		r = -1.f;
	}
	if (r > 1.f) {
		r = 1.f;
	}
	_corr = _corr * 0.65f + r * 0.35f;
}

void
CorrelationPanel::set_gonio (const float* x, const float* y, int count)
{
	if (!x || !y || count <= 0) {
		_gonio_n = 0;
		return;
	}
	_gonio_n = std::max (0, std::min (count, kGonioPts));
	for (int i = 0; i < _gonio_n; ++i) {
		float xi = x[i];
		float yi = y[i];
		if (!(xi >= -1.f)) {
			xi = -1.f;
		}
		if (xi > 1.f) {
			xi = 1.f;
		}
		if (!(yi >= -1.f)) {
			yi = -1.f;
		}
		if (yi > 1.f) {
			yi = 1.f;
		}
		_gonio_x[i] = xi;
		_gonio_y[i] = yi;
	}
}

void
CorrelationPanel::set_label (const wchar_t* label)
{
	if (label && label[0]) {
		wcsncpy_s (_label, label, _TRUNCATE);
	} else {
		wcsncpy_s (_label, L"Phase", _TRUNCATE);
	}
}

bool
CorrelationPanel::hit (float x, float y) const
{
	if (!_visible) {
		return false;
	}
	return x >= _bounds.left && x <= _bounds.right
	       && y >= _bounds.top && y <= _bounds.bottom;
}

void
CorrelationPanel::draw (ID2D1RenderTarget* rt,
                        ID2D1SolidColorBrush* brand,
                        ID2D1SolidColorBrush* body,
                        ID2D1SolidColorBrush* accent,
                        ID2D1SolidColorBrush* pane,
                        ID2D1SolidColorBrush* edge,
                        IDWriteTextFormat* fmt,
                        float dpi_scale) const
{
	if (!rt || !_visible || _bounds.right <= _bounds.left) {
		return;
	}

	rt->FillRectangle (_bounds, pane);
	rt->DrawRectangle (_bounds, edge, 1.f);

	const float pad = 6.f * dpi_scale;
	const float title_h = 16.f * dpi_scale;
	if (fmt && brand) {
		rt->DrawTextW (_label, (UINT32)wcslen (_label), fmt,
		               D2D1::RectF (_bounds.left + pad, _bounds.top + 2.f * dpi_scale,
		                            _bounds.right - pad, _bounds.top + title_h),
		               brand);
	}

	wchar_t val[32];
	std::swprintf (val, 32, L"%+.2f", _corr);
	if (fmt && body) {
		rt->DrawTextW (val, (UINT32)wcslen (val), fmt,
		               D2D1::RectF (_bounds.right - pad - 48.f * dpi_scale,
		                            _bounds.top + 2.f * dpi_scale,
		                            _bounds.right - pad,
		                            _bounds.top + title_h),
		               body);
	}

	const float plot_t = _bounds.top + title_h + 2.f * dpi_scale;
	const float plot_b = _bounds.bottom - pad;
	const float plot_h = plot_b - plot_t;
	if (plot_h < 8.f) {
		return;
	}

	/* Left: correlation bar (−1..+1). Right: goniometer square. */
	const float split = _bounds.left + pad
	                    + (_bounds.right - _bounds.left - pad * 2.f) * 0.42f;
	const float bar_l = _bounds.left + pad;
	const float bar_r = split - 4.f * dpi_scale;
	const float bar_mid_y = (plot_t + plot_b) * 0.5f;
	const float bar_h = 10.f * dpi_scale;

	if (edge) {
		rt->DrawRectangle (D2D1::RectF (bar_l, bar_mid_y - bar_h * 0.5f,
		                                bar_r, bar_mid_y + bar_h * 0.5f),
		                   edge, 1.f);
		float cx = (bar_l + bar_r) * 0.5f;
		rt->DrawLine (D2D1::Point2F (cx, bar_mid_y - bar_h * 0.5f - 2.f * dpi_scale),
		              D2D1::Point2F (cx, bar_mid_y + bar_h * 0.5f + 2.f * dpi_scale),
		              body ? body : edge, 1.f);
	}
	if (accent) {
		float t = (_corr + 1.f) * 0.5f; /* 0..1 */
		float nx = bar_l + t * (bar_r - bar_l);
		float tw = 4.f * dpi_scale;
		rt->FillRectangle (D2D1::RectF (nx - tw * 0.5f, bar_mid_y - bar_h * 0.5f - 1.f * dpi_scale,
		                                nx + tw * 0.5f, bar_mid_y + bar_h * 0.5f + 1.f * dpi_scale),
		                   accent);
	}
	if (fmt && body) {
		rt->DrawTextW (L"−1", 2, fmt,
		               D2D1::RectF (bar_l, plot_b - 14.f * dpi_scale, bar_l + 20.f * dpi_scale, plot_b),
		               body);
		rt->DrawTextW (L"+1", 2, fmt,
		               D2D1::RectF (bar_r - 20.f * dpi_scale, plot_b - 14.f * dpi_scale, bar_r, plot_b),
		               body);
	}

	/* Goniometer */
	const float g_l = split + 4.f * dpi_scale;
	const float g_r = _bounds.right - pad;
	float g_side = std::min (g_r - g_l, plot_h);
	if (g_side < 8.f) {
		return;
	}
	float g_cx = (g_l + g_r) * 0.5f;
	float g_cy = (plot_t + plot_b) * 0.5f;
	float g_l2 = g_cx - g_side * 0.5f;
	float g_t2 = g_cy - g_side * 0.5f;
	D2D1_RECT_F gbox = D2D1::RectF (g_l2, g_t2, g_l2 + g_side, g_t2 + g_side);
	if (edge) {
		rt->DrawRectangle (gbox, edge, 1.f);
		/* Diagonal guides (mono / out-of-phase). */
		rt->DrawLine (D2D1::Point2F (gbox.left, gbox.bottom),
		              D2D1::Point2F (gbox.right, gbox.top), body ? body : edge, 1.f);
		rt->DrawLine (D2D1::Point2F (gbox.left, gbox.top),
		              D2D1::Point2F (gbox.right, gbox.bottom), body ? body : edge, 1.f);
	}
	if (accent && _gonio_n > 0) {
		for (int i = 0; i < _gonio_n; ++i) {
			float px = g_l2 + (_gonio_x[i] * 0.5f + 0.5f) * g_side;
			float py = g_t2 + (1.f - (_gonio_y[i] * 0.5f + 0.5f)) * g_side;
			float s = 1.5f * dpi_scale;
			rt->FillRectangle (D2D1::RectF (px - s, py - s, px + s, py + s), accent);
		}
	}
}
