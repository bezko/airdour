/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Thin Cairo→D2D bridge for Editor ink (waveforms, automation curves,
 * fade wedges, MIDI notes). Reuses the libs/canvas ImageSurface pattern
 * (offline ARGB32) without linking libs/canvas or opening GTK windows.
 * Product chrome stays D2D.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <cstdint>

#include <windows.h>
#include <d2d1.h>

#if defined(AIRDOUR_HAVE_CAIRO)

/** One MIDI note rect in normalized dest coords (0..1). */
struct CairoMidiNoteBlit
{
	float x0, y0, x1, y1;
	float r, g, b, a;
	bool  selected;
};

/**
 * Offline Cairo ImageSurface → ID2D1Bitmap blit helpers.
 * Labels / playhead / pane chrome stay native D2D (caller draws those).
 */
class CairoWaveBlit
{
public:
	/** True when built with AIRDOUR_HAVE_CAIRO (vcpkg cairo linked). */
	static bool available () { return true; }

	/**
	 * Render peaks/rms envelope into an ARGB32 Cairo surface and DrawBitmap.
	 * Returns false on Cairo/D2D failure (caller may fall back to FillRectangle).
	 */
	static bool blit_peaks (ID2D1RenderTarget* rt,
	                        const D2D1_RECT_F& dest,
	                        const float* peaks,
	                        const float* rms,
	                        size_t n,
	                        float dpi_scale,
	                        float peak_r, float peak_g, float peak_b,
	                        float rms_r, float rms_g, float rms_b);

	/**
	 * Antialiased automation polyline (y 0..1, bottom=0) + point dots.
	 * Midline / label / playhead stay D2D.
	 */
	static bool blit_automation_curve (ID2D1RenderTarget* rt,
	                                   const D2D1_RECT_F& dest,
	                                   const float* y_0_1,
	                                   size_t n,
	                                   const float* pt_x_norm,
	                                   const float* pt_y_0_1,
	                                   size_t pt_n,
	                                   float dpi_scale,
	                                   float line_r, float line_g, float line_b);

	/**
	 * Fade-in / fade-out wedges (normalized edge + handle positions 0..1).
	 * Selection fill stays D2D; wedges get Cairo AA fill.
	 */
	static bool blit_fade_wedges (ID2D1RenderTarget* rt,
	                              const D2D1_RECT_F& dest,
	                              float sel_n0, float sel_n1,
	                              float fade_in_n, float fade_out_n,
	                              float dpi_scale,
	                              float r, float g, float b, float a);

	/**
	 * MIDI note rectangles (normalized within dest) with optional select stroke.
	 */
	static bool blit_midi_notes (ID2D1RenderTarget* rt,
	                             const D2D1_RECT_F& dest,
	                             const CairoMidiNoteBlit* notes,
	                             size_t n,
	                             float dpi_scale);

	/**
	 * Spectrum / meter bars from levels 0..1 (left→right, bottom-aligned).
	 */
	static bool blit_bars (ID2D1RenderTarget* rt,
	                       const D2D1_RECT_F& dest,
	                       const float* levels_0_1,
	                       size_t n,
	                       float dpi_scale,
	                       float r, float g, float b, float a);

	/**
	 * Scatter points in normalized dest coords (0..1) — e.g. goniometer.
	 */
	static bool blit_points (ID2D1RenderTarget* rt,
	                         const D2D1_RECT_F& dest,
	                         const float* x_norm,
	                         const float* y_norm,
	                         size_t n,
	                         float dpi_scale,
	                         float r, float g, float b, float a);

	/**
	 * Headless smoke: peaks + curve + fades + notes ink; no HWND / GTK.
	 * For --smoke-cairo-wave.
	 */
	static bool smoke_offline ();

private:
	CairoWaveBlit () = delete;
};

#else /* !AIRDOUR_HAVE_CAIRO */

struct CairoMidiNoteBlit
{
	float x0, y0, x1, y1;
	float r, g, b, a;
	bool  selected;
};

class CairoWaveBlit
{
public:
	static bool available () { return false; }
	static bool blit_peaks (ID2D1RenderTarget*, const D2D1_RECT_F&,
	                        const float*, const float*, size_t, float,
	                        float, float, float, float, float, float)
	{
		return false;
	}
	static bool blit_automation_curve (ID2D1RenderTarget*, const D2D1_RECT_F&,
	                                   const float*, size_t,
	                                   const float*, const float*, size_t,
	                                   float, float, float, float)
	{
		return false;
	}
	static bool blit_fade_wedges (ID2D1RenderTarget*, const D2D1_RECT_F&,
	                              float, float, float, float, float,
	                              float, float, float, float)
	{
		return false;
	}
	static bool blit_midi_notes (ID2D1RenderTarget*, const D2D1_RECT_F&,
	                             const CairoMidiNoteBlit*, size_t, float)
	{
		return false;
	}
	static bool blit_bars (ID2D1RenderTarget*, const D2D1_RECT_F&,
	                       const float*, size_t, float,
	                       float, float, float, float)
	{
		return false;
	}
	static bool blit_points (ID2D1RenderTarget*, const D2D1_RECT_F&,
	                         const float*, const float*, size_t, float,
	                         float, float, float, float)
	{
		return false;
	}
	static bool smoke_offline () { return false; }
};

#endif
