/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "cairo_wave_blit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(AIRDOUR_HAVE_CAIRO)

#include <cairo.h>

namespace {

/* Cairo ARGB32 little-endian == BGRA premultiplied — matches D2D default. */
void
paint_peaks_surface (cairo_surface_t* surface,
                     int width,
                     int height,
                     const float* peaks,
                     const float* rms,
                     size_t n,
                     float dpi_scale,
                     float peak_r, float peak_g, float peak_b,
                     float rms_r, float rms_g, float rms_b)
{
	cairo_t* cr = cairo_create (surface);
	cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba (cr, 0, 0, 0, 0);
	cairo_paint (cr);
	cairo_set_operator (cr, CAIRO_OPERATOR_OVER);

	if (n == 0 || width < 2 || height < 2) {
		cairo_destroy (cr);
		return;
	}

	const double mid_y = (double)height * 0.5;
	const double col_w = (double)width / (double)n;
	const double gap = std::max (0.5, (double)dpi_scale);
	const double peak_edge = std::max (1.0, 2.0 * (double)dpi_scale);

	for (size_t i = 0; i < n; ++i) {
		double x0 = (double)i * col_w + gap * 0.5;
		double x1 = (double)(i + 1) * col_w - gap * 0.5;
		if (x1 <= x0) {
			continue;
		}
		float p = peaks ? std::max (0.f, std::min (1.f, peaks[i])) : 0.f;
		float r = rms ? std::max (0.f, std::min (p, rms[i])) : p * 0.55f;
		double peak_h = (double)p * ((double)height * 0.46);
		double rms_h = (double)r * ((double)height * 0.46);

		cairo_set_source_rgba (cr, rms_r, rms_g, rms_b, 0.55);
		cairo_rectangle (cr, x0, mid_y - rms_h, x1 - x0, rms_h * 2.0);
		cairo_fill (cr);

		cairo_set_source_rgba (cr, peak_r, peak_g, peak_b, 0.95);
		cairo_rectangle (cr, x0, mid_y - peak_h, x1 - x0, peak_edge);
		cairo_fill (cr);
		cairo_rectangle (cr, x0, mid_y + peak_h - peak_edge, x1 - x0, peak_edge);
		cairo_fill (cr);
	}

	cairo_destroy (cr);
}

} /* anonymous */

bool
CairoWaveBlit::blit_peaks (ID2D1RenderTarget* rt,
                           const D2D1_RECT_F& dest,
                           const float* peaks,
                           const float* rms,
                           size_t n,
                           float dpi_scale,
                           float peak_r, float peak_g, float peak_b,
                           float rms_r, float rms_g, float rms_b)
{
	if (!rt || !peaks || n == 0) {
		return false;
	}
	float dw = dest.right - dest.left;
	float dh = dest.bottom - dest.top;
	if (dw < 8.f || dh < 8.f) {
		return false;
	}

	int width = (int)std::lround (dw);
	int height = (int)std::lround (dh);
	if (width < 8) {
		width = 8;
	}
	if (height < 8) {
		height = 8;
	}
	/* Cap cost — WaveStrip columns already ≤ 256. */
	if (width > 2048) {
		width = 2048;
	}
	if (height > 512) {
		height = 512;
	}

	cairo_surface_t* surface =
	    cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
	if (!surface || cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
		if (surface) {
			cairo_surface_destroy (surface);
		}
		return false;
	}

	paint_peaks_surface (surface, width, height, peaks, rms, n, dpi_scale,
	                     peak_r, peak_g, peak_b, rms_r, rms_g, rms_b);

	cairo_surface_flush (surface);
	unsigned char* data = cairo_image_surface_get_data (surface);
	int stride = cairo_image_surface_get_stride (surface);
	if (!data || stride <= 0) {
		cairo_surface_destroy (surface);
		return false;
	}

	D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties (
		D2D1::PixelFormat (DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	ID2D1Bitmap* bitmap = nullptr;
	HRESULT hr = rt->CreateBitmap (D2D1::SizeU ((UINT32)width, (UINT32)height),
	                               data, (UINT32)stride, props, &bitmap);
	cairo_surface_destroy (surface);
	if (FAILED (hr) || !bitmap) {
		return false;
	}

	rt->DrawBitmap (bitmap, dest, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
	bitmap->Release ();
	return true;
}

bool
CairoWaveBlit::smoke_offline ()
{
	const size_t n = 32;
	float peaks[32];
	float rms[32];
	for (size_t i = 0; i < n; ++i) {
		peaks[i] = 0.2f + 0.6f * ((float)(i % 8) / 7.f);
		rms[i] = peaks[i] * 0.55f;
	}

	const int width = 128;
	const int height = 48;
	cairo_surface_t* surface =
	    cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
	if (!surface || cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
		std::fprintf (stderr, "airdour_win_shell: smoke-cairo-wave FAILED (surface)\n");
		std::fflush (stderr);
		if (surface) {
			cairo_surface_destroy (surface);
		}
		return false;
	}

	paint_peaks_surface (surface, width, height, peaks, rms, n, 1.f,
	                     0.95f, 0.55f, 0.20f,
	                     0.35f, 0.55f, 0.75f);
	cairo_surface_flush (surface);

	unsigned char* data = cairo_image_surface_get_data (surface);
	int stride = cairo_image_surface_get_stride (surface);
	if (!data || stride <= 0) {
		std::fprintf (stderr, "airdour_win_shell: smoke-cairo-wave FAILED (data)\n");
		std::fflush (stderr);
		cairo_surface_destroy (surface);
		return false;
	}

	/* Count non-transparent pixels — canvas ImageSurface pattern inked. */
	uint32_t inked = 0;
	for (int y = 0; y < height; ++y) {
		const uint32_t* row = (const uint32_t*)(data + y * stride);
		for (int x = 0; x < width; ++x) {
			if ((row[x] >> 24) != 0) {
				++inked;
			}
		}
	}
	cairo_surface_destroy (surface);

	if (inked < 64) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-cairo-wave FAILED (inked=%u)\n",
		              inked);
		std::fflush (stderr);
		return false;
	}

	std::fprintf (stderr,
	              "airdour_win_shell: smoke-cairo-wave ok (cairo %s; inked=%u; no GTK chrome)\n",
	              cairo_version_string (), inked);
	std::fflush (stderr);
	return true;
}

#endif /* AIRDOUR_HAVE_CAIRO */
