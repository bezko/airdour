/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "wave_strip.h"

#include <algorithm>
#include <cmath>

WaveStrip::WaveStrip ()
	: _pan (0.f)
	, _zoom (1.f)
	, _user_view (false)
	, _has_regions (false)
	, _playhead_norm (kFollowFrac)
	, _view_start_sec (0.f)
	, _view_span_sec (kDefaultSpanSec)
	, _sel_active (false)
	, _sel_start_sec (0.f)
	, _sel_end_sec (0.f)
	, _loop_active (false)
	, _loop_start_sec (0.f)
	, _loop_end_sec (0.f)
{
	_peaks.assign (128, 0.f);
	_rms.assign (128, 0.f);
}

void
WaveStrip::resize_for_width (float width_px)
{
	size_t n = (size_t)std::max (32.f, std::min ((float)kMaxColumns, width_px / 3.f));
	if (n == _peaks.size ()) {
		return;
	}
	_peaks.assign (n, 0.f);
	_rms.assign (n, 0.f);
}

void
WaveStrip::zoom_at (float factor, float norm_x)
{
	norm_x = std::max (0.f, std::min (1.f, norm_x));
	float old_zoom = _zoom;
	_zoom = std::max (kZoomMin, std::min (kZoomMax, _zoom * factor));
	if (_zoom == old_zoom) {
		return;
	}
	float old_span = kDefaultSpanSec / old_zoom;
	float content_at_cursor = _view_start_sec + norm_x * old_span;
	float new_span = kDefaultSpanSec / _zoom;
	_pan = content_at_cursor - norm_x * new_span;
	_view_start_sec = _pan;
	_view_span_sec = new_span;
	_user_view = true;
}

void
WaveStrip::pan_by (float delta_widths)
{
	float span = kDefaultSpanSec / std::max (0.001f, _zoom);
	_pan += delta_widths * span;
	_view_start_sec = _pan;
	_view_span_sec = span;
	_user_view = true;
}

void
WaveStrip::reset_view ()
{
	_zoom = 1.f;
	_pan = 0.f;
	_user_view = false;
	_view_span_sec = kDefaultSpanSec;
}

void
WaveStrip::sync_view_from (const WaveStrip& other)
{
	_zoom = other._zoom;
	_pan = other._pan;
	_user_view = other._user_view;
	_view_start_sec = other._view_start_sec;
	_view_span_sec = other._view_span_sec;
}

void
WaveStrip::set_selection (bool active, float start_sec, float end_sec)
{
	_sel_active = active;
	if (end_sec < start_sec) {
		float t = start_sec;
		start_sec = end_sec;
		end_sec = t;
	}
	_sel_start_sec = start_sec;
	_sel_end_sec = end_sec;
}

void
WaveStrip::set_loop_range (bool active, float start_sec, float end_sec)
{
	_loop_active = active;
	if (end_sec < start_sec) {
		float t = start_sec;
		start_sec = end_sec;
		end_sec = t;
	}
	_loop_start_sec = start_sec;
	_loop_end_sec = end_sec;
}

float
WaveStrip::time_at_x (const D2D1_RECT_F& bounds, float x) const
{
	float w = bounds.right - bounds.left;
	float norm = (w > 1.f) ? (x - bounds.left) / w : 0.f;
	norm = std::max (0.f, std::min (1.f, norm));
	return _view_start_sec + norm * std::max (0.001f, _view_span_sec);
}

float
WaveStrip::x_at_time (const D2D1_RECT_F& bounds, float time_sec) const
{
	float w = bounds.right - bounds.left;
	float span = std::max (0.001f, _view_span_sec);
	float norm = (time_sec - _view_start_sec) / span;
	return bounds.left + norm * w;
}

void
WaveStrip::update_view (const WaveStripParams& params)
{
	_view_span_sec = kDefaultSpanSec / std::max (0.001f, _zoom);

	if (_user_view) {
		_view_start_sec = _pan;
	} else {
		_view_start_sec = params.playhead_sec - kFollowFrac * _view_span_sec;
		_pan = _view_start_sec;
	}

	_playhead_norm = (params.playhead_sec - _view_start_sec) / std::max (0.001f, _view_span_sec);
	if (_playhead_norm < -0.05f || _playhead_norm > 1.05f) {
		_playhead_norm = -1.f;
	} else {
		_playhead_norm = std::max (0.f, std::min (1.f, _playhead_norm));
	}
}

void
WaveStrip::set_waveform (const float* peaks, const float* rms, size_t n, bool has_regions)
{
	_has_regions = has_regions;
	if (_peaks.empty ()) {
		return;
	}
	if (!peaks || n == 0) {
		std::fill (_peaks.begin (), _peaks.end (), 0.f);
		std::fill (_rms.begin (), _rms.end (), 0.f);
		return;
	}

	const size_t count = std::min (n, _peaks.size ());
	float amp = 1.f;
	for (size_t i = 0; i < count; ++i) {
		float p = std::max (0.f, std::min (1.f, peaks[i] * amp));
		float r = rms ? std::max (0.f, std::min (p, rms[i] * amp)) : p * 0.55f;
		_peaks[i] = p;
		_rms[i] = r;
	}
	for (size_t i = count; i < _peaks.size (); ++i) {
		_peaks[i] = 0.f;
		_rms[i] = 0.f;
	}
}

