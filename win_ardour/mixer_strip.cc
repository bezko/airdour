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
	if (count_1_to_3 < 1) {
		count_1_to_3 = 1;
	}
	if (count_1_to_3 > 3) {
		count_1_to_3 = 3;
	}
	_fx_param_count = count_1_to_3;
}

void
MixerStrip::set_fx_dual (bool dual)
{
	_fx_param_count = dual ? 2 : 1;
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
	return contains (_fx_btn, x, y);
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
	{
		ID2D1SolidColorBrush* bar = (!_route_active || _muted) ? body : accent;
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
		rt->FillRectangle (
			D2D1::RectF (_strip.left, _strip.top, _strip.left + 3.f * dpi_scale, _strip.bottom),
			bar);
		if (owned) {
			owned->Release ();
		}
	}

	/* Group indicator: top accent bar + "G"; pick shows a second edge wash.
	 * When RouteGroup shares color, bar uses group RRGGBBAA. */
	if (_in_group || _group_picked) {
		ID2D1SolidColorBrush* bar = _in_group ? accent : edge;
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
		rt->FillRectangle (
			D2D1::RectF (_strip.left + 3.f * dpi_scale, _strip.top,
			             _strip.right, _strip.top + 3.f * dpi_scale),
			bar);
		if (_in_group) {
			DWRITE_TEXT_ALIGNMENT ta_g = fmt_meta->GetTextAlignment ();
			fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_TRAILING);
			rt->DrawTextW (L"G", 1, fmt_meta,
			               D2D1::RectF (_strip.right - 18.f * dpi_scale, _strip.top + 2.f * dpi_scale,
			                            _strip.right - 4.f * dpi_scale, _strip.top + 18.f * dpi_scale),
			               brand);
			fmt_meta->SetTextAlignment (ta_g);
		}
		if (owned) {
			owned->Release ();
		}
	}
	/* Selection wash (group select share lights peers). */
	if (_selected) {
		accent->SetOpacity (0.18f);
		rt->FillRectangle (_strip, accent);
		accent->SetOpacity (1.f);
	}

	DWRITE_TEXT_ALIGNMENT ta = fmt_title->GetTextAlignment ();
	fmt_title->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
	if (_has_comment && !_midi_strip) {
		/* Cm badge — Route::comment non-empty. */
		D2D1_RECT_F badge = D2D1::RectF (
			_label_rc.right - 20.f * dpi_scale, _label_rc.top + 1.f * dpi_scale,
			_label_rc.right - 2.f * dpi_scale, _label_rc.top + 15.f * dpi_scale);
		accent->SetOpacity (0.7f);
		rt->FillRectangle (badge, accent);
		accent->SetOpacity (1.f);
		DWRITE_TEXT_ALIGNMENT ta_b = fmt_meta->GetTextAlignment ();
		DWRITE_PARAGRAPH_ALIGNMENT pa_b = fmt_meta->GetParagraphAlignment ();
		fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
		fmt_meta->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		rt->DrawTextW (L"Cm", 2, fmt_meta, badge, brand);
		fmt_meta->SetTextAlignment (ta_b);
		fmt_meta->SetParagraphAlignment (pa_b);
	}
	if (_phase_inverted && !_midi_strip) {
		/* Ø badge — polarity inverted (PhaseControl). */
		D2D1_RECT_F badge = D2D1::RectF (
			_label_rc.left + 2.f * dpi_scale, _label_rc.top + 1.f * dpi_scale,
			_label_rc.left + 16.f * dpi_scale, _label_rc.top + 15.f * dpi_scale);
		accent->SetOpacity (0.75f);
		rt->FillRectangle (badge, accent);
		accent->SetOpacity (1.f);
		DWRITE_TEXT_ALIGNMENT ta_b = fmt_meta->GetTextAlignment ();
		DWRITE_PARAGRAPH_ALIGNMENT pa_b = fmt_meta->GetParagraphAlignment ();
		fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
		fmt_meta->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		rt->DrawTextW (L"\x00D8", 1, fmt_meta, badge, brand);
		fmt_meta->SetTextAlignment (ta_b);
		fmt_meta->SetParagraphAlignment (pa_b);
	}
	if (_route_active) {
		rt->DrawTextW (_label, (UINT32)wcslen (_label), fmt_title, _label_rc, brand);
	} else {
		wchar_t off_lab[40];
		_snwprintf_s (off_lab, _TRUNCATE, L"%s · Off", _label);
		rt->DrawTextW (off_lab, (UINT32)wcslen (off_lab), fmt_title, _label_rc, body);
	}
	fmt_title->SetTextAlignment (ta);

	DWRITE_TEXT_ALIGNMENT ta_m = fmt_meta->GetTextAlignment ();
	DWRITE_PARAGRAPH_ALIGNMENT pa_m = fmt_meta->GetParagraphAlignment ();
	fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
	fmt_meta->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

	rt->FillRectangle (_arm_btn, _armed ? accent : edge);
	if (!_midi_strip) {
		const wchar_t* arm_label =
			_record_safe ? L"R · Rs"
			: (_armed ? L"R · armed" : L"R · arm");
		rt->DrawTextW (arm_label, (UINT32)wcslen (arm_label), fmt_meta, _arm_btn, brand);

		/* Mon — Auto / In / Disk; ·N peer badge when group shares monitoring. */
		const bool mon_forced = _monitor_choice != 0 || _monitor_peer_count > 0;
		rt->FillRectangle (_mon_btn, mon_forced ? accent : edge);
		wchar_t mon_label[16];
		const wchar_t* base =
			(_monitor_choice == 1) ? L"In"
			: (_monitor_choice == 2) ? L"Disk" : L"Auto";
		if (_monitor_peer_count > 0) {
			_snwprintf_s (mon_label, _TRUNCATE, L"%s·%d", base, _monitor_peer_count + 1);
		} else {
			_snwprintf_s (mon_label, _TRUNCATE, L"%s", base);
		}
		rt->DrawTextW (mon_label, (UINT32)wcslen (mon_label), fmt_meta, _mon_btn, brand);
	}

	/* Mute — lit body wash when muted; All badge when mute-points=AllPoints. */
	{
		const wchar_t* mlab = (_mute_points != 0) ? L"M·All" : L"M";
		if (_muted) {
			body->SetOpacity (0.9f);
			rt->FillRectangle (_mute_btn, body);
			body->SetOpacity (1.f);
			rt->DrawTextW (mlab, (UINT32)wcslen (mlab), fmt_meta, _mute_btn, brand);
		} else {
			rt->FillRectangle (_mute_btn, edge);
			rt->DrawTextW (mlab, (UINT32)wcslen (mlab), fmt_meta, _mute_btn, brand);
		}
	}

	/* Solo — accent when self/effective solo; Si when solo-isolated, Ss when safe. */
	if (_soloed) {
		accent->SetOpacity (0.85f);
		rt->FillRectangle (_solo_btn, accent);
		accent->SetOpacity (1.f);
	} else if (_solo_isolated || _solo_safe) {
		body->SetOpacity (0.55f);
		rt->FillRectangle (_solo_btn, body);
		body->SetOpacity (1.f);
	} else {
		rt->FillRectangle (_solo_btn, edge);
	}
	{
		const wchar_t* s_lab = _solo_isolated ? L"Si" : (_solo_safe ? L"Ss" : L"S");
		const wchar_t* solo_glyph = s_lab;
		if (!_solo_isolated && !_solo_safe) {
			if (_listen_mode == 1) {
				solo_glyph = L"P";
			} else if (_listen_mode == 2) {
				solo_glyph = L"A";
			}
		}
		rt->DrawTextW (solo_glyph, (UINT32)wcslen (solo_glyph), fmt_meta, _solo_btn, brand);
	}

	if (!_midi_strip) {
		/* Trim track — −20 · 0 · +20 dB (knob at unity when centered). */
		rt->FillRectangle (_trim_track, edge);
		{
			float tw = _trim_track.right - _trim_track.left;
			float kn_cx = _trim_track.left + tw * _trim;
			float kn_w = 7.f * dpi_scale;
			D2D1_RECT_F kn = D2D1::RectF (
				kn_cx - kn_w * 0.5f, _trim_track.top,
				kn_cx + kn_w * 0.5f, _trim_track.bottom);
			rt->FillRectangle (kn, accent);
			float mid_x = (_trim_track.left + _trim_track.right) * 0.5f;
			rt->DrawLine (
				D2D1::Point2F (mid_x, _trim_track.top),
				D2D1::Point2F (mid_x, _trim_track.bottom),
				body, 1.f);
		}

		if (!_narrow) {
			/* Pan azimuth track — L · C · R; stereo width band when available. */
		rt->FillRectangle (_pan_track, edge);
		float pw = _pan_track.right - _pan_track.left;
		float kn_cx = _pan_track.left + pw * _pan;
		if (_pan_width_ok) {
			float half = std::fabs (_pan_width) * 0.5f * pw;
			float band_l = std::max (_pan_track.left, kn_cx - half);
			float band_r = std::min (_pan_track.right, kn_cx + half);
			accent->SetOpacity (0.35f);
			rt->FillRectangle (
				D2D1::RectF (band_l, _pan_track.top, band_r, _pan_track.bottom),
				accent);
			accent->SetOpacity (1.f);
		}
		float kn_w = 8.f * dpi_scale;
		D2D1_RECT_F kn = D2D1::RectF (
			kn_cx - kn_w * 0.5f, _pan_track.top,
			kn_cx + kn_w * 0.5f, _pan_track.bottom);
		rt->FillRectangle (kn, accent);
		/* Center tick */
		float mid_x = (_pan_track.left + _pan_track.right) * 0.5f;
		rt->DrawLine (
			D2D1::Point2F (mid_x, _pan_track.top),
			D2D1::Point2F (mid_x, _pan_track.bottom),
			body, 1.f);
		if (_pan_width_ok) {
			/* Thin W badge — Shift+drag pan adjusts stereo width. */
			DWRITE_TEXT_ALIGNMENT ta_w = fmt_meta->GetTextAlignment ();
			fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_TRAILING);
			rt->DrawTextW (
				L"W", 1, fmt_meta,
				D2D1::RectF (_pan_track.right - 14.f * dpi_scale,
				             _pan_track.top - 1.f * dpi_scale,
				             _pan_track.right + 2.f * dpi_scale,
				             _pan_track.bottom + 1.f * dpi_scale),
				body);
			fmt_meta->SetTextAlignment (ta_w);
		}

		/* Aux send level → Aux bus; pan knob when send panner present. */
		rt->FillRectangle (_send_track, edge);
		{
			float sw = _send_track.right - _send_track.left;
			float fill_r = _send_track.left + sw * _send;
			accent->SetOpacity (_send_muted ? 0.22f : 0.65f);
			rt->FillRectangle (
				D2D1::RectF (_send_track.left, _send_track.top, fill_r, _send_track.bottom),
				accent);
			accent->SetOpacity (1.f);
			if (_send_muted) {
				rt->DrawTextW (L"Sm", 2, fmt_meta, _send_track, body);
			}
			if (_send_pan_ok) {
				float kn_cx = _send_track.left + sw * _send_pan;
				float kn_w = 6.f * dpi_scale;
				rt->FillRectangle (
					D2D1::RectF (kn_cx - kn_w * 0.5f, _send_track.top,
					             kn_cx + kn_w * 0.5f, _send_track.bottom),
					body);
				DWRITE_TEXT_ALIGNMENT ta_p = fmt_meta->GetTextAlignment ();
				fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_TRAILING);
				rt->DrawTextW (
					L"P", 1, fmt_meta,
					D2D1::RectF (_send_track.right - 12.f * dpi_scale,
					             _send_track.top - 1.f * dpi_scale,
					             _send_track.right + 2.f * dpi_scale,
					             _send_track.bottom + 1.f * dpi_scale),
					body);
				fmt_meta->SetTextAlignment (ta_p);
			}
		}

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

		if (fx_on) {
			auto draw_param = [&] (const D2D1_RECT_F& track, float value, float opac) {
				rt->FillRectangle (track, edge);
				float pw = track.right - track.left;
				float fill_r = track.left + pw * value;
				ID2D1SolidColorBrush* fill = _fx_bypassed ? body : accent;
				fill->SetOpacity (_fx_bypassed ? 0.35f : opac);
				rt->FillRectangle (
					D2D1::RectF (track.left, track.top, fill_r, track.bottom), fill);
				fill->SetOpacity (1.f);
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

	fmt_meta->SetTextAlignment (ta_m);
	fmt_meta->SetParagraphAlignment (pa_m);

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
	rt->FillRectangle (knob, _muted ? body : accent);

	if (_gain > 0.001f) {
		ID2D1SolidColorBrush* fill = _muted ? body : accent;
		fill->SetOpacity (0.35f);
		rt->FillRectangle (D2D1::RectF (_track.left, knob_y, _track.right, _track.bottom), fill);
		fill->SetOpacity (1.f);
	}

	_meter.draw (rt, accent, edge, meter_hot, dpi_scale);
	if (!_midi_strip) {
		const wchar_t* mp = L"Pst";
		switch (_meter_point) {
		case 0: mp = L"In"; break;
		case 1: mp = L"Pre"; break;
		case 3: mp = L"Out"; break;
		default: mp = L"Pst"; break;
		}
		const D2D1_RECT_F& mb = _meter.bounds ();
		DWRITE_TEXT_ALIGNMENT ta_mp = fmt_meta->GetTextAlignment ();
		fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
		rt->DrawTextW (mp, (UINT32)wcslen (mp), fmt_meta,
		               D2D1::RectF (mb.left - 2.f * dpi_scale, mb.top - 14.f * dpi_scale,
		                            mb.right + 2.f * dpi_scale, mb.top - 1.f * dpi_scale),
		               body);
		fmt_meta->SetTextAlignment (ta_mp);
		const wchar_t* mt = (_meter_type == 1) ? L"K14" : L"Pk";
		const D2D1_RECT_F& mb2 = _meter.bounds ();
		DWRITE_TEXT_ALIGNMENT ta_mt = fmt_meta->GetTextAlignment ();
		fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
		rt->DrawTextW (mt, (UINT32)wcslen (mt), fmt_meta,
		               D2D1::RectF (mb2.left - 4.f * dpi_scale, mb2.bottom + 1.f * dpi_scale,
		                            mb2.right + 4.f * dpi_scale, mb2.bottom + 14.f * dpi_scale),
		               body);
		fmt_meta->SetTextAlignment (ta_mt);
	}

	wchar_t db[48];
	float db_v = (_gain <= 0.0001f) ? -96.f : (20.f * log10f (_gain));
	if (_narrow && !_midi_strip) {
		_snwprintf_s (db, _TRUNCATE, L"%.0f", db_v);
	} else if (!_midi_strip) {
		const wchar_t* pan_txt = L"C";
		if (_pan < 0.45f) {
			pan_txt = L"L";
		} else if (_pan > 0.55f) {
			pan_txt = L"R";
		}
		if (!_route_active) {
			_snwprintf_s (db, _TRUNCATE, L"%.1f dB  ·  %s  ·  OFF", db_v, pan_txt);
		} else if (_fx_label[0] != L'\0') {
			_snwprintf_s (db, _TRUNCATE, L"%.1f dB  ·  %s  ·  Snd %.0f%%",
			              db_v, pan_txt, _send * 100.f);
		} else if (_muted) {
			_snwprintf_s (db, _TRUNCATE, L"%.1f dB  ·  %s  ·  MUTE", db_v, pan_txt);
		} else if (_soloed) {
			_snwprintf_s (db, _TRUNCATE, L"%.1f dB  ·  %s  ·  SOLO", db_v, pan_txt);
		} else {
			_snwprintf_s (db, _TRUNCATE, L"%.1f dB  ·  %s  ·  Snd %.0f%%",
			              db_v, pan_txt, _send * 100.f);
		}
	} else if (_muted && _soloed) {
		_snwprintf_s (db, _TRUNCATE, L"%.1f dB  ·  M+S", db_v);
	} else if (_muted) {
		_snwprintf_s (db, _TRUNCATE, L"%.1f dB  ·  MUTE", db_v);
	} else if (_soloed) {
		_snwprintf_s (db, _TRUNCATE, L"%.1f dB  ·  SOLO", db_v);
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
