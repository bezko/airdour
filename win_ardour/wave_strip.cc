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
	, _playhead_norm (kFollowFrac)
	, _content_playhead (0.f)
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
	float content_at_cursor = _pan + norm_x / old_zoom;
	_pan = content_at_cursor - norm_x / _zoom;
	_user_view = true;
}

void
WaveStrip::pan_by (float delta_widths)
{
	_pan += delta_widths / std::max (0.001f, _zoom);
	_user_view = true;
}

void
WaveStrip::reset_view ()
{
	_zoom = 1.f;
	_pan = 0.f;
	_user_view = false;
}

void
WaveStrip::sync_view_from (const WaveStrip& other)
{
	_zoom = other._zoom;
	_pan = other._pan;
	_user_view = other._user_view;
}

void
WaveStrip::update (const WaveStripParams& params)
{
	const size_t n = _peaks.size ();
	if (n == 0) {
		return;
	}

	float scroll_hz = 0.45f;
	if (params.engine_running && params.sample_rate > 0 && params.period_frames > 0) {
		float period_sec = (float)params.period_frames / (float)params.sample_rate;
		scroll_hz = std::max (0.25f, std::min (2.5f, 0.1f / std::max (0.001f, period_sec)));
	}

	_content_playhead = params.playhead_sec * scroll_hz;
	float span = 1.f / std::max (0.001f, _zoom);

	float origin;
	if (_user_view) {
		origin = _pan;
	} else {
		/* Keep playhead parked at kFollowFrac while transport runs (or frozen). */
		origin = _content_playhead - kFollowFrac * span;
		_pan = origin;
	}

	_playhead_norm = (_content_playhead - origin) / span;
	if (_playhead_norm < -0.05f || _playhead_norm > 1.05f) {
		_playhead_norm = -1.f; /* off-screen */
	} else {
		_playhead_norm = std::max (0.f, std::min (1.f, _playhead_norm));
	}

	float amp = std::max (0.f, std::min (1.f, params.gain));
	float seed = params.track_seed;
	float seed_phase = seed * 1.7f + 0.3f;
	float harm = 2.3f + seed * 0.9f;

	for (size_t i = 0; i < n; ++i) {
		float x = (float)i / (float)n;
		float content_x = origin + x * span;
		float phase = content_x * 6.2831853f * (2.4f + seed * 0.35f) + seed_phase;

		/* Multi-harmonic synthetic — denser near the playhead content. */
		float dist = content_x - _content_playhead;
		float near_ph = std::exp (-dist * dist * 14.f);

		float s1 = std::sin (phase);
		float s2 = 0.42f * std::sin (phase * harm + 0.4f);
		float s3 = 0.22f * std::sin (phase * (harm + 2.1f) + content_x);
		float noise = 0.07f * std::sin (phase * 19.f + seed * 5.f);
		float sample = (s1 + s2 + s3 + noise) * (0.55f + 0.45f * near_ph);
		float peak = std::min (1.f, std::fabs (sample) * 0.9f * amp);

		if (params.transport_playing || params.engine_running) {
			/* Transient blips synced to playhead time (not screen x). */
			float beat = std::fmod (_content_playhead * 2.f + seed * 0.37f, 1.f);
			if (beat < 0.f) {
				beat += 1.f;
			}
			if (beat < 0.05f && near_ph > 0.2f) {
				peak = std::min (1.f, peak + 0.65f * amp * (1.f - beat / 0.05f));
			}
		} else {
			peak *= 0.35f; /* quieter when stopped */
		}

		float rms = peak * (0.52f + 0.22f * std::sin (phase * 0.45f + seed));
		rms = std::min (peak, std::max (0.015f * amp, rms));

		_peaks[i] = _peaks[i] * 0.5f + peak * 0.5f;
		_rms[i] = _rms[i] * 0.55f + rms * 0.45f;
	}
}

void
WaveStrip::draw (ID2D1RenderTarget* rt,
                 const D2D1_RECT_F& bounds,
                 ID2D1SolidColorBrush* peak_brush,
                 ID2D1SolidColorBrush* rms_brush,
                 ID2D1SolidColorBrush* mid_brush,
                 ID2D1SolidColorBrush* playhead_brush,
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

	mid_brush->SetOpacity (0.3f);
	rt->DrawLine (D2D1::Point2F (bounds.left, mid_y),
	              D2D1::Point2F (bounds.right, mid_y),
	              mid_brush, 1.f * dpi_scale);
	mid_brush->SetOpacity (1.f);

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
