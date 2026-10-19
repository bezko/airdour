/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Wave strip — Direct2D peak/RMS + canvas zoom/pan (not Cairo WaveView).
 * Peaks come from Session playlist readouts (or stay empty). No synthetic fill.
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
};

class WaveStrip
{
public:
	static constexpr size_t kMaxColumns = 256;
	static constexpr float  kZoomMin    = 0.05f;  /* ~160s window at default span */
	static constexpr float  kZoomMax    = 32.f;   /* ~0.25s window */
	/** Default visible window at zoom=1 (seconds). */
	static constexpr float  kDefaultSpanSec = 8.f;
	/** Screen fraction where playhead sits while following transport. */
	static constexpr float  kFollowFrac = 0.28f;

	WaveStrip ();

	void resize_for_width (float width_px);

	/** Update view window + playhead from transport; peaks stay until set_waveform. */
	void update_view (const WaveStripParams& params);

	/** Copy Session peak/RMS columns, or clear to empty lane if peaks is null. */
	void set_waveform (const float* peaks, const float* rms, size_t n, bool has_regions);

	void draw (ID2D1RenderTarget* rt,
	           const D2D1_RECT_F& bounds,
	           ID2D1SolidColorBrush* peak_brush,
	           ID2D1SolidColorBrush* rms_brush,
	           ID2D1SolidColorBrush* mid_brush,
	           ID2D1SolidColorBrush* playhead_brush,
	           ID2D1SolidColorBrush* selection_brush,
	           float dpi_scale) const;

	void zoom_at (float factor, float norm_x);
	void pan_by (float delta_widths);
	void reset_view ();
	/**
	 * Fit absolute time range into the strip width (small pad).
	 * Sets user_view so transport follow does not steal the window.
	 */
	void fit_range (float start_sec, float end_sec, float pad_frac = 0.06f);
	/** Absolute view window (seconds); clamps zoom to kZoomMin..kZoomMax. */
	void set_view_window (float start_sec, float span_sec);

	/** Copy shared timeline view from another strip (linked zoom/pan). */
	void sync_view_from (const WaveStrip& other);

	/** Absolute-time selection highlight (seconds); inactive clears. */
	void set_selection (bool active, float start_sec, float end_sec,
	                    float fade_in_sec = 0.f, float fade_out_sec = 0.f);

	/** Hit-test fade handles on selection; returns -1 none, 0=in, 1=out. */
	int hit_fade_handle (const D2D1_RECT_F& bounds, float x, float y, float dpi_scale) const;

	/** Hit-test outer trim edges; returns -1 none, 0=front, 1=end. */
	int hit_trim_edge (const D2D1_RECT_F& bounds, float x, float y, float dpi_scale) const;

	/** True if (x,y) is inside the selection body (not outer trim pads). */
	bool hit_selection_body (const D2D1_RECT_F& bounds, float x, float y, float dpi_scale) const;

	/** Absolute-time loop range overlay; inactive clears. */
	void set_loop_range (bool active, float start_sec, float end_sec);

	/** Map client X within strip bounds to absolute time (seconds). */
	float time_at_x (const D2D1_RECT_F& bounds, float x) const;

	/** Map absolute time to X within strip bounds. */
	float x_at_time (const D2D1_RECT_F& bounds, float time_sec) const;

	float  zoom () const { return _zoom; }
	float  pan () const { return _pan; }
	float  view_start_sec () const { return _view_start_sec; }
	float  view_span_sec () const { return _view_span_sec; }
	bool   interacting () const { return _user_view; }
	bool   has_regions () const { return _has_regions; }
	/** Playhead X in [0,1] of strip, or <0 if off-screen. */
	float  playhead_norm () const { return _playhead_norm; }
	size_t column_count () const { return _peaks.size (); }

private:
	std::vector<float> _peaks;
	std::vector<float> _rms;
	float              _pan; /* view start seconds when user-panned */
	float              _zoom;
	bool               _user_view;
	bool               _has_regions;
	float              _playhead_norm;
	float              _view_start_sec;
	float              _view_span_sec;
	bool               _sel_active;
	float              _sel_start_sec;
	float              _sel_end_sec;
	float              _sel_fade_in_sec;
	float              _sel_fade_out_sec;
	bool               _loop_active;
	float              _loop_start_sec;
	float              _loop_end_sec;
};
