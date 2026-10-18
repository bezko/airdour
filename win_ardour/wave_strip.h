/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Wave strip v0 — peak/RMS columns drawn with Direct2D (not Cairo WaveView).
 * Uses synthetic demo peaks until session audio buffers are wired.
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
	uint32_t sample_rate;
	uint32_t period_frames;
	float    time_sec; /* UI clock */
};

class WaveStrip
{
public:
	static constexpr size_t kMaxColumns = 256;

	WaveStrip ();

	/** Rebuild column count for the drawable width (DIP pixels). */
	void resize_for_width (float width_px);

	/** UI-thread: refresh synthetic peak/RMS (demo until session audio exists). */
	void update (const WaveStripParams& params);

	/** Draw into |bounds| using existing RT brushes (peak + rms + mid). */
	void draw (ID2D1RenderTarget* rt,
	           const D2D1_RECT_F& bounds,
	           ID2D1SolidColorBrush* peak_brush,
	           ID2D1SolidColorBrush* rms_brush,
	           ID2D1SolidColorBrush* mid_brush,
	           float dpi_scale) const;

	size_t column_count () const { return _peaks.size (); }
	bool   demo_mode () const { return true; }

private:
	std::vector<float> _peaks; /* 0..1 */
	std::vector<float> _rms;   /* 0..1 */
	float              _scroll;
};
