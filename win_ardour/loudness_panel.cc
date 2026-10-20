/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "loudness_panel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

float
clamp_lufs (float v)
{
	if (!(v > -120.f)) {
		return -120.f;
	}
	if (v > 0.f) {
		return 0.f;
	}
	return v;
}

/* Map −60..0 LUFS → 0..1 for bar fill. */
float
lufs_to_norm (float lufs)
{
	float t = (clamp_lufs (lufs) + 60.f) / 60.f;
	if (t < 0.f) {
		return 0.f;
	}
	if (t > 1.f) {
		return 1.f;
	}
	return t;
}

} // namespace

LoudnessPanel::LoudnessPanel ()
	: _bounds ()
	, _momentary (-70.f)
	, _integrated (-70.f)
	, _visible (false)
{
	wcsncpy_s (_label, L"LUFS ≈", _TRUNCATE);
}

void
LoudnessPanel::layout (const D2D1_RECT_F& bounds)
{
	_bounds = bounds;
}

void
LoudnessPanel::set_values (float momentary_lufs, float integrated_lufs)
{
	float m = clamp_lufs (momentary_lufs);
	float i = clamp_lufs (integrated_lufs);
	_momentary = _momentary * 0.55f + m * 0.45f;
	_integrated = _integrated * 0.75f + i * 0.25f;
}

void
LoudnessPanel::set_label (const wchar_t* label)
{
	if (label && label[0]) {
		wcsncpy_s (_label, label, _TRUNCATE);
	} else {
		wcsncpy_s (_label, L"LUFS ≈", _TRUNCATE);
	}
}

bool
LoudnessPanel::hit (float x, float y) const
{
	if (!_visible) {
		return false;
	}
	return x >= _bounds.left && x <= _bounds.right
	       && y >= _bounds.top && y <= _bounds.bottom;
}

void
LoudnessPanel::draw (ID2D1RenderTarget* rt,
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
		                            _bounds.right - pad * 0.5f - 52.f * dpi_scale,
		                            _bounds.top + title_h),
		               brand);
	}
	if (fmt && body) {
		rt->DrawTextW (L"≈ LUFS", 7, fmt,
		               D2D1::RectF (_bounds.right - pad - 52.f * dpi_scale,
		                            _bounds.top + 2.f * dpi_scale,
		                            _bounds.right - pad,
		                            _bounds.top + title_h),
		               body);
	}

	const float plot_t = _bounds.top + title_h + 2.f * dpi_scale;
	const float plot_b = _bounds.bottom - pad;
	const float plot_h = plot_b - plot_t;
	if (plot_h < 12.f) {
		return;
	}

	wchar_t mtxt[40];
	wchar_t itxt[40];
	std::swprintf (mtxt, 40, L"M  %+.1f", _momentary);
	std::swprintf (itxt, 40, L"I  %+.1f", _integrated);
	if (fmt && body) {
		rt->DrawTextW (mtxt, (UINT32)wcslen (mtxt), fmt,
		               D2D1::RectF (_bounds.left + pad, plot_t,
		                            _bounds.right - pad, plot_t + 16.f * dpi_scale),
		               body);
		rt->DrawTextW (itxt, (UINT32)wcslen (itxt), fmt,
		               D2D1::RectF (_bounds.left + pad, plot_t + 16.f * dpi_scale,
		                            _bounds.right - pad, plot_t + 32.f * dpi_scale),
		               body);
	}

	/* Dual horizontal bars: momentary (accent) over integrated (body). */
	const float bar_l = _bounds.left + pad;
	const float bar_r = _bounds.right - pad;
	const float bar_h = 6.f * dpi_scale;
	const float y0 = plot_b - bar_h * 2.5f - 2.f * dpi_scale;
	const float y1 = plot_b - bar_h;

	if (edge) {
		rt->DrawRectangle (D2D1::RectF (bar_l, y0, bar_r, y0 + bar_h), edge, 1.f);
		rt->DrawRectangle (D2D1::RectF (bar_l, y1, bar_r, y1 + bar_h), edge, 1.f);
	}
	float nm = lufs_to_norm (_momentary);
	float ni = lufs_to_norm (_integrated);
	if (accent && nm > 0.f) {
		rt->FillRectangle (D2D1::RectF (bar_l, y0, bar_l + nm * (bar_r - bar_l), y0 + bar_h),
		                   accent);
	}
	if (body && ni > 0.f) {
		rt->FillRectangle (D2D1::RectF (bar_l, y1, bar_l + ni * (bar_r - bar_l), y1 + bar_h),
		                   body);
	}
}