void
WaveStrip::draw (ID2D1RenderTarget* rt,
                 const D2D1_RECT_F& bounds,
                 ID2D1SolidColorBrush* peak_brush,
                 ID2D1SolidColorBrush* rms_brush,
                 ID2D1SolidColorBrush* mid_brush,
                 ID2D1SolidColorBrush* playhead_brush,
                 ID2D1SolidColorBrush* selection_brush,
                 float dpi_scale) const
{
	if (!rt || _peaks.empty ()) {
		return;
	}

	const float w = bounds.right - bounds.left;
	const float h = bounds.bottom - bounds.top;
	if (w < 8.f || h < 8.f) {
		return;
	}

	const size_t n = _peaks.size ();
	const float mid_y = bounds.top + h * 0.5f;
	const float col_w = w / (float)n;
	const float gap = std::max (0.5f, 1.f * dpi_scale);
	const float span = std::max (0.001f, _view_span_sec);

	mid_brush->SetOpacity (0.3f);
	rt->DrawLine (D2D1::Point2F (bounds.left, mid_y),
	              D2D1::Point2F (bounds.right, mid_y),
	              mid_brush, 1.f * dpi_scale);
	mid_brush->SetOpacity (1.f);

	if (_loop_active && mid_brush && _loop_end_sec > _loop_start_sec) {
		float n0 = (_loop_start_sec - _view_start_sec) / span;
		float n1 = (_loop_end_sec - _view_start_sec) / span;
		n0 = std::max (0.f, std::min (1.f, n0));
		n1 = std::max (0.f, std::min (1.f, n1));
		if (n1 > n0) {
			mid_brush->SetOpacity (0.18f);
			rt->FillRectangle (
				D2D1::RectF (bounds.left + n0 * w, bounds.top,
				             bounds.left + n1 * w, bounds.bottom),
				mid_brush);
			mid_brush->SetOpacity (0.7f);
			float edge = std::max (1.f, 2.f * dpi_scale);
			rt->FillRectangle (
				D2D1::RectF (bounds.left + n0 * w, bounds.top,
				             bounds.left + n0 * w + edge, bounds.bottom),
				mid_brush);
			rt->FillRectangle (
				D2D1::RectF (bounds.left + n1 * w - edge, bounds.top,
				             bounds.left + n1 * w, bounds.bottom),
				mid_brush);
			mid_brush->SetOpacity (1.f);
		}
	}

	if (_sel_active && selection_brush && _sel_end_sec > _sel_start_sec) {
		float n0 = (_sel_start_sec - _view_start_sec) / span;
		float n1 = (_sel_end_sec - _view_start_sec) / span;
		n0 = std::max (0.f, std::min (1.f, n0));
		n1 = std::max (0.f, std::min (1.f, n1));
		if (n1 > n0) {
			selection_brush->SetOpacity (0.28f);
			rt->FillRectangle (
				D2D1::RectF (bounds.left + n0 * w, bounds.top,
				             bounds.left + n1 * w, bounds.bottom),
				selection_brush);
			selection_brush->SetOpacity (0.85f);
			float edge = std::max (1.f, 1.5f * dpi_scale);
			rt->FillRectangle (
				D2D1::RectF (bounds.left + n0 * w, bounds.top,
				             bounds.left + n0 * w + edge, bounds.bottom),
				selection_brush);
			rt->FillRectangle (
				D2D1::RectF (bounds.left + n1 * w - edge, bounds.top,
				             bounds.left + n1 * w, bounds.bottom),
				selection_brush);
			selection_brush->SetOpacity (1.f);
		}
	}

	if (!_has_regions) {
		/* Empty lane: midline + playhead only. */
		if (playhead_brush && _playhead_norm >= 0.f) {
			float px = bounds.left + _playhead_norm * w;
			float thick = std::max (1.5f, 2.f * dpi_scale);
			rt->FillRectangle (D2D1::RectF (px - thick * 0.5f, bounds.top, px + thick * 0.5f, bounds.bottom),
			                   playhead_brush);
		}
		return;
	}

	for (size_t i = 0; i < n; ++i) {
		float x0 = bounds.left + (float)i * col_w + gap * 0.5f;
		float x1 = bounds.left + (float)(i + 1) * col_w - gap * 0.5f;
		if (x1 <= x0) {
			continue;
		}

		float peak_h = _peaks[i] * (h * 0.46f);
		float rms_h = _rms[i] * (h * 0.46f);

		rt->FillRectangle (D2D1::RectF (x0, mid_y - rms_h, x1, mid_y + rms_h), rms_brush);
		rt->FillRectangle (D2D1::RectF (x0, mid_y - peak_h, x1, mid_y - peak_h + 2.f * dpi_scale), peak_brush);
		rt->FillRectangle (D2D1::RectF (x0, mid_y + peak_h - 2.f * dpi_scale, x1, mid_y + peak_h), peak_brush);
	}

	if (playhead_brush && _playhead_norm >= 0.f) {
		float px = bounds.left + _playhead_norm * w;
		float thick = std::max (1.5f, 2.f * dpi_scale);
		rt->FillRectangle (D2D1::RectF (px - thick * 0.5f, bounds.top, px + thick * 0.5f, bounds.bottom),
		                   playhead_brush);
	}
}
