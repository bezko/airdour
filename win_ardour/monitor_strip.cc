/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "monitor_strip.h"

#include <cstring>

namespace {

bool
contains (const D2D1_RECT_F& r, float x, float y)
{
	return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

} // namespace

MonitorStrip::MonitorStrip ()
	: _strip ()
	, _dim_btn ()
	, _mono_btn ()
	, _mute_btn ()
	, _available (false)
	, _dim (false)
	, _mono (false)
	, _mute (false)
{
}

void
MonitorStrip::layout (const D2D1_RECT_F& bounds, float dpi_scale)
{
	_strip = bounds;
	float pad = 8.f * dpi_scale;
	float y = _strip.top + 28.f * dpi_scale;
	float btn_h = 26.f * dpi_scale;
	float gap = 6.f * dpi_scale;
	_dim_btn = D2D1::RectF (_strip.left + pad, y, _strip.right - pad, y + btn_h);
	y = _dim_btn.bottom + gap;
	_mono_btn = D2D1::RectF (_strip.left + pad, y, _strip.right - pad, y + btn_h);
	y = _mono_btn.bottom + gap;
	_mute_btn = D2D1::RectF (_strip.left + pad, y, _strip.right - pad, y + btn_h);
}

MonitorHit
MonitorStrip::hit_test (float x, float y) const
{
	if (!_available) {
		return MonitorHit::None;
	}
	if (contains (_dim_btn, x, y)) {
		return MonitorHit::Dim;
	}
	if (contains (_mono_btn, x, y)) {
		return MonitorHit::Mono;
	}
	if (contains (_mute_btn, x, y)) {
		return MonitorHit::Mute;
	}
	return MonitorHit::None;
}

void
MonitorStrip::draw (ID2D1RenderTarget* rt,
                    ID2D1SolidColorBrush* brand,
                    ID2D1SolidColorBrush* body,
                    ID2D1SolidColorBrush* accent,
                    ID2D1SolidColorBrush* pane,
                    ID2D1SolidColorBrush* edge,
                    IDWriteTextFormat* fmt_title,
                    IDWriteTextFormat* fmt_meta,
                    float dpi_scale) const
{
	if (!rt) {
		return;
	}

	rt->FillRectangle (_strip, pane);
	rt->DrawRectangle (_strip, edge, 1.f);
	rt->FillRectangle (
		D2D1::RectF (_strip.left, _strip.top, _strip.left + 3.f * dpi_scale, _strip.bottom),
		accent);

	DWRITE_TEXT_ALIGNMENT ta = fmt_title->GetTextAlignment ();
	fmt_title->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
	rt->DrawTextW (L"Monitor", 7, fmt_title,
	               D2D1::RectF (_strip.left, _strip.top + 4.f * dpi_scale,
	                            _strip.right, _strip.top + 26.f * dpi_scale),
	               brand);
	fmt_title->SetTextAlignment (ta);

	DWRITE_TEXT_ALIGNMENT ta_m = fmt_meta->GetTextAlignment ();
	DWRITE_PARAGRAPH_ALIGNMENT pa_m = fmt_meta->GetParagraphAlignment ();
	fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
	fmt_meta->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

	auto draw_btn = [&] (const D2D1_RECT_F& r, const wchar_t* label, bool on) {
		rt->FillRectangle (r, on ? accent : edge);
		rt->DrawTextW (label, (UINT32)wcslen (label), fmt_meta, r, brand);
	};

	if (_available) {
		draw_btn (_dim_btn, L"Dim", _dim);
		draw_btn (_mono_btn, L"Mono", _mono);
		draw_btn (_mute_btn, L"Mute", _mute);
	} else {
		rt->FillRectangle (_dim_btn, edge);
		rt->DrawTextW (L"—", 1, fmt_meta, _dim_btn, body);
	}

	fmt_meta->SetTextAlignment (ta_m);
	fmt_meta->SetParagraphAlignment (pa_m);
}
