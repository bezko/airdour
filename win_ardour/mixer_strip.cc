/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "mixer_strip.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

MixerStrip::MixerStrip ()
	: _strip ()
	, _track ()
	, _gain (0.85f)
{
}

void
MixerStrip::layout (const D2D1_RECT_F& pane, float dpi_scale)
{
	float inset = 14.f * dpi_scale;
	float strip_w = 88.f * dpi_scale;
	_strip = D2D1::RectF (
		pane.left + inset,
		pane.top + inset + 28.f * dpi_scale,
		pane.left + inset + strip_w,
		pane.bottom - inset);

	float track_w = 10.f * dpi_scale;
	float cx = (_strip.left + _strip.right) * 0.5f;
	float top = _strip.top + 36.f * dpi_scale;
	float bot = _strip.bottom - 40.f * dpi_scale;
	_track = D2D1::RectF (cx - track_w * 0.5f, top, cx + track_w * 0.5f, bot);
}

void
MixerStrip::set_gain (float g)
{
	_gain = std::max (0.f, std::min (1.f, g));
}

bool
MixerStrip::hit_fader (float x, float y) const
{
	/* Generous hit pad around the groove. */
	float pad = 14.f;
	return x >= _track.left - pad && x <= _track.right + pad
	    && y >= _track.top - pad && y <= _track.bottom + pad;
}

bool
MixerStrip::drag_to (float y)
{
	float h = _track.bottom - _track.top;
	if (h < 1.f) {
		return false;
	}
	/* Top = unity, bottom = silence. */
	float t = (_track.bottom - y) / h;
	float g = std::max (0.f, std::min (1.f, t));
	if (g == _gain) {
		return false;
	}
	_gain = g;
	return true;
}

void
MixerStrip::draw (ID2D1RenderTarget* rt,
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
	rt->DrawTextW (L"Track 1", 7, fmt_title,
	               D2D1::RectF (_strip.left, _strip.top + 6.f * dpi_scale,
	                            _strip.right, _strip.top + 28.f * dpi_scale),
	               brand);
	fmt_title->SetTextAlignment (ta);

	/* Groove */
	rt->FillRectangle (_track, edge);

	float h = _track.bottom - _track.top;
	float knob_y = _track.bottom - _gain * h;
	float knob_h = 12.f * dpi_scale;
	float knob_w = 28.f * dpi_scale;
	float cx = (_track.left + _track.right) * 0.5f;
	D2D1_RECT_F knob = D2D1::RectF (
		cx - knob_w * 0.5f,
		knob_y - knob_h * 0.5f,
		cx + knob_w * 0.5f,
		knob_y + knob_h * 0.5f);
	rt->FillRectangle (knob, accent);

	/* Fill below knob (level meter style). */
	if (_gain > 0.001f) {
		accent->SetOpacity (0.35f);
		rt->FillRectangle (D2D1::RectF (_track.left, knob_y, _track.right, _track.bottom), accent);
		accent->SetOpacity (1.f);
	}

	wchar_t db[32];
	float db_v = (_gain <= 0.0001f) ? -96.f : (20.f * log10f (_gain));
	_snwprintf_s (db, _TRUNCATE, L"%.1f dB", db_v);
	DWRITE_TEXT_ALIGNMENT ta2 = fmt_meta->GetTextAlignment ();
	fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
	rt->DrawTextW (db, (UINT32)wcslen (db), fmt_meta,
	               D2D1::RectF (_strip.left, _strip.bottom - 28.f * dpi_scale,
	                            _strip.right, _strip.bottom - 6.f * dpi_scale),
	               body);
	fmt_meta->SetTextAlignment (ta2);
}
