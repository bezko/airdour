/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "transport_bar.h"

#include <cstdio>
#include <cstring>

namespace {

bool
contains (const D2D1_RECT_F& r, float x, float y)
{
	return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

} // namespace

TransportBar::TransportBar ()
	: _bounds ()
	, _play_btn ()
	, _stop_btn ()
	, _rec_btn ()
	, _import_btn ()
	, _playing (false)
	, _recording (false)
	, _record_armed (false)
{
}

void
TransportBar::layout (const D2D1_RECT_F& bounds, float dpi_scale)
{
	_bounds = bounds;
	float h = bounds.bottom - bounds.top;
	float btn_h = h - 10.f * dpi_scale;
	float btn_w = 64.f * dpi_scale;
	float y0 = bounds.top + 5.f * dpi_scale;
	float x0 = bounds.left + 16.f * dpi_scale;
	float gap = 6.f * dpi_scale;
	_play_btn = D2D1::RectF (x0, y0, x0 + btn_w, y0 + btn_h);
	_stop_btn = D2D1::RectF (_play_btn.right + gap, y0, _play_btn.right + gap + btn_w, y0 + btn_h);
	_rec_btn = D2D1::RectF (_stop_btn.right + gap, y0, _stop_btn.right + gap + btn_w, y0 + btn_h);
	_import_btn = D2D1::RectF (_rec_btn.right + gap, y0, _rec_btn.right + gap + btn_w * 1.15f, y0 + btn_h);
}

TransportHit
TransportBar::hit_test (float x, float y) const
{
	if (contains (_play_btn, x, y)) {
		return TransportHit::Play;
	}
	if (contains (_stop_btn, x, y)) {
		return TransportHit::Stop;
	}
	if (contains (_rec_btn, x, y)) {
		return TransportHit::Rec;
	}
	if (contains (_import_btn, x, y)) {
		return TransportHit::Import;
	}
	return TransportHit::None;
}

void
TransportBar::draw (ID2D1RenderTarget* rt,
                    ID2D1SolidColorBrush* brand,
                    ID2D1SolidColorBrush* body,
                    ID2D1SolidColorBrush* accent,
                    ID2D1SolidColorBrush* pane,
                    ID2D1SolidColorBrush* edge,
                    IDWriteTextFormat* fmt,
                    float playhead_sec,
                    float dpi_scale) const
{
	if (!rt) {
		return;
	}

	rt->FillRectangle (_bounds, pane);
	rt->DrawLine (
		D2D1::Point2F (_bounds.left, _bounds.bottom),
		D2D1::Point2F (_bounds.right, _bounds.bottom),
		edge, 1.f);

	auto draw_btn = [&] (const D2D1_RECT_F& r, const wchar_t* label, bool active) {
		rt->FillRectangle (r, active ? accent : edge);
		ID2D1SolidColorBrush* ink = brand;
		DWRITE_TEXT_ALIGNMENT ta = fmt->GetTextAlignment ();
		DWRITE_PARAGRAPH_ALIGNMENT pa = fmt->GetParagraphAlignment ();
		fmt->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
		fmt->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		rt->DrawTextW (label, (UINT32)wcslen (label), fmt, r, ink);
		fmt->SetTextAlignment (ta);
		fmt->SetParagraphAlignment (pa);
	};

	draw_btn (_play_btn, L"Play", _playing);
	draw_btn (_stop_btn, L"Stop", !_playing);
	draw_btn (_rec_btn, L"Rec", _recording || _record_armed);
	draw_btn (_import_btn, L"Import", false);

	wchar_t clock[64];
	int m = (int)(playhead_sec / 60.f);
	float s = playhead_sec - (float)m * 60.f;
	if (_recording) {
		_snwprintf_s (clock, _TRUNCATE, L"%02d:%05.2f  ·  RECORDING", m, s);
	} else if (_record_armed) {
		_snwprintf_s (clock, _TRUNCATE, L"%02d:%05.2f  ·  rec armed", m, s);
	} else {
		_snwprintf_s (clock, _TRUNCATE, L"%02d:%05.2f  ·  space toggle", m, s);
	}

	float clock_left = _import_btn.right + 16.f * dpi_scale;
	DWRITE_TEXT_ALIGNMENT ta = fmt->GetTextAlignment ();
	DWRITE_PARAGRAPH_ALIGNMENT pa = fmt->GetParagraphAlignment ();
	fmt->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_LEADING);
	fmt->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	rt->DrawTextW (clock, (UINT32)wcslen (clock), fmt,
	               D2D1::RectF (clock_left, _bounds.top, _bounds.right - 16.f * dpi_scale, _bounds.bottom),
	               body);
	fmt->SetTextAlignment (ta);
	fmt->SetParagraphAlignment (pa);
}
