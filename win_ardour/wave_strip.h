/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Wave strip — Direct2D peak/RMS + canvas zoom/pan (not Cairo WaveView).
 * Synthetic demo peaks until session audio buffers are wired.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <vector>

#include <windows.h>
#include <d2d1.h>

struct WaveStripParams {
	bool     engine_running;
	bool     transport_playing;
	uint32_t sample_rate;
	uint32_t period_frames;
	float    time_sec; /* UI clock */
	float    gain;     /* 0..1 amplitude scale */
	float    playhead; /* 0..1 across visible strip; <0 hide */
};

class WaveStrip
{
public:
	static constexpr size_t kMaxColumns = 256;
	static constexpr float  kZoomMin    = 0.25f;
	static constexpr float  kZoomMax    = 16.f;

	WaveStrip ();

	void resize_for_width (float width_px);
	void update (const WaveStripParams& params);

	void draw (ID2D1RenderTarget* rt,
	           const D2D1_RECT_F& bounds,
	           ID2D1SolidColorBrush* peak_brush,
	           ID2D1SolidColorBrush* rms_brush,
	           ID2D1SolidColorBrush* mid_brush,
	           ID2D1SolidColorBrush* playhead_brush,
	           float playhead_norm,
	           float dpi_scale) const;

	/** Zoom toward |norm_x| in [0,1] across the strip (wheel). */
	void zoom_at (float factor, float norm_x);
	/** Pan by delta in strip widths (negative = earlier). */
	void pan_by (float delta_widths);
	void reset_view ();

	float  zoom () const { return _zoom; }
	float  pan () const { return _pan; }
	bool   interacting () const { return _user_view; }
	size_t column_count () const { return _peaks.size (); }

private:
	std::vector<float> _peaks;
	std::vector<float> _rms;
	float              _auto_scroll; /* clock-driven */
	float              _pan;         /* user offset in "widths" of content */
	float              _zoom;        /* 1 = default; >1 zoomed in */
	bool               _user_view;   /* true after wheel/drag until reset */
};
