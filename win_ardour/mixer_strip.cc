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
	wcsncpy_s (_label, L"Track", _TRUNCATE);
}

void
MixerStrip::set_label (const wchar_t* label)
{
	if (label) {
		wcsncpy_s (_label, label, _TRUNCATE);
	}
}

void
MixerStrip::layout (const D2D1_RECT_F& strip_bounds, float dpi_scale)
{
	_strip = strip_bounds;

	float pad = 8.f * dpi_scale;
	float top = _strip.top + 32.f * dpi_scale;
	float bot = _strip.bottom - 36.f * dpi_scale;
	float inner_w = _strip.right - _strip.left - pad * 2.f;
	float fader_w = inner_w * 0.42f;
	float meter_w = 12.f * dpi_scale;

	float fader_cx = _strip.left + pad + fader_w * 0.5f;
	float track_w = 10.f * dpi_scale;
	_track = D2D1::RectF (fader_cx - track_w * 0.5f, top, fader_cx + track_w * 0.5f, bot);

	float meter_x = _strip.right - pad - meter_w;
	_meter.layout (D2D1::RectF (meter_x, top, meter_x + meter_w, bot));
}

void
MixerStrip::set_gain (float g)
{
	_gain = std::max (0.f, std::min (1.f, g));
}

void
MixerStrip::set_meter_peak (float peak)
{
	_meter.set_peak (peak);
}

void
MixerStrip::meter_tick (float dt)
{
	_meter.tick (dt);
}

bool
MixerStrip::hit_fader (float x, float y) const
{
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
                  ID2D1SolidColorBrush* meter_hot,
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
	rt->DrawTextW (_label, (UINT32)wcslen (_label), fmt_title,
	               D2D1::RectF (_strip.left, _strip.top + 4.f * dpi_scale,
	                            _strip.right, _strip.top + 26.f * dpi_scale),
	               brand);
	fmt_title->SetTextAlignment (ta);

	rt->FillRectangle (_track, edge);

	float h = _track.bottom - _track.top;
	float knob_y = _track.bottom - _gain * h;
	float knob_h = 12.f * dpi_scale;
	float knob_w = 26.f * dpi_scale;
	float cx = (_track.left + _track.right) * 0.5f;
	D2D1_RECT_F knob = D2D1::RectF (
		cx - knob_w * 0.5f,
		knob_y - knob_h * 0.5f,
		cx + knob_w * 0.5f,
		knob_y + knob_h * 0.5f);
	rt->FillRectangle (knob, accent);

	if (_gain > 0.001f) {
		accent->SetOpacity (0.35f);
		rt->FillRectangle (D2D1::RectF (_track.left, knob_y, _track.right, _track.bottom), accent);
		accent->SetOpacity (1.f);
	}

	_meter.draw (rt, accent, edge, meter_hot, dpi_scale);

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
