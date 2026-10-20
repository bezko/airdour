/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Thin Cairo→D2D bridge for editor waveforms.
 * Reuses the libs/canvas ImageSurface pattern (offline ARGB32) without
 * linking libs/canvas or opening GTK windows. Product chrome stays D2D.
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

/**
 * Paint peak/RMS columns into a Cairo image surface, then blit via ID2D1Bitmap.
 * Midline / selection / playhead stay native D2D (caller draws those).
 */
class CairoWaveBlit
{
public:
	/** True when built with AIRDOUR_HAVE_CAIRO (vcpkg cairo linked). */
	static bool available () { return true; }

	/**
	 * Render peaks/rms into an ARGB32 Cairo surface and DrawBitmap into dest.
	 * peak_rgb / rms_rgb are 0..1 linear RGB (alpha applied inside).
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
	 * Headless smoke: Cairo image surface + non-zero ink; no HWND / GTK.
	 * For --smoke-cairo-wave.
	 */
	static bool smoke_offline ();

private:
	CairoWaveBlit () = delete;
};

#else /* !AIRDOUR_HAVE_CAIRO */

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
	static bool smoke_offline () { return false; }
};

#endif
