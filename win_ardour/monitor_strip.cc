/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "monitor_strip.h"

#include <algorithm>
#include <cstdio>
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
	, _boost_btn ()
	, _aux_track ()
	, _lim_btn ()
	, _available (false)
	, _aux_ok (false)
	, _dim (false)
	, _dim_db (-12.f)
	, _mono (false)
	, _mute (false)
	, _solo_boost_db (0.f)
	, _aux_return (1.f)
	, _master_lim_ok (false)
	, _master_lim_bypassed (false)
{
}

void
MonitorStrip::layout (const D2D1_RECT_F& bounds, float dpi_scale)
{
	_strip = bounds;
	float pad = 8.f * dpi_scale;
	float y = _strip.top + 32.f * dpi_scale;
	float btn_h = 26.f * dpi_scale;
	float gap = 6.f * dpi_scale;
	_dim_btn = D2D1::RectF (_strip.left + pad, y, _strip.right - pad, y + btn_h);
	y = _dim_btn.bottom + gap;
	_mono_btn = D2D1::RectF (_strip.left + pad, y, _strip.right - pad, y + btn_h);
	y = _mono_btn.bottom + gap;
	_mute_btn = D2D1::RectF (_strip.left + pad, y, _strip.right - pad, y + btn_h);
	y = _mute_btn.bottom + gap;
	_boost_btn = D2D1::RectF (_strip.left + pad, y, _strip.right - pad, y + btn_h);
	y = _boost_btn.bottom + gap;
	_lim_btn = D2D1::RectF (_strip.left + pad, y, _strip.right - pad, y + btn_h);
	y = _lim_btn.bottom + gap + 4.f * dpi_scale;
	/* Thin vertical Aux return fader; label sits above the track. */
	float label_h = 16.f * dpi_scale;
	float track_top = y + label_h;
	float track_bot = _strip.bottom - pad;
	float track_w = 10.f * dpi_scale;
	float cx = (_strip.left + _strip.right) * 0.5f;
	if (track_bot > track_top + 24.f * dpi_scale) {
		_aux_track = D2D1::RectF (cx - track_w * 0.5f, track_top,
		                          cx + track_w * 0.5f, track_bot);
	} else {
		_aux_track = D2D1::RectF (0, 0, 0, 0);
	}
}

void
MonitorStrip::set_aux_return (float g)
{
	_aux_return = (g < 0.f) ? 0.f : ((g > 1.f) ? 1.f : g);
}

bool
MonitorStrip::hit_aux_return (float x, float y) const
{
	if (!_aux_ok) {
		return false;
	}
	float pad = 10.f;
	return x >= _aux_track.left - pad && x <= _aux_track.right + pad
	    && y >= _aux_track.top - pad && y <= _aux_track.bottom + pad;
}

bool
MonitorStrip::drag_aux_return_to (float y)
{
	if (!_aux_ok) {
		return false;
	}
	float h = _aux_track.bottom - _aux_track.top;
	if (h < 1.f) {
		return false;
	}
	float t = (_aux_track.bottom - y) / h;
	float g = (t < 0.f) ? 0.f : ((t > 1.f) ? 1.f : t);
	if (g == _aux_return) {
		return false;
	}
	_aux_return = g;
	return true;
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
	if (contains (_boost_btn, x, y)) {
		return MonitorHit::Boost;
	}
	if (hit_aux_return (x, y)) {
		return MonitorHit::AuxReturn;
	}
	if (_master_lim_ok && contains (_lim_btn, x, y)) {
		return MonitorHit::MasterLim;
	}
	return MonitorHit::None;
}

void
MonitorStrip::draw (UiDraw& ui) const
{
	const D2D1_RECT_F& p = _strip;
	const float s = ui.dpi_scale ();
	const float hair = std::max (1.f, s);

	ui.card (p);

	const float header_h = 26.f * s;
	ui.text_vcenter (L"Monitor", ui::TextRole::Section,
	                 _available ? ui::ColorToken::TextPrimary
	                            : ui::ColorToken::TextMuted,
	                 D2D1::RectF (p.left + 12.f * s, p.top,
	                              p.right - 12.f * s, p.top + header_h));
	ui.divider (D2D1::RectF (p.left + 8.f * s, p.top + header_h,
	                         p.right - 8.f * s, p.top + header_h + hair));

	auto draw_btn = [&] (const D2D1_RECT_F& r, const wchar_t* label, bool on) {
		UiDraw::ButtonStyle st;
		st.label = label;
		st.latched = on;
		ui.button (r, st);
	};

	if (_available) {
		wchar_t dim_label[16];
		_snwprintf_s (dim_label, _TRUNCATE, L"Dim %.0f", _dim_db);
		draw_btn (_dim_btn, dim_label, _dim);
		draw_btn (_mono_btn, L"Mono", _mono);
		draw_btn (_mute_btn, L"Mute", _mute);
		wchar_t boost_label[16];
		_snwprintf_s (boost_label, _TRUNCATE, L"Boost +%.0f", _solo_boost_db);
		draw_btn (_boost_btn, boost_label, _solo_boost_db > 0.5f);
		if (_master_lim_ok) {
			/* Lit when limiter is active (not bypassed). */
			draw_btn (_lim_btn, L"MLim", !_master_lim_bypassed);
		}
	} else {
		ui.fill_round (_dim_btn, 4.f, ui::ColorToken::Control, 0.4f);
		ui.stroke_round (_dim_btn, 4.f, ui::ColorToken::Border, 1.f, 0.6f);
		ui.text_vcenter (L"—", ui::TextRole::Body, ui::ColorToken::TextDisabled,
		                 _dim_btn, DWRITE_TEXT_ALIGNMENT_CENTER);
	}

	if (_aux_ok && _aux_track.bottom > _aux_track.top) {
		float label_bot = _aux_track.top - 2.f * s;
		ui.text_vcenter (L"Aux", ui::TextRole::Caption, ui::ColorToken::TextMuted,
		                 D2D1::RectF (p.left, label_bot - 14.f * s, p.right, label_bot),
		                 DWRITE_TEXT_ALIGNMENT_CENTER);
		ui.fill_round (_aux_track, 3.f, ui::ColorToken::Control);
		ui.stroke_round (_aux_track, 3.f, ui::ColorToken::Border, 1.f, 0.8f);
		float h = _aux_track.bottom - _aux_track.top;
		float fill_top = _aux_track.bottom - h * _aux_return;
		if (fill_top < _aux_track.bottom - 1.f) {
			ui.fill_round (D2D1::RectF (_aux_track.left, fill_top,
			                             _aux_track.right, _aux_track.bottom),
			                3.f, ui::ColorToken::Accent, 0.85f);
		}
		float kn_h = 4.f * s;
		ui.fill_round (D2D1::RectF (_aux_track.left - 3.f * s, fill_top - kn_h * 0.5f,
		                             _aux_track.right + 3.f * s, fill_top + kn_h * 0.5f),
		                2.f, ui::ColorToken::TextPrimary, 0.9f);
	}
}
