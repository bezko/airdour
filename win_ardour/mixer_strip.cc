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
	: _color_rgba (0)
	, _strip ()
	, _label_rc ()
	, _track ()
	, _arm_btn ()
	, _mon_btn ()
	, _mute_btn ()
	, _solo_btn ()
	, _pan_track ()
	, _send_track ()
	, _fx_btn ()
	, _fx_bypass_btn ()
	, _fx_param_reset_btn ()
	, _fx_unload_btn ()
	, _fx_param_track ()
	, _fx_param_track2 ()
	, _fx_param_track3 ()
	, _meter_point (2)
	, _meter_type (0)
	, _gain (0.85f)
	, _trim (0.5f)
	, _pan (0.5f)
	, _pan_width (1.f)
	, _pan_width_ok (false)
	, _send (0.f)
	, _send_muted (false)
	, _send_pan (0.5f)
	, _send_pan_ok (false)
	, _fx_param (0.3f)
	, _fx_param2 (0.16f)
	, _fx_param3 (0.5f)
	, _armed (false)
	, _monitor_choice (0)
	, _monitor_peer_count (0)
	, _record_safe (false)
	, _muted (false)
	, _mute_points (0)
	, _soloed (false)
	, _solo_isolated (false)
	, _solo_safe (false)
	, _has_comment (false)
	, _phase_inverted (false)
	, _listen_mode (0)
	, _route_active (true)
	, _fx_bypassed (false)
	, _fx_can_unload (false)
	, _fx_param_count (1)
	, _midi_strip (false)
	, _narrow (false)
	, _in_group (false)
	, _group_picked (false)
	, _group_rgba (0)
	, _selected (false)
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
MixerStrip::set_color (uint32_t rgba_rrggbbaa)
{
	_color_rgba = rgba_rrggbbaa;
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
MixerStrip::set_fx_param_count (int count_1_to_3)
{
	if (count_1_to_3 < 0) {
		count_1_to_3 = 0;
	}
	if (count_1_to_3 > EngineSnapshot::kMaxVst3Params) {
		count_1_to_3 = EngineSnapshot::kMaxVst3Params;
	}
	_fx_param_count = count_1_to_3;
	place_fx_param_resets ();
}

void
MixerStrip::set_fx_can_unload (bool on)
{
	_fx_can_unload = on;
	place_fx_param_resets ();
}

void
MixerStrip::place_fx_param_resets ()
{
	for (int i = 0; i < EngineSnapshot::kMaxVst3Params; ++i) {
		_fx_param_reset_btn[i] = D2D1::RectF (0, 0, 0, 0);
	}
	if (_fx_btn.bottom <= _fx_btn.top || _fx_unload_btn.right <= _fx_unload_btn.left) {
		return;
	}
	const float fx_h = _fx_btn.bottom - _fx_btn.top;
	const float reset_w = fx_h * (11.f / 18.f);
	const float bypass_w = fx_h * (12.f / 18.f);
	int n = 0;
	if (_fx_can_unload) {
		n = _fx_param_count;
		if (n < 0) {
			n = 0;
		}
		if (n > EngineSnapshot::kMaxVst3Params) {
			n = EngineSnapshot::kMaxVst3Params;
		}
	}
	float edge = _fx_unload_btn.left;
	for (int i = n - 1; i >= 0; --i) {
		_fx_param_reset_btn[i] = D2D1::RectF (edge - reset_w, _fx_btn.top,
		                                      edge, _fx_btn.bottom);
		edge = _fx_param_reset_btn[i].left;
	}
	_fx_bypass_btn = D2D1::RectF (edge - bypass_w, _fx_btn.top, edge, _fx_btn.bottom);
}

void
MixerStrip::set_fx_param2 (float value_0_1)
{
	_fx_param2 = std::max (0.f, std::min (1.f, value_0_1));
}

void
MixerStrip::set_fx_param3 (float value_0_1)
{
	_fx_param3 = std::max (0.f, std::min (1.f, value_0_1));
}

void
MixerStrip::set_trim (float trim_0_1)
{
	_trim = std::max (0.f, std::min (1.f, trim_0_1));
}

void
MixerStrip::set_pan (float pan_0_1)
{
	_pan = std::max (0.f, std::min (1.f, pan_0_1));
}

void
MixerStrip::set_pan_width (float width_m1_to_1)
{
	_pan_width = std::max (-1.f, std::min (1.f, width_m1_to_1));
}

void
MixerStrip::set_send (float send_0_1)
{
	_send = std::max (0.f, std::min (1.f, send_0_1));
}

void
MixerStrip::set_send_pan (float pan_0_1)
{
	_send_pan = std::max (0.f, std::min (1.f, pan_0_1));
}

void
MixerStrip::layout (const D2D1_RECT_F& strip_bounds, float dpi_scale)
{
	_strip = strip_bounds;

	float pad = 8.f * dpi_scale;
	_label_rc = D2D1::RectF (
		_strip.left, _strip.top, _strip.right, _strip.top + 26.f * dpi_scale);
	float y = _strip.top + 28.f * dpi_scale;
	float arm_h = 22.f * dpi_scale;
	float ms_h = 22.f * dpi_scale;
	float gap = 4.f * dpi_scale;

	/* Arm is live for audio + MIDI (MIDI rec-enable). */
	_arm_btn = D2D1::RectF (
		_strip.left + pad, y, _strip.right - pad, y + arm_h);
	y = _arm_btn.bottom + gap;
	float mid = (_strip.left + _strip.right) * 0.5f;
	if (_midi_strip) {
		_arm_btn = D2D1::RectF (0, 0, 0, 0);
		_mon_btn = D2D1::RectF (0, 0, 0, 0);
		_pan_track = D2D1::RectF (0, 0, 0, 0);
		_send_track = D2D1::RectF (0, 0, 0, 0);
	} else {
		_arm_btn = D2D1::RectF (
			_strip.left + pad, y, mid - gap * 0.5f, y + arm_h);
		_mon_btn = D2D1::RectF (
			mid + gap * 0.5f, y, _strip.right - pad, y + arm_h);
		y = _arm_btn.bottom + gap;
	}

	_mute_btn = D2D1::RectF (_strip.left + pad, y, mid - gap * 0.5f, y + ms_h);
	_solo_btn = D2D1::RectF (mid + gap * 0.5f, y, _strip.right - pad, y + ms_h);
	y = _mute_btn.bottom + gap;

	float fx_h = 18.f * dpi_scale;
	if (_midi_strip || _narrow) {
		_trim_track = D2D1::RectF (0, 0, 0, 0);
		_pan_track = D2D1::RectF (0, 0, 0, 0);
		_send_track = D2D1::RectF (0, 0, 0, 0);
		_fx_btn = D2D1::RectF (0, 0, 0, 0);
		_fx_bypass_btn = D2D1::RectF (0, 0, 0, 0);
		_fx_unload_btn = D2D1::RectF (0, 0, 0, 0);
		place_fx_param_resets ();
		_fx_param_track = D2D1::RectF (0, 0, 0, 0);
		_fx_param_track2 = D2D1::RectF (0, 0, 0, 0);
		_fx_param_track3 = D2D1::RectF (0, 0, 0, 0);
	} else {
		float trim_h = 10.f * dpi_scale;
		_trim_track = D2D1::RectF (_strip.left + pad, y, _strip.right - pad, y + trim_h);
		y = _trim_track.bottom + gap;

		float pan_h = 12.f * dpi_scale;
		_pan_track = D2D1::RectF (_strip.left + pad, y, _strip.right - pad, y + pan_h);
		y = _pan_track.bottom + gap;

		float send_h = 10.f * dpi_scale;
		_send_track = D2D1::RectF (_strip.left + pad, y, _strip.right - pad, y + send_h);
		y = _send_track.bottom + gap;

		_fx_btn = D2D1::RectF (_strip.left + pad, y, _strip.right - pad, y + fx_h);
		float unload_w = 14.f * dpi_scale;
		_fx_unload_btn = D2D1::RectF (_fx_btn.right - unload_w, _fx_btn.top,
		                              _fx_btn.right, _fx_btn.bottom);
		place_fx_param_resets ();
		y = _fx_btn.bottom + 3.f * dpi_scale;
		float param_h = 8.f * dpi_scale;
		/* Always reserve 3 param tracks so EQ / Comp fit without re-layout. */
		_fx_param_track = D2D1::RectF (_strip.left + pad, y, _strip.right - pad, y + param_h);
		y = _fx_param_track.bottom + 2.f * dpi_scale;
		_fx_param_track2 = D2D1::RectF (_strip.left + pad, y, _strip.right - pad, y + param_h);
		y = _fx_param_track2.bottom + 2.f * dpi_scale;
		_fx_param_track3 = D2D1::RectF (_strip.left + pad, y, _strip.right - pad, y + param_h);
		y = _fx_param_track3.bottom + 6.f * dpi_scale;
	}

	float top = y;
	float bot = _strip.bottom - 36.f * dpi_scale;
	float inner_w = _strip.right - _strip.left - pad * 2.f;
	float fader_w = inner_w * 0.42f;
	float meter_w = 12.f * dpi_scale;

	float fader_cx = _strip.left + pad + fader_w * 0.5f;
	float track_w = 10.f * dpi_scale;
	_track = D2D1::RectF (fader_cx - track_w * 0.5f, top, fader_cx + track_w * 0.5f, bot);

	float meter_x = _strip.right - pad - meter_w;
	_meter.layout (D2D1::RectF (meter_x, top, meter_x + meter_w, bot));
	_meter.set_clip_led_height (std::max (3.f, 4.f * dpi_scale));
}

void
MixerStrip::set_gain (float g)
{
	_gain = std::max (0.f, std::min (1.f, g));
}

void
MixerStrip::set_meter_peak (float peak, bool clipped)
{
	_meter.set_peak (peak);
	if (clipped) {
		_meter.set_clipped (true);
	}
}

void
MixerStrip::meter_tick (float dt)
{
	_meter.tick (dt);
}

void
MixerStrip::clear_meter_clip ()
{
	_meter.clear_clip ();
}

bool
MixerStrip::hit_meter (float x, float y) const
{
	return _meter.hit (x, y);
}

bool
MixerStrip::hit_meter_clip_led (float x, float y) const
{
	return _meter.hit_clip_led (x, y);
}

void
MixerStrip::set_meter_point (int point_0_3)
{
	if (point_0_3 < 0) {
		point_0_3 = 0;
	}
	if (point_0_3 > 3) {
		point_0_3 = 3;
	}
	_meter_point = point_0_3;
}

void
MixerStrip::set_meter_type (int type_0_1)
{
	_meter_type = (type_0_1 == 1) ? 1 : 0;
}

bool
MixerStrip::hit_label (float x, float y) const
{
	return contains (_label_rc, x, y);
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
MixerStrip::hit_mon (float x, float y) const
{
	if (_midi_strip) {
		return false;
	}
	return contains (_mon_btn, x, y);
}

void
MixerStrip::set_monitor_choice (int choice_0_1_2)
{
	_monitor_choice = (choice_0_1_2 < 0) ? 0 : ((choice_0_1_2 > 2) ? 2 : choice_0_1_2);
}

bool
MixerStrip::hit_mute (float x, float y) const
{
	return contains (_mute_btn, x, y);
}

bool
MixerStrip::hit_solo (float x, float y) const
{
	return contains (_solo_btn, x, y);
}

bool
MixerStrip::hit_trim (float x, float y) const
{
	if (_midi_strip) {
		return false;
	}
	float pad = 4.f;
	return x >= _trim_track.left - pad && x <= _trim_track.right + pad
	    && y >= _trim_track.top - pad && y <= _trim_track.bottom + pad;
}

bool
MixerStrip::drag_trim_to (float x)
{
	if (_midi_strip) {
		return false;
	}
	float w = _trim_track.right - _trim_track.left;
	if (w <= 1.f) {
		return false;
	}
	float n = (x - _trim_track.left) / w;
	n = std::max (0.f, std::min (1.f, n));
	if (std::fabs (n - _trim) < 0.0005f) {
		return false;
	}
	_trim = n;
	return true;
}

bool
MixerStrip::hit_pan (float x, float y) const
{
	if (_midi_strip || _narrow) {
		return false;
	}
	float pad = 4.f;
	return x >= _pan_track.left - pad && x <= _pan_track.right + pad
	    && y >= _pan_track.top - pad && y <= _pan_track.bottom + pad;
}

bool
MixerStrip::drag_pan_to (float x)
{
	if (_midi_strip || _narrow) {
		return false;
	}
	float w = _pan_track.right - _pan_track.left;
	if (w < 1.f) {
		return false;
	}
	float t = (x - _pan_track.left) / w;
	float v = std::max (0.f, std::min (1.f, t));
	if (v == _pan) {
		return false;
	}
	_pan = v;
	return true;
}

bool
MixerStrip::drag_pan_width_to (float x)
{
	if (_midi_strip || !_pan_width_ok) {
		return false;
	}
	float w = _pan_track.right - _pan_track.left;
	if (w < 1.f) {
		return false;
	}
	float t = (x - _pan_track.left) / w;
	float iface = std::max (0.f, std::min (1.f, t));
	/* ParameterDescriptor::from_interface PanWidth: 2v−1 */
	float width = 2.f * iface - 1.f;
	if (width == _pan_width) {
		return false;
	}
	_pan_width = width;
	return true;
}

bool
MixerStrip::hit_send (float x, float y) const
{
	if (_midi_strip || _narrow) {
		return false;
	}
	float pad = 4.f;
	return x >= _send_track.left - pad && x <= _send_track.right + pad
	    && y >= _send_track.top - pad && y <= _send_track.bottom + pad;
}

bool
MixerStrip::drag_send_to (float x)
{
	if (_midi_strip || _narrow) {
		return false;
	}
	float w = _send_track.right - _send_track.left;
	if (w < 1.f) {
		return false;
	}
	float t = (x - _send_track.left) / w;
	float v = std::max (0.f, std::min (1.f, t));
	if (v == _send) {
		return false;
	}
	_send = v;
	return true;
}

bool
MixerStrip::drag_send_pan_to (float x)
{
	if (_midi_strip || !_send_pan_ok) {
		return false;
	}
	float w = _send_track.right - _send_track.left;
	if (w < 1.f) {
		return false;
	}
	float t = (x - _send_track.left) / w;
	float v = std::max (0.f, std::min (1.f, t));
	if (v == _send_pan) {
		return false;
	}
	_send_pan = v;
	return true;
}

bool
MixerStrip::hit_fx (float x, float y) const
{
	if (_midi_strip || _narrow) {
		return false;
	}
	if (_fx_can_unload && _fx_label[0] != L'\0') {
		if (contains (_fx_unload_btn, x, y) || contains (_fx_bypass_btn, x, y)
		    || hit_fx_param_reset (x, y) >= 0) {
			return false; /* owned by hit_fx_unload / bypass / reset chips */
		}
	}
	return contains (_fx_btn, x, y);
}

bool
MixerStrip::hit_fx_unload (float x, float y) const
{
	if (_midi_strip || _narrow || !_fx_can_unload || _fx_label[0] == L'\0') {
		return false;
	}
	return contains (_fx_unload_btn, x, y);
}

bool
MixerStrip::hit_fx_bypass (float x, float y) const
{
	if (_midi_strip || _narrow || !_fx_can_unload || _fx_label[0] == L'\0') {
		return false;
	}
	return _fx_bypass_btn.right > _fx_bypass_btn.left && contains (_fx_bypass_btn, x, y);
}

int
MixerStrip::hit_fx_param_reset (float x, float y) const
{
	if (_midi_strip || _narrow || !_fx_can_unload || _fx_label[0] == L'\0') {
		return -1;
	}
	const int n = (_fx_param_count < EngineSnapshot::kMaxVst3Params)
	                  ? _fx_param_count
	                  : EngineSnapshot::kMaxVst3Params;
	for (int i = 0; i < n; ++i) {
		const D2D1_RECT_F& r = _fx_param_reset_btn[i];
		if (r.right > r.left && contains (r, x, y)) {
			return i;
		}
	}
	return -1;
}

bool
MixerStrip::hit_fx_param (float x, float y) const
{
	if (_midi_strip || _narrow || _fx_label[0] == L'\0') {
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
MixerStrip::hit_fx_param2 (float x, float y) const
{
	if (_midi_strip || _narrow || _fx_param_count < 2 || _fx_label[0] == L'\0') {
		return false;
	}
	float pad = 4.f;
	return x >= _fx_param_track2.left - pad && x <= _fx_param_track2.right + pad
	    && y >= _fx_param_track2.top - pad && y <= _fx_param_track2.bottom + pad;
}

bool
MixerStrip::drag_fx_param2_to (float x)
{
	float w = _fx_param_track2.right - _fx_param_track2.left;
	if (w < 1.f) {
		return false;
	}
	float t = (x - _fx_param_track2.left) / w;
	float v = std::max (0.f, std::min (1.f, t));
	if (v == _fx_param2) {
		return false;
	}
	_fx_param2 = v;
	return true;
}

bool
MixerStrip::hit_fx_param3 (float x, float y) const
{
	if (_midi_strip || _narrow || _fx_param_count < 3 || _fx_label[0] == L'\0') {
		return false;
	}
	float pad = 4.f;
	return x >= _fx_param_track3.left - pad && x <= _fx_param_track3.right + pad
	    && y >= _fx_param_track3.top - pad && y <= _fx_param_track3.bottom + pad;
}

bool
MixerStrip::drag_fx_param3_to (float x)
{
	float w = _fx_param_track3.right - _fx_param_track3.left;
	if (w < 1.f) {
		return false;
	}
	float t = (x - _fx_param_track3.left) / w;
	float v = std::max (0.f, std::min (1.f, t));
	if (v == _fx_param3) {
		return false;
	}
	_fx_param3 = v;
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
MixerStrip::draw (UiDraw& ui) const
{
	ID2D1RenderTarget* rt = ui.rt ();
	if (!rt) {
		return;
	}
	const float s = ui.dpi_scale ();
	const float hair = std::max (1.f, s);

	ui.fill_round (_strip, 6.f, ui::ColorToken::Card);
	ui.stroke_round (_strip, 6.f, ui::ColorToken::Border, 1.f, 0.9f);

	/* Route colour strip along the left edge. */
	{
		ID2D1SolidColorBrush* bar = (!_route_active || _muted)
			? ui.brush (ui::ColorToken::TextDisabled)
			: ui.brush (ui::ColorToken::Accent);
		ID2D1SolidColorBrush* owned = nullptr;
		if (_route_active && !_muted && _color_rgba != 0) {
			float r = (float)((_color_rgba >> 24) & 0xff) / 255.f;
			float g = (float)((_color_rgba >> 16) & 0xff) / 255.f;
			float b = (float)((_color_rgba >> 8) & 0xff) / 255.f;
			float a = (float)(_color_rgba & 0xff) / 255.f;
			if (SUCCEEDED (rt->CreateSolidColorBrush (D2D1::ColorF (r, g, b, a), &owned))
			    && owned) {
				bar = owned;
			}
		}
		if (bar) {
			const float bar_w = 3.f * s;
			rt->FillRoundedRectangle (
				D2D1::RoundedRect (
					D2D1::RectF (_strip.left + 1.f * s, _strip.top + 8.f * s,
					             _strip.left + 1.f * s + bar_w, _strip.bottom - 8.f * s),
					bar_w * 0.5f, bar_w * 0.5f),
				bar);
		}
		if (owned) {
			owned->Release ();
		}
	}

	/* Group indicator: top accent pill + "G"; pick shows a quieter bar.
	 * When RouteGroup shares color, the bar uses group RRGGBBAA. */
	if (_in_group || _group_picked) {
		ID2D1SolidColorBrush* bar = _in_group ? ui.brush (ui::ColorToken::Accent)
		                                      : ui.brush (ui::ColorToken::BorderStrong);
		ID2D1SolidColorBrush* owned = nullptr;
		if (_in_group && _group_rgba != 0) {
			float cr = ((float)((_group_rgba >> 24) & 0xff)) / 255.f;
			float cg = ((float)((_group_rgba >> 16) & 0xff)) / 255.f;
			float cb = ((float)((_group_rgba >> 8) & 0xff)) / 255.f;
			float ca = ((float)(_group_rgba & 0xff)) / 255.f;
			if (SUCCEEDED (rt->CreateSolidColorBrush (
				    D2D1::ColorF (cr, cg, cb, ca), &owned)) && owned) {
				bar = owned;
			}
		}
		if (bar) {
			rt->FillRoundedRectangle (
				D2D1::RoundedRect (
					D2D1::RectF (_strip.left + 8.f * s, _strip.top + 2.f * s,
					             _strip.right - 8.f * s, _strip.top + 5.f * s),
					1.5f, 1.5f),
				bar);
		}
		if (_in_group) {
			ui.text_vcenter (L"G", ui::TextRole::Caption, ui::ColorToken::Accent,
			                 D2D1::RectF (_strip.right - 18.f * s, _strip.top + 2.f * s,
			                              _strip.right - 5.f * s, _strip.top + 18.f * s),
			                 DWRITE_TEXT_ALIGNMENT_TRAILING);
		}
		if (owned) {
			owned->Release ();
		}
	}
	/* Selection wash (group select share lights peers). */
	if (_selected) {
		ui.fill_round (_strip, 6.f, ui::ColorToken::Accent, 0.10f);
	}

	if (_has_comment && !_midi_strip) {
		/* Cm badge — Route::comment non-empty. */
		D2D1_RECT_F badge = D2D1::RectF (
			_label_rc.right - 20.f * s, _label_rc.top + 3.f * s,
			_label_rc.right - 4.f * s, _label_rc.top + 17.f * s);
		ui.fill_round (badge, 3.f, ui::ColorToken::Accent, 0.7f);
		ui.text_vcenter (L"Cm", ui::TextRole::Caption, ui::ColorToken::AccentInk,
		                 badge, DWRITE_TEXT_ALIGNMENT_CENTER);
	}
	if (_phase_inverted && !_midi_strip) {
		/* Ø badge — polarity inverted (PhaseControl). */
		D2D1_RECT_F badge = D2D1::RectF (
			_label_rc.left + 4.f * s, _label_rc.top + 3.f * s,
			_label_rc.left + 18.f * s, _label_rc.top + 17.f * s);
		ui.fill_round (badge, 3.f, ui::ColorToken::Accent, 0.75f);
		ui.text_vcenter (L"\x00D8", ui::TextRole::Caption,
		                 ui::ColorToken::AccentInk, badge,
		                 DWRITE_TEXT_ALIGNMENT_CENTER);
	}
	if (_route_active) {
		ui.text_vcenter (_label, ui::TextRole::Section,
		                 ui::ColorToken::TextPrimary, _label_rc,
		                 DWRITE_TEXT_ALIGNMENT_CENTER);
	} else {
		wchar_t off_lab[48];
		_snwprintf_s (off_lab, _TRUNCATE, L"%s · Off", _label);
		ui.text_vcenter (off_lab, ui::TextRole::Section,
		                 ui::ColorToken::TextMuted, _label_rc,
		                 DWRITE_TEXT_ALIGNMENT_CENTER);
	}

	/* Compact chip button: quiet idle, tinted when latched. */
	auto chip = [&] (const D2D1_RECT_F& r, const wchar_t* label, bool lit,
	                 ui::ColorToken tint) {
		if (r.right <= r.left || r.bottom <= r.top) {
			return;
		}
		if (lit) {
			ui.fill_round (r, 4.f, tint, 0.92f);
			ui.text_vcenter (label, ui::TextRole::Caption,
			                 ui::ColorToken::AccentInk, r,
			                 DWRITE_TEXT_ALIGNMENT_CENTER);
		} else {
			ui.fill_round (r, 4.f, ui::ColorToken::Control, 0.85f);
			ui.stroke_round (r, 4.f, ui::ColorToken::Border, 1.f, 0.7f);
			ui.text_vcenter (label, ui::TextRole::Caption,
			                 ui::ColorToken::TextSecondary, r,
			                 DWRITE_TEXT_ALIGNMENT_CENTER);
		}
	};

	const bool compact_btns = (_arm_btn.right - _arm_btn.left) < 44.f * s;
	{
		const wchar_t* arm_label = compact_btns
			? (_record_safe ? L"Rs" : L"R")
			: (_record_safe ? L"R · Rs"
			                : (_armed ? L"R · armed" : L"R · arm"));
		chip (_arm_btn, arm_label, _armed || _record_safe,
		      ui::ColorToken::Record);
	}
	if (!_midi_strip) {
		/* Mon — Auto / In / Disk; ·N peer badge when group shares monitoring. */
		const bool mon_forced = _monitor_choice != 0 || _monitor_peer_count > 0;
		const wchar_t* base =
			(_monitor_choice == 1) ? L"In"
			: (_monitor_choice == 2) ? L"Disk" : L"Auto";
		wchar_t mon_label[16];
		if (compact_btns) {
			_snwprintf_s (mon_label, _TRUNCATE, L"%s",
			              (_monitor_choice == 1) ? L"I"
			              : (_monitor_choice == 2) ? L"D" : L"A");
		} else if (_monitor_peer_count > 0) {
			_snwprintf_s (mon_label, _TRUNCATE, L"%s·%d", base,
			              _monitor_peer_count + 1);
		} else {
			_snwprintf_s (mon_label, _TRUNCATE, L"%s", base);
		}
		chip (_mon_btn, mon_label, mon_forced, ui::ColorToken::Accent);
	}

	/* Mute — lit red wash when muted; All badge when mute-points=AllPoints. */
	{
		const wchar_t* mlab = compact_btns ? L"M"
		                                    : ((_mute_points != 0) ? L"M·All" : L"M");
		chip (_mute_btn, mlab, _muted, ui::ColorToken::Mute);
	}

	/* Solo — amber when self/effective solo; Si when isolated, Ss when safe. */
	{
		const wchar_t* s_lab = _solo_isolated ? L"Si" : (_solo_safe ? L"Ss" : L"S");
		const wchar_t* solo_glyph = s_lab;
		if (!_solo_isolated && !_solo_safe && !compact_btns) {
			if (_listen_mode == 1) {
				solo_glyph = L"P";
			} else if (_listen_mode == 2) {
				solo_glyph = L"A";
			}
		}
		const bool lonely_solo = _solo_isolated || _solo_safe;
		chip (_solo_btn, solo_glyph, _soloed || lonely_solo,
		      ui::ColorToken::Solo);
	}

	if (!_midi_strip) {
		/* Trim track — −20 · 0 · +20 dB (knob at unity when centered). */
		if (_trim_track.right > _trim_track.left) {
			const float th = _trim_track.bottom - _trim_track.top;
			const float radius = th * 0.5f / s;
			ui.fill_round (_trim_track, radius, ui::ColorToken::Control);
			ui.stroke_round (_trim_track, radius, ui::ColorToken::Border, 1.f, 0.7f);
			const float mid_x = (_trim_track.left + _trim_track.right) * 0.5f;
			rt->DrawLine (D2D1::Point2F (mid_x, _trim_track.top),
			              D2D1::Point2F (mid_x, _trim_track.bottom),
			              ui.brush (ui::ColorToken::BorderStrong), 0.8f);
			const float tw = _trim_track.right - _trim_track.left;
			const float kn_cx = _trim_track.left + tw * _trim;
			const float kn_w = 7.f * s;
			ui.fill_round (D2D1::RectF (kn_cx - kn_w * 0.5f, _trim_track.top - s,
			                             kn_cx + kn_w * 0.5f, _trim_track.bottom + s),
			               2.f, ui::ColorToken::Accent);
		}

		if (!_narrow) {
			/* Pan azimuth track — L · C · R; stereo width band when available. */
			if (_pan_track.right > _pan_track.left) {
				const float ph = _pan_track.bottom - _pan_track.top;
				const float radius = ph * 0.5f / s;
				ui.fill_round (_pan_track, radius, ui::ColorToken::Control);
				ui.stroke_round (_pan_track, radius, ui::ColorToken::Border, 1.f, 0.7f);
				const float pw = _pan_track.right - _pan_track.left;
				const float kn_cx = _pan_track.left + pw * _pan;
				if (_pan_width_ok) {
					float half = std::fabs (_pan_width) * 0.5f * pw;
					float band_l = std::max (_pan_track.left, kn_cx - half);
					float band_r = std::min (_pan_track.right, kn_cx + half);
					ui.fill_round (D2D1::RectF (band_l, _pan_track.top,
					                             band_r, _pan_track.bottom),
					               radius, ui::ColorToken::Accent, 0.28f);
				}
				const float mid_x = (_pan_track.left + _pan_track.right) * 0.5f;
				rt->DrawLine (D2D1::Point2F (mid_x, _pan_track.top),
				              D2D1::Point2F (mid_x, _pan_track.bottom),
				              ui.brush (ui::ColorToken::BorderStrong), 0.8f);
				const float kn_w = 8.f * s;
				ui.fill_round (D2D1::RectF (kn_cx - kn_w * 0.5f, _pan_track.top - s,
				                             kn_cx + kn_w * 0.5f, _pan_track.bottom + s),
				               2.f, ui::ColorToken::Accent);
				if (_pan_width_ok) {
					/* Thin W badge — Shift+drag pan adjusts stereo width. */
					ui.text_vcenter (L"W", ui::TextRole::Caption,
					                 ui::ColorToken::TextMuted,
					                 D2D1::RectF (_pan_track.right - 14.f * s,
					                              _pan_track.top - 2.f * s,
					                              _pan_track.right + 2.f * s,
					                              _pan_track.bottom + 2.f * s),
					                 DWRITE_TEXT_ALIGNMENT_TRAILING);
				}
			}

			/* Aux send level → Aux bus; pan knob when send panner present. */
			if (_send_track.right > _send_track.left) {
				const float sh = _send_track.bottom - _send_track.top;
				const float radius = sh * 0.5f / s;
				ui.fill_round (_send_track, radius, ui::ColorToken::Control);
				ui.stroke_round (_send_track, radius, ui::ColorToken::Border, 1.f, 0.7f);
				const float sw = _send_track.right - _send_track.left;
				const float fill_r = _send_track.left + sw * _send;
				if (fill_r > _send_track.left + 1.f) {
					ui.fill_round (D2D1::RectF (_send_track.left, _send_track.top,
					                             fill_r, _send_track.bottom),
					               radius, ui::ColorToken::Accent,
					               _send_muted ? 0.22f : 0.55f);
				}
				if (_send_muted) {
					ui.text_vcenter (L"Sm", ui::TextRole::Caption,
					                 ui::ColorToken::TextPrimary, _send_track,
					                 DWRITE_TEXT_ALIGNMENT_CENTER);
				}
				if (_send_pan_ok) {
					const float kn_cx = _send_track.left + sw * _send_pan;
					const float kn_w = 6.f * s;
					ui.fill_round (D2D1::RectF (kn_cx - kn_w * 0.5f, _send_track.top - s,
					                             kn_cx + kn_w * 0.5f, _send_track.bottom + s),
					               2.f, ui::ColorToken::TextPrimary, 0.85f);
					ui.text_vcenter (L"P", ui::TextRole::Caption,
					                 ui::ColorToken::TextMuted,
					                 D2D1::RectF (_send_track.right - 12.f * s,
					                              _send_track.top - 2.f * s,
					                              _send_track.right + 2.f * s,
					                              _send_track.bottom + 2.f * s),
					                 DWRITE_TEXT_ALIGNMENT_TRAILING);
				}
			}

			const bool fx_on = _fx_label[0] != L'\0';
			if (_fx_btn.right > _fx_btn.left) {
				D2D1_RECT_F fx_label_rc = _fx_btn;
				if (fx_on && _fx_can_unload && _fx_bypass_btn.right > _fx_bypass_btn.left) {
					fx_label_rc.right = _fx_bypass_btn.left;
				}
				if (fx_on) {
					if (_fx_bypassed) {
						ui.fill_round (_fx_btn, 4.f, ui::ColorToken::Punch, 0.18f);
						ui.stroke_round (_fx_btn, 4.f, ui::ColorToken::Punch, 1.f, 0.55f);
						wchar_t byp[48];
						_snwprintf_s (byp, _TRUNCATE, L"BYP %s", _fx_label);
						ui.text_vcenter (byp, ui::TextRole::Caption,
						                 ui::ColorToken::TextSecondary, fx_label_rc,
						                 DWRITE_TEXT_ALIGNMENT_CENTER);
					} else {
						ui.fill_round (_fx_btn, 4.f, ui::ColorToken::Accent, 0.16f);
						ui.stroke_round (_fx_btn, 4.f, ui::ColorToken::Accent, 1.f, 0.6f);
						ui.text_vcenter (_fx_label, ui::TextRole::Caption,
						                 ui::ColorToken::Accent, fx_label_rc,
						                 DWRITE_TEXT_ALIGNMENT_CENTER);
					}
					if (_fx_can_unload) {
						if (_fx_bypass_btn.right > _fx_bypass_btn.left) {
							if (_fx_bypassed) {
								ui.fill_round (_fx_bypass_btn, 3.f, ui::ColorToken::Accent, 0.35f);
							} else {
								ui.fill_round (_fx_bypass_btn, 3.f, ui::ColorToken::Control, 0.7f);
							}
							ui.text_vcenter (L"B", ui::TextRole::Caption,
							                 ui::ColorToken::Accent, _fx_bypass_btn,
							                 DWRITE_TEXT_ALIGNMENT_CENTER);
						}
						for (int i = 0; i < EngineSnapshot::kMaxVst3Params; ++i) {
							const D2D1_RECT_F& r = _fx_param_reset_btn[i];
							if (r.right > r.left) {
								const wchar_t lab[2] = { (wchar_t)(L'1' + i), L'\0' };
								ui.fill_round (r, 3.f, ui::ColorToken::Control, 0.7f);
								ui.text_vcenter (lab, ui::TextRole::Caption,
								                 ui::ColorToken::Accent, r,
								                 DWRITE_TEXT_ALIGNMENT_CENTER);
							}
						}
						if (_fx_unload_btn.right > _fx_unload_btn.left) {
							ui.fill_round (_fx_unload_btn, 3.f, ui::ColorToken::Control, 0.7f);
							ui.text_vcenter (L"×", ui::TextRole::Caption,
							                 ui::ColorToken::Accent, _fx_unload_btn,
							                 DWRITE_TEXT_ALIGNMENT_CENTER);
						}
					}
				} else {
					ui.fill_round (_fx_btn, 4.f, ui::ColorToken::Control, 0.55f);
					ui.stroke_round (_fx_btn, 4.f, ui::ColorToken::Border, 1.f, 0.7f);
					ui.text_vcenter (L"+ FX", ui::TextRole::Caption,
					                 ui::ColorToken::TextMuted, _fx_btn,
					                 DWRITE_TEXT_ALIGNMENT_CENTER);
				}
			}

			if (fx_on) {
				auto draw_param = [&] (const D2D1_RECT_F& track, float value, float opac) {
					if (track.right <= track.left) {
						return;
					}
					const float radius = (track.bottom - track.top) * 0.5f / s;
					ui.fill_round (track, radius, ui::ColorToken::Control, 0.8f);
					const float pw = track.right - track.left;
					const float fill_r = track.left + pw * value;
					if (fill_r > track.left + 0.5f) {
						ui.fill_round (D2D1::RectF (track.left, track.top,
						                             fill_r, track.bottom),
						               radius,
						               _fx_bypassed ? ui::ColorToken::TextMuted
						                            : ui::ColorToken::Accent,
						               _fx_bypassed ? 0.35f : opac);
					}
				};
				draw_param (_fx_param_track, _fx_param, 0.7f);
				if (_fx_param_count >= 2) {
					draw_param (_fx_param_track2, _fx_param2, 0.55f);
				}
				if (_fx_param_count >= 3) {
					draw_param (_fx_param_track3, _fx_param3, 0.45f);
				}
			}
		}
	}

	/* Fader track + accent fill + chunky knob. */
	{
		const float tw = _track.right - _track.left;
		const float radius = tw * 0.5f / s;
		ui.fill_round (_track, radius, ui::ColorToken::Control);
		ui.stroke_round (_track, radius, ui::ColorToken::Border, 1.f, 0.7f);

		float h = _track.bottom - _track.top;
		float knob_y = _track.bottom - _gain * h;
		float knob_h = 12.f * s;
		float knob_w = 26.f * s;
		float cx = (_track.left + _track.right) * 0.5f;
		D2D1_RECT_F knob = D2D1::RectF (
			cx - knob_w * 0.5f,
			knob_y - knob_h * 0.5f,
			cx + knob_w * 0.5f,
			knob_y + knob_h * 0.5f);

		if (_gain > 0.001f) {
			ui.fill_round (D2D1::RectF (_track.left, knob_y,
			                             _track.right, _track.bottom),
			               radius,
			               _muted ? ui::ColorToken::TextMuted
			                      : ui::ColorToken::Accent,
			               0.30f);
		}
		ui.fill_round (knob, 4.f,
		               _muted ? ui::ColorToken::TextMuted
		                      : ui::ColorToken::Accent);
		ui.fill_round (D2D1::RectF (knob.left + 4.f * s, knob_y - s,
		                             knob.right - 4.f * s, knob_y + s),
		               1.f, ui::ColorToken::AccentInk, 0.55f);
	}

	_meter.draw (ui);
	if (!_midi_strip) {
		const wchar_t* mp = L"Pst";
		switch (_meter_point) {
		case 0: mp = L"In"; break;
		case 1: mp = L"Pre"; break;
		case 3: mp = L"Out"; break;
		default: mp = L"Pst"; break;
		}
		const D2D1_RECT_F& mb = _meter.bounds ();
		ui.text_vcenter (mp, ui::TextRole::Caption, ui::ColorToken::TextMuted,
		                 D2D1::RectF (mb.left - 6.f * s, mb.top - 14.f * s,
		                              mb.right + 6.f * s, mb.top - 1.f * s),
		                 DWRITE_TEXT_ALIGNMENT_CENTER);
		const wchar_t* mt = (_meter_type == 1) ? L"K14" : L"Pk";
		const D2D1_RECT_F& mb2 = _meter.bounds ();
		ui.text_vcenter (mt, ui::TextRole::Caption, ui::ColorToken::TextMuted,
		                 D2D1::RectF (mb2.left - 6.f * s, mb2.bottom + 1.f * s,
		                              mb2.right + 6.f * s, mb2.bottom + 14.f * s),
		                 DWRITE_TEXT_ALIGNMENT_CENTER);
	}

	wchar_t db[48];
	float db_v = (_gain <= 0.0001f) ? -96.f : (20.f * log10f (_gain));
	if (_narrow && !_midi_strip) {
		_snwprintf_s (db, _TRUNCATE, L"%.0f", db_v);
	} else if (_muted && _soloed) {
		_snwprintf_s (db, _TRUNCATE, L"%.1f dB · M+S", db_v);
	} else if (_muted) {
		_snwprintf_s (db, _TRUNCATE, L"%.1f dB · MUTE", db_v);
	} else if (_soloed) {
		_snwprintf_s (db, _TRUNCATE, L"%.1f dB · SOLO", db_v);
	} else if (!_route_active) {
		_snwprintf_s (db, _TRUNCATE, L"%.1f dB · OFF", db_v);
	} else {
		_snwprintf_s (db, _TRUNCATE, L"%.1f dB", db_v);
	}
	ui.text_vcenter (db, ui::TextRole::Caption,
	                 _muted ? ui::ColorToken::Mute
	                 : (_soloed ? ui::ColorToken::Solo
	                            : ui::ColorToken::TextSecondary),
	                 D2D1::RectF (_strip.left, _strip.bottom - 28.f * s,
	                              _strip.right, _strip.bottom - 6.f * s),
	                 DWRITE_TEXT_ALIGNMENT_CENTER);
}
