/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "mixer_strip.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

bool
contains (const D2D1_RECT_F& r, float x, float y)
{
	return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

} // namespace

MixerStrip::MixerStrip ()
	: _strip ()
	, _track ()
	, _arm_btn ()
	, _fx_btn ()
	, _fx_param_track ()
	, _gain (0.85f)
	, _fx_param (0.3f)
	, _armed (false)
	, _fx_bypassed (false)
{
	wcsncpy_s (_label, L"Track", _TRUNCATE);
	_fx_label[0] = L'\0';
}

void
MixerStrip::set_label (const wchar_t* label)
{
	if (label) {
		wcsncpy_s (_label, label, _TRUNCATE);
	}
}

void
MixerStrip::set_fx_label (const wchar_t* label)
{
	if (label && label[0]) {
		wcsncpy_s (_fx_label, label, _TRUNCATE);
	} else {
		_fx_label[0] = L'\0';
	}
}

void
MixerStrip::set_fx_bypassed (bool bypassed)
{
	_fx_bypassed = bypassed;
}

void
MixerStrip::set_fx_param (float value_0_1)
{
	_fx_param = std::max (0.f, std::min (1.f, value_0_1));
}

void
MixerStrip::layout (const D2D1_RECT_F& strip_bounds, float dpi_scale)
{
	_strip = strip_bounds;

	float pad = 8.f * dpi_scale;
	float arm_h = 22.f * dpi_scale;
	_arm_btn = D2D1::RectF (
		_strip.left + pad,
		_strip.top + 28.f * dpi_scale,
		_strip.right - pad,
		_strip.top + 28.f * dpi_scale + arm_h);

	float fx_h = 18.f * dpi_scale;
	_fx_btn = D2D1::RectF (
		_strip.left + pad,
		_arm_btn.bottom + 4.f * dpi_scale,
		_strip.right - pad,
		_arm_btn.bottom + 4.f * dpi_scale + fx_h);

	float param_h = 10.f * dpi_scale;
	_fx_param_track = D2D1::RectF (
		_strip.left + pad,
		_fx_btn.bottom + 3.f * dpi_scale,
		_strip.right - pad,
		_fx_btn.bottom + 3.f * dpi_scale + param_h);

	float top = _fx_param_track.bottom + 6.f * dpi_scale;
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
MixerStrip::hit_arm (float x, float y) const
{
	return contains (_arm_btn, x, y);
}

bool
MixerStrip::hit_fx (float x, float y) const
{
	return contains (_fx_btn, x, y);
}

bool
MixerStrip::hit_fx_param (float x, float y) const
{
	if (_fx_label[0] == L'\0') {
		return false;
	}
	float pad = 4.f;
	return x >= _fx_param_track.left - pad && x <= _fx_param_track.right + pad
	    && y >= _fx_param_track.top - pad && y <= _fx_param_track.bottom + pad;
}

bool
MixerStrip::drag_fx_param_to (float x)
{
	float w = _fx_param_track.right - _fx_param_track.left;
	if (w < 1.f) {
		return false;
	}
	float t = (x - _fx_param_track.left) / w;
	float v = std::max (0.f, std::min (1.f, t));
	if (v == _fx_param) {
		return false;
	}
	_fx_param = v;
	return true;
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

	rt->FillRectangle (_arm_btn, _armed ? accent : edge);
	DWRITE_TEXT_ALIGNMENT ta_arm = fmt_meta->GetTextAlignment ();
	DWRITE_PARAGRAPH_ALIGNMENT pa_arm = fmt_meta->GetParagraphAlignment ();
	fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
	fmt_meta->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	const wchar_t* arm_label = _armed ? L"R · armed" : L"R · arm";
	rt->DrawTextW (arm_label, (UINT32)wcslen (arm_label), fmt_meta, _arm_btn, brand);

	const bool fx_on = _fx_label[0] != L'\0';
	rt->FillRectangle (_fx_btn, edge);
	if (fx_on) {
		if (_fx_bypassed) {
			edge->SetOpacity (0.85f);
			rt->FillRectangle (_fx_btn, edge);
			edge->SetOpacity (1.f);
			wchar_t byp[48];
			_snwprintf_s (byp, _TRUNCATE, L"BYP %s", _fx_label);
			rt->DrawTextW (byp, (UINT32)wcslen (byp), fmt_meta, _fx_btn, body);
		} else {
			accent->SetOpacity (0.55f);
			rt->FillRectangle (_fx_btn, accent);
			accent->SetOpacity (1.f);
			rt->DrawTextW (_fx_label, (UINT32)wcslen (_fx_label), fmt_meta, _fx_btn, brand);
		}
	} else {
		rt->DrawTextW (L"+ FX", 4, fmt_meta, _fx_btn, brand);
	}

	/* Blend param track (only when FX present). */
	if (fx_on) {
		rt->FillRectangle (_fx_param_track, edge);
		float pw = _fx_param_track.right - _fx_param_track.left;
		float fill_r = _fx_param_track.left + pw * _fx_param;
		ID2D1SolidColorBrush* fill = _fx_bypassed ? body : accent;
		fill->SetOpacity (_fx_bypassed ? 0.35f : 0.7f);
		rt->FillRectangle (
			D2D1::RectF (_fx_param_track.left, _fx_param_track.top, fill_r, _fx_param_track.bottom),
			fill);
		fill->SetOpacity (1.f);
	}

	fmt_meta->SetTextAlignment (ta_arm);
	fmt_meta->SetParagraphAlignment (pa_arm);

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

	wchar_t db[48];
	float db_v = (_gain <= 0.0001f) ? -96.f : (20.f * log10f (_gain));
	if (fx_on) {
		_snwprintf_s (db, _TRUNCATE, L"%.1f dB  ·  B %.0f%%", db_v, _fx_param * 100.f);
	} else {
		_snwprintf_s (db, _TRUNCATE, L"%.1f dB", db_v);
	}
	DWRITE_TEXT_ALIGNMENT ta2 = fmt_meta->GetTextAlignment ();
	fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
	rt->DrawTextW (db, (UINT32)wcslen (db), fmt_meta,
	               D2D1::RectF (_strip.left, _strip.bottom - 28.f * dpi_scale,
	                            _strip.right, _strip.bottom - 6.f * dpi_scale),
	               body);
	fmt_meta->SetTextAlignment (ta2);
}
