/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "spectrum_panel.h"
#include "cairo_wave_blit.h"

#include <algorithm>
#include <cmath>
#include <cstring>

SpectrumPanel::SpectrumPanel ()
	: _bounds ()
	, _visible (false)
{
	std::memset (_levels, 0, sizeof (_levels));
	wcsncpy_s (_label, L"Spectrum", _TRUNCATE);
}

void
SpectrumPanel::layout (const D2D1_RECT_F& bounds)
{
	_bounds = bounds;
}

void
SpectrumPanel::set_bands (const float* levels, int count)
{
	if (!levels) {
		std::memset (_levels, 0, sizeof (_levels));
		return;
	}
	const int n = std::max (0, std::min (count, kBands));
	for (int i = 0; i < n; ++i) {
		float v = levels[i];
		if (!(v >= 0.f)) {
			v = 0.f;
		}
		if (v > 1.f) {
			v = 1.f;
		}
		/* Light peak-hold so bars read clearly at UI rate. */
		if (v >= _levels[i]) {
			_levels[i] = v;
		} else {
			_levels[i] = _levels[i] * 0.82f + v * 0.18f;
		}
	}
	for (int i = n; i < kBands; ++i) {
		_levels[i] *= 0.85f;
	}
}

void
SpectrumPanel::set_label (const wchar_t* label)
{
	if (label && label[0]) {
		wcsncpy_s (_label, label, _TRUNCATE);
	} else {
		wcsncpy_s (_label, L"Spectrum", _TRUNCATE);
	}
}

bool
SpectrumPanel::hit (float x, float y) const
{
	if (!_visible) {
		return false;
	}
	return x >= _bounds.left && x <= _bounds.right
	       && y >= _bounds.top && y <= _bounds.bottom;
}

void
SpectrumPanel::draw (ID2D1RenderTarget* rt,
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

	const float plot_l = _bounds.left + pad;
	const float plot_r = _bounds.right - pad;
	const float plot_t = _bounds.top + title_h + 2.f * dpi_scale;
	const float plot_b = _bounds.bottom - pad;
	const float plot_w = plot_r - plot_l;
	const float plot_h = plot_b - plot_t;
	if (plot_w < 8.f || plot_h < 8.f || !accent) {
		return;
	}

	D2D1_RECT_F plot = D2D1::RectF (plot_l, plot_t, plot_r, plot_b);
	bool cairo_ok = false;
	if (CairoWaveBlit::available ()) {
		cairo_ok = CairoWaveBlit::blit_bars (
			rt, plot, _levels, (size_t)kBands, dpi_scale,
			0.12f, 0.86f, 0.74f, 0.95f);
	}
	if (!cairo_ok) {
		const float gap = 1.f * dpi_scale;
		const float bar_w = (plot_w - gap * (float)(kBands - 1)) / (float)kBands;
		for (int i = 0; i < kBands; ++i) {
			float h = _levels[i] * plot_h;
			if (h < 1.f * dpi_scale && _levels[i] > 0.01f) {
				h = 1.f * dpi_scale;
			}
			float x0 = plot_l + (float)i * (bar_w + gap);
			D2D1_RECT_F bar = D2D1::RectF (x0, plot_b - h, x0 + bar_w, plot_b);
			rt->FillRectangle (bar, accent);
		}
	}

	if (body && fmt) {
		rt->DrawTextW (L"Spec", 4, fmt,
		               D2D1::RectF (_bounds.right - pad - 36.f * dpi_scale,
		                            _bounds.top + 2.f * dpi_scale,
		                            _bounds.right - pad,
		                            _bounds.top + title_h),
		               body);
	}
}
