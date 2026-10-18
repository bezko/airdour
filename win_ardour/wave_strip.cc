/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "wave_strip.h"

#include <algorithm>
#include <cmath>

WaveStrip::WaveStrip ()
	: _auto_scroll (0.f)
	, _pan (0.f)
	, _zoom (1.f)
	, _user_view (false)
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
	/* Keep the point under the cursor stable in content space. */
	float content_at_cursor = _pan + norm_x / old_zoom;
	_pan = content_at_cursor - norm_x / _zoom;
	_user_view = true;
}

void
WaveStrip::pan_by (float delta_widths)
{
	/* delta_widths is in screen widths; convert to content units. */
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
WaveStrip::update (const WaveStripParams& params)
{
	const size_t n = _peaks.size ();
	if (n == 0) {
		return;
	}

	float scroll_hz = 0.35f;
	if (params.engine_running && params.sample_rate > 0 && params.period_frames > 0) {
		float period_sec = (float)params.period_frames / (float)params.sample_rate;
		scroll_hz = std::max (0.2f, std::min (2.5f, 0.08f / std::max (0.001f, period_sec)));
	}
	_auto_scroll = params.time_sec * scroll_hz;

	/* Content origin: auto-scroll unless user has taken the view. */
	float origin = _user_view ? _pan : (_auto_scroll + _pan);
	/* Visible content span shrinks as zoom increases. */
	float span = 1.f / std::max (0.001f, _zoom);
	float amp = std::max (0.f, std::min (1.f, params.gain));

	for (size_t i = 0; i < n; ++i) {
		float x = (float)i / (float)n;
		float content_x = origin + x * span;
		float phase = content_x * 6.2831853f * 3.f;

		float s1 = std::sin (phase);
		float s2 = 0.45f * std::sin (phase * 2.17f + 0.7f);
		float s3 = 0.25f * std::sin (phase * 5.3f + content_x);
		float noise = 0.08f * std::sin (phase * 17.f + content_x * 3.f);
		float sample = s1 + s2 + s3 + noise;
		float peak = std::min (1.f, std::fabs (sample) * 0.85f * amp);

		if (params.engine_running || params.transport_playing) {
			float beat = std::fmod (content_x * 2.f + x * 4.f, 1.f);
			if (beat < 0.f) {
				beat += 1.f;
			}
			if (beat < 0.06f) {
				peak = std::min (1.f, peak + 0.55f * amp * (1.f - beat / 0.06f));
			}
		}

		float rms = peak * (0.55f + 0.2f * std::sin (phase * 0.5f));
		rms = std::min (peak, std::max (0.02f * amp, rms));

		_peaks[i] = _peaks[i] * 0.55f + peak * 0.45f;
		_rms[i] = _rms[i] * 0.6f + rms * 0.4f;
	}
}

void
WaveStrip::draw (ID2D1RenderTarget* rt,
                 const D2D1_RECT_F& bounds,
                 ID2D1SolidColorBrush* peak_brush,
                 ID2D1SolidColorBrush* rms_brush,
                 ID2D1SolidColorBrush* mid_brush,
                 ID2D1SolidColorBrush* playhead_brush,
                 float playhead_norm,
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

	mid_brush->SetOpacity (0.35f);
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

		float peak_h = _peaks[i] * (h * 0.48f);
		float rms_h = _rms[i] * (h * 0.48f);

		rt->FillRectangle (D2D1::RectF (x0, mid_y - rms_h, x1, mid_y + rms_h), rms_brush);
		rt->FillRectangle (D2D1::RectF (x0, mid_y - peak_h, x1, mid_y - peak_h + 2.f * dpi_scale), peak_brush);
		rt->FillRectangle (D2D1::RectF (x0, mid_y + peak_h - 2.f * dpi_scale, x1, mid_y + peak_h), peak_brush);
	}

	if (playhead_brush && playhead_norm >= 0.f) {
		float pn = std::max (0.f, std::min (1.f, playhead_norm));
		float px = bounds.left + pn * w;
		float thick = std::max (1.5f, 2.f * dpi_scale);
		rt->FillRectangle (D2D1::RectF (px - thick * 0.5f, bounds.top, px + thick * 0.5f, bounds.bottom),
		                   playhead_brush);
	}
}
