/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "wave_strip.h"

#include <algorithm>
#include <cmath>

WaveStrip::WaveStrip ()
	: _scroll (0.f)
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
WaveStrip::update (const WaveStripParams& params)
{
	const size_t n = _peaks.size ();
	if (n == 0) {
		return;
	}

	/* Scroll rate: when engine runs, advance by periods; else wall-clock. */
	float scroll_hz = 0.35f;
	if (params.engine_running && params.sample_rate > 0 && params.period_frames > 0) {
		float period_sec = (float)params.period_frames / (float)params.sample_rate;
		scroll_hz = std::max (0.2f, std::min (2.5f, 0.08f / std::max (0.001f, period_sec)));
	}
	_scroll = params.time_sec * scroll_hz;

	for (size_t i = 0; i < n; ++i) {
		float x = (float)i / (float)n;
		float phase = x * 6.2831853f * 3.f + _scroll * 6.2831853f;

		/* Layered tones + soft noise → peak envelope (demo audio shape). */
		float s1 = std::sin (phase);
		float s2 = 0.45f * std::sin (phase * 2.17f + 0.7f);
		float s3 = 0.25f * std::sin (phase * 5.3f + _scroll);
		float noise = 0.08f * std::sin (phase * 17.f + _scroll * 3.f);
		float sample = s1 + s2 + s3 + noise;
		float peak = std::min (1.f, std::fabs (sample) * 0.85f);

		/* Transient bumps every ~bar when "engine" feels live. */
		if (params.engine_running) {
			float beat = std::fmod (_scroll * 2.f + x * 4.f, 1.f);
			if (beat < 0.06f) {
				peak = std::min (1.f, peak + 0.55f * (1.f - beat / 0.06f));
			}
		}

		float rms = peak * (0.55f + 0.2f * std::sin (phase * 0.5f));
		rms = std::min (peak, std::max (0.02f, rms));

		/* Light smoothing so columns don't sparkle every frame. */
		_peaks[i] = _peaks[i] * 0.65f + peak * 0.35f;
		_rms[i] = _rms[i] * 0.7f + rms * 0.3f;
	}
}

void
WaveStrip::draw (ID2D1RenderTarget* rt,
                 const D2D1_RECT_F& bounds,
                 ID2D1SolidColorBrush* peak_brush,
                 ID2D1SolidColorBrush* rms_brush,
                 ID2D1SolidColorBrush* mid_brush,
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

	/* Center line. */
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

		/* RMS body (wider feel, lower opacity via separate brush color). */
		rt->FillRectangle (D2D1::RectF (x0, mid_y - rms_h, x1, mid_y + rms_h), rms_brush);

		/* Peak outline as thin bipolar bars. */
		rt->FillRectangle (D2D1::RectF (x0, mid_y - peak_h, x1, mid_y - peak_h + 2.f * dpi_scale), peak_brush);
		rt->FillRectangle (D2D1::RectF (x0, mid_y + peak_h - 2.f * dpi_scale, x1, mid_y + peak_h), peak_brush);
	}
}
