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
	float    time_sec;     /* UI clock */
	float    playhead_sec; /* transport time */
	float    gain;         /* 0..1 amplitude scale */
	float    track_seed;   /* per-lane timbre offset */
};

class WaveStrip
{
public:
	static constexpr size_t kMaxColumns = 256;
	static constexpr float  kZoomMin    = 0.25f;
	static constexpr float  kZoomMax    = 16.f;
	/** Screen fraction where playhead sits while following transport. */
	static constexpr float  kFollowFrac = 0.28f;

	WaveStrip ();

	void resize_for_width (float width_px);
	void update (const WaveStripParams& params);

	void draw (ID2D1RenderTarget* rt,
	           const D2D1_RECT_F& bounds,
	           ID2D1SolidColorBrush* peak_brush,
	           ID2D1SolidColorBrush* rms_brush,
	           ID2D1SolidColorBrush* mid_brush,
	           ID2D1SolidColorBrush* playhead_brush,
	           float dpi_scale) const;

	void zoom_at (float factor, float norm_x);
	void pan_by (float delta_widths);
	void reset_view ();

	/** Copy shared timeline view from another strip (linked zoom/pan). */
	void sync_view_from (const WaveStrip& other);

	float  zoom () const { return _zoom; }
	float  pan () const { return _pan; }
	bool   interacting () const { return _user_view; }
	/** Playhead X in [0,1] of strip, or <0 if off-screen. */
	float  playhead_norm () const { return _playhead_norm; }
	size_t column_count () const { return _peaks.size (); }

private:
	std::vector<float> _peaks;
	std::vector<float> _rms;
	float              _pan;
	float              _zoom;
	bool               _user_view;
	float              _playhead_norm;
	float              _content_playhead;
};
