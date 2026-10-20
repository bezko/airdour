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

void
surface_clear (cairo_t* cr)
{
	cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba (cr, 0, 0, 0, 0);
	cairo_paint (cr);
	cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
}

bool
dims_ok (float dw, float dh, int* width, int* height, int max_w, int max_h)
{
	if (dw < 8.f || dh < 8.f) {
		return false;
	}
	int w = (int)std::lround (dw);
	int h = (int)std::lround (dh);
	if (w < 8) {
		w = 8;
	}
	if (h < 8) {
		h = 8;
	}
	if (w > max_w) {
		w = max_w;
	}
	if (h > max_h) {
		h = max_h;
	}
	*width = w;
	*height = h;
	return true;
}

bool
blit_surface_to_d2d (ID2D1RenderTarget* rt, const D2D1_RECT_F& dest, cairo_surface_t* surface)
{
	cairo_surface_flush (surface);
	unsigned char* data = cairo_image_surface_get_data (surface);
	int stride = cairo_image_surface_get_stride (surface);
	int width = cairo_image_surface_get_width (surface);
	int height = cairo_image_surface_get_height (surface);
	if (!data || stride <= 0 || width < 1 || height < 1) {
		return false;
	}
	D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties (
		D2D1::PixelFormat (DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
	ID2D1Bitmap* bitmap = nullptr;
	HRESULT hr = rt->CreateBitmap (D2D1::SizeU ((UINT32)width, (UINT32)height),
	                               data, (UINT32)stride, props, &bitmap);
	if (FAILED (hr) || !bitmap) {
		return false;
	}
	rt->DrawBitmap (bitmap, dest, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
	bitmap->Release ();
	return true;
}

uint32_t
count_inked (cairo_surface_t* surface)
{
	unsigned char* data = cairo_image_surface_get_data (surface);
	int stride = cairo_image_surface_get_stride (surface);
	int width = cairo_image_surface_get_width (surface);
	int height = cairo_image_surface_get_height (surface);
	if (!data || stride <= 0) {
		return 0;
	}
	uint32_t inked = 0;
	for (int y = 0; y < height; ++y) {
		const uint32_t* row = (const uint32_t*)(data + y * stride);
		for (int x = 0; x < width; ++x) {
			if ((row[x] >> 24) != 0) {
				++inked;
			}
		}
	}
	return inked;
}

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
	surface_clear (cr);

	if (n == 0 || width < 2 || height < 2) {
		cairo_destroy (cr);
		return;
	}

	const double mid_y = (double)height * 0.5;
	const double col_w = (double)width / (double)n;
	const double amp = (double)height * 0.46;
	const double stroke = std::max (1.0, 1.5 * (double)dpi_scale);

	cairo_new_path (cr);
	for (size_t i = 0; i < n; ++i) {
		float p = peaks ? std::max (0.f, std::min (1.f, peaks[i])) : 0.f;
		float r = rms ? std::max (0.f, std::min (p, rms[i])) : p * 0.55f;
		double x = ((double)i + 0.5) * col_w;
		double y = mid_y - (double)r * amp;
		if (i == 0) {
			cairo_move_to (cr, x, y);
		} else {
			cairo_line_to (cr, x, y);
		}
	}
	for (size_t i = n; i-- > 0;) {
		float p = peaks ? std::max (0.f, std::min (1.f, peaks[i])) : 0.f;
		float r = rms ? std::max (0.f, std::min (p, rms[i])) : p * 0.55f;
		double x = ((double)i + 0.5) * col_w;
		cairo_line_to (cr, x, mid_y + (double)r * amp);
	}
	cairo_close_path (cr);
	cairo_set_source_rgba (cr, rms_r, rms_g, rms_b, 0.50);
	cairo_fill (cr);

	cairo_set_line_width (cr, stroke);
	cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
	cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_source_rgba (cr, peak_r, peak_g, peak_b, 0.95);
	cairo_new_path (cr);
	for (size_t i = 0; i < n; ++i) {
		float p = peaks ? std::max (0.f, std::min (1.f, peaks[i])) : 0.f;
		double x = ((double)i + 0.5) * col_w;
		double y = mid_y - (double)p * amp;
		if (i == 0) {
			cairo_move_to (cr, x, y);
		} else {
			cairo_line_to (cr, x, y);
		}
	}
	cairo_stroke (cr);
	cairo_new_path (cr);
	for (size_t i = 0; i < n; ++i) {
		float p = peaks ? std::max (0.f, std::min (1.f, peaks[i])) : 0.f;
		double x = ((double)i + 0.5) * col_w;
		double y = mid_y + (double)p * amp;
		if (i == 0) {
			cairo_move_to (cr, x, y);
		} else {
			cairo_line_to (cr, x, y);
		}
	}
	cairo_stroke (cr);

	cairo_destroy (cr);
}

void
paint_curve_surface (cairo_surface_t* surface,
                     int width,
                     int height,
                     const float* y_0_1,
                     size_t n,
                     const float* pt_x,
                     const float* pt_y,
                     size_t pt_n,
                     float dpi_scale,
                     float line_r, float line_g, float line_b)
{
	cairo_t* cr = cairo_create (surface);
	surface_clear (cr);
	if (width < 2 || height < 2) {
		cairo_destroy (cr);
		return;
	}

	const double stroke = std::max (1.5, 1.8 * (double)dpi_scale);
	const double radius = std::max (2.5, 3.0 * (double)dpi_scale);

	if (y_0_1 && n >= 2) {
		const double col_w = (double)width / (double)n;
		cairo_set_line_width (cr, stroke);
		cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
		cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
		cairo_set_source_rgba (cr, line_r, line_g, line_b, 0.95);
		cairo_new_path (cr);
		for (size_t i = 0; i < n; ++i) {
			float yv = std::max (0.f, std::min (1.f, y_0_1[i]));
			double x = ((double)i + 0.5) * col_w;
			double y = (double)height - (double)yv * (double)height;
			if (i == 0) {
				cairo_move_to (cr, x, y);
			} else {
				cairo_line_to (cr, x, y);
			}
		}
		cairo_stroke (cr);
	}

	if (pt_x && pt_y && pt_n > 0) {
		cairo_set_source_rgba (cr, line_r, line_g, line_b, 0.95);
		for (size_t i = 0; i < pt_n; ++i) {
			float xn = std::max (0.f, std::min (1.f, pt_x[i]));
			float yn = std::max (0.f, std::min (1.f, pt_y[i]));
			double x = (double)xn * (double)width;
			double y = (double)height - (double)yn * (double)height;
			cairo_arc (cr, x, y, radius, 0, 2.0 * 3.14159265358979323846);
			cairo_fill (cr);
		}
	}

	cairo_destroy (cr);
}

void
paint_fade_surface (cairo_surface_t* surface,
                    int width,
                    int height,
                    float sel_n0, float sel_n1,
                    float fade_in_n, float fade_out_n,
                    float r, float g, float b, float a)
{
	cairo_t* cr = cairo_create (surface);
	surface_clear (cr);
	if (width < 2 || height < 2 || sel_n1 <= sel_n0) {
		cairo_destroy (cr);
		return;
	}

	cairo_set_source_rgba (cr, r, g, b, a);
	cairo_set_antialias (cr, CAIRO_ANTIALIAS_DEFAULT);

	if (fade_in_n > sel_n0 + 0.0005f) {
		double x0 = (double)sel_n0 * (double)width;
		double xf = (double)fade_in_n * (double)width;
		cairo_move_to (cr, x0, (double)height);
		cairo_line_to (cr, xf, 0);
		cairo_line_to (cr, xf, (double)height);
		cairo_close_path (cr);
		cairo_fill (cr);
	}
	if (fade_out_n < sel_n1 - 0.0005f) {
		double x1 = (double)sel_n1 * (double)width;
		double xf = (double)fade_out_n * (double)width;
		cairo_move_to (cr, x1, (double)height);
		cairo_line_to (cr, xf, 0);
		cairo_line_to (cr, xf, (double)height);
		cairo_close_path (cr);
		cairo_fill (cr);
	}

	cairo_destroy (cr);
}

void
paint_notes_surface (cairo_surface_t* surface,
                     int width,
                     int height,
                     const CairoMidiNoteBlit* notes,
                     size_t n,
                     float dpi_scale)
{
	cairo_t* cr = cairo_create (surface);
	surface_clear (cr);
	if (!notes || n == 0 || width < 2 || height < 2) {
		cairo_destroy (cr);
		return;
	}

	const double stroke = std::max (1.0, 1.2 * (double)dpi_scale);
	cairo_set_antialias (cr, CAIRO_ANTIALIAS_DEFAULT);

	for (size_t i = 0; i < n; ++i) {
		const CairoMidiNoteBlit& note = notes[i];
		double x0 = (double)std::max (0.f, std::min (1.f, note.x0)) * (double)width;
		double x1 = (double)std::max (0.f, std::min (1.f, note.x1)) * (double)width;
		double y0 = (double)std::max (0.f, std::min (1.f, note.y0)) * (double)height;
		double y1 = (double)std::max (0.f, std::min (1.f, note.y1)) * (double)height;
		if (x1 < x0) {
			double t = x0;
			x0 = x1;
			x1 = t;
		}
		if (y1 < y0) {
			double t = y0;
			y0 = y1;
			y1 = t;
		}
		if (x1 - x0 < 1.0) {
			x1 = x0 + 1.0;
		}
		if (y1 - y0 < 1.0) {
			y1 = y0 + 1.0;
		}
		double rr = std::min (3.0 * (double)dpi_scale, (x1 - x0) * 0.25);
		cairo_new_path (cr);
		cairo_move_to (cr, x0 + rr, y0);
		cairo_line_to (cr, x1 - rr, y0);
		cairo_arc (cr, x1 - rr, y0 + rr, rr, -1.5707963267948966, 0);
		cairo_line_to (cr, x1, y1 - rr);
		cairo_arc (cr, x1 - rr, y1 - rr, rr, 0, 1.5707963267948966);
		cairo_line_to (cr, x0 + rr, y1);
		cairo_arc (cr, x0 + rr, y1 - rr, rr, 1.5707963267948966, 3.141592653589793);
		cairo_line_to (cr, x0, y0 + rr);
		cairo_arc (cr, x0 + rr, y0 + rr, rr, 3.141592653589793, 4.71238898038469);
		cairo_close_path (cr);
		cairo_set_source_rgba (cr, note.r, note.g, note.b, note.a);
		cairo_fill_preserve (cr);
		if (note.selected) {
			cairo_set_line_width (cr, stroke);
			cairo_set_source_rgba (cr, 0.95, 0.85, 0.35, 0.95);
			cairo_stroke (cr);
		} else {
			cairo_new_path (cr);
		}
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
	int width = 0;
	int height = 0;
	if (!dims_ok (dest.right - dest.left, dest.bottom - dest.top, &width, &height, 2048, 512)) {
		return false;
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
	bool ok = blit_surface_to_d2d (rt, dest, surface);
	cairo_surface_destroy (surface);
	return ok;
}

bool
CairoWaveBlit::blit_automation_curve (ID2D1RenderTarget* rt,
                                      const D2D1_RECT_F& dest,
                                      const float* y_0_1,
                                      size_t n,
                                      const float* pt_x_norm,
                                      const float* pt_y_0_1,
                                      size_t pt_n,
                                      float dpi_scale,
                                      float line_r, float line_g, float line_b)
{
	if (!rt || ((!y_0_1 || n < 2) && (!pt_x_norm || pt_n == 0))) {
		return false;
	}
	int width = 0;
	int height = 0;
	if (!dims_ok (dest.right - dest.left, dest.bottom - dest.top, &width, &height, 2048, 256)) {
		return false;
	}

	cairo_surface_t* surface =
	    cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
	if (!surface || cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
		if (surface) {
			cairo_surface_destroy (surface);
		}
		return false;
	}

	paint_curve_surface (surface, width, height, y_0_1, n, pt_x_norm, pt_y_0_1, pt_n,
	                     dpi_scale, line_r, line_g, line_b);
	bool ok = blit_surface_to_d2d (rt, dest, surface);
	cairo_surface_destroy (surface);
	return ok;
}

bool
CairoWaveBlit::blit_fade_wedges (ID2D1RenderTarget* rt,
                                 const D2D1_RECT_F& dest,
                                 float sel_n0, float sel_n1,
                                 float fade_in_n, float fade_out_n,
                                 float dpi_scale,
                                 float r, float g, float b, float a)
{
	(void)dpi_scale;
	if (!rt || sel_n1 <= sel_n0) {
		return false;
	}
	int width = 0;
	int height = 0;
	if (!dims_ok (dest.right - dest.left, dest.bottom - dest.top, &width, &height, 2048, 512)) {
		return false;
	}

	cairo_surface_t* surface =
	    cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
	if (!surface || cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
		if (surface) {
			cairo_surface_destroy (surface);
		}
		return false;
	}

	paint_fade_surface (surface, width, height, sel_n0, sel_n1, fade_in_n, fade_out_n,
	                    r, g, b, a);
	bool ok = blit_surface_to_d2d (rt, dest, surface);
	cairo_surface_destroy (surface);
	return ok;
}

bool
CairoWaveBlit::blit_midi_notes (ID2D1RenderTarget* rt,
                                const D2D1_RECT_F& dest,
                                const CairoMidiNoteBlit* notes,
                                size_t n,
                                float dpi_scale)
{
	if (!rt || !notes || n == 0) {
		return false;
	}
	int width = 0;
	int height = 0;
	if (!dims_ok (dest.right - dest.left, dest.bottom - dest.top, &width, &height, 2048, 512)) {
		return false;
	}

	cairo_surface_t* surface =
	    cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
	if (!surface || cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
		if (surface) {
			cairo_surface_destroy (surface);
		}
		return false;
	}

	paint_notes_surface (surface, width, height, notes, n, dpi_scale);
	bool ok = blit_surface_to_d2d (rt, dest, surface);
	cairo_surface_destroy (surface);
	return ok;
}

bool
CairoWaveBlit::blit_bars (ID2D1RenderTarget* rt,
                          const D2D1_RECT_F& dest,
                          const float* levels_0_1,
                          size_t n,
                          float dpi_scale,
                          float r, float g, float b, float a)
{
	if (!rt || !levels_0_1 || n == 0) {
		return false;
	}
	int width = 0;
	int height = 0;
	if (!dims_ok (dest.right - dest.left, dest.bottom - dest.top, &width, &height, 2048, 512)) {
		return false;
	}

	cairo_surface_t* surface =
	    cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
	if (!surface || cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
		if (surface) {
			cairo_surface_destroy (surface);
		}
		return false;
	}

	cairo_t* cr = cairo_create (surface);
	surface_clear (cr);
	const double gap = std::max (1.0, (double)dpi_scale);
	const double bar_w = ((double)width - gap * (double)(n - 1)) / (double)n;
	cairo_set_source_rgba (cr, r, g, b, a);
	cairo_set_antialias (cr, CAIRO_ANTIALIAS_DEFAULT);
	for (size_t i = 0; i < n; ++i) {
		float lv = std::max (0.f, std::min (1.f, levels_0_1[i]));
		double h = (double)lv * (double)height;
		if (h < 1.0 * (double)dpi_scale && lv > 0.01f) {
			h = 1.0 * (double)dpi_scale;
		}
		double x0 = (double)i * (bar_w + gap);
		double rr = std::min (2.0 * (double)dpi_scale, bar_w * 0.25);
		cairo_new_path (cr);
		cairo_move_to (cr, x0 + rr, (double)height - h);
		cairo_line_to (cr, x0 + bar_w - rr, (double)height - h);
		cairo_arc (cr, x0 + bar_w - rr, (double)height - h + rr, rr, -1.5707963267948966, 0);
		cairo_line_to (cr, x0 + bar_w, (double)height - rr);
		cairo_arc (cr, x0 + bar_w - rr, (double)height - rr, rr, 0, 1.5707963267948966);
		cairo_line_to (cr, x0 + rr, (double)height);
		cairo_arc (cr, x0 + rr, (double)height - rr, rr, 1.5707963267948966, 3.141592653589793);
		cairo_line_to (cr, x0, (double)height - h + rr);
		cairo_arc (cr, x0 + rr, (double)height - h + rr, rr, 3.141592653589793, 4.71238898038469);
		cairo_close_path (cr);
		cairo_fill (cr);
	}
	cairo_destroy (cr);

	bool ok = blit_surface_to_d2d (rt, dest, surface);
	cairo_surface_destroy (surface);
	return ok;
}

bool
CairoWaveBlit::smoke_offline ()
{
	uint32_t total_ink = 0;
	const char* fail_stage = nullptr;

	/* --- peaks --- */
	{
		const size_t n = 32;
		float peaks[32];
		float rms[32];
		for (size_t i = 0; i < n; ++i) {
			peaks[i] = 0.2f + 0.6f * ((float)(i % 8) / 7.f);
			rms[i] = peaks[i] * 0.55f;
		}
		cairo_surface_t* surface =
		    cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 128, 48);
		if (!surface || cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
			fail_stage = "peaks-surface";
			if (surface) {
				cairo_surface_destroy (surface);
			}
		} else {
			paint_peaks_surface (surface, 128, 48, peaks, rms, n, 1.f,
			                     0.95f, 0.55f, 0.20f, 0.35f, 0.55f, 0.75f);
			cairo_surface_flush (surface);
			uint32_t ink = count_inked (surface);
			cairo_surface_destroy (surface);
			if (ink < 64) {
				fail_stage = "peaks-ink";
			} else {
				total_ink += ink;
			}
		}
	}

	/* --- automation curve --- */
	if (!fail_stage) {
		float curve[16];
		float px[3] = { 0.1f, 0.5f, 0.9f };
		float py[3] = { 0.2f, 0.9f, 0.4f };
		for (int i = 0; i < 16; ++i) {
			curve[i] = 0.2f + 0.6f * ((float)i / 15.f);
		}
		cairo_surface_t* surface =
		    cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 160, 40);
		if (!surface || cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
			fail_stage = "curve-surface";
			if (surface) {
				cairo_surface_destroy (surface);
			}
		} else {
			paint_curve_surface (surface, 160, 40, curve, 16, px, py, 3, 1.f,
			                     0.30f, 0.75f, 0.95f);
			cairo_surface_flush (surface);
			uint32_t ink = count_inked (surface);
			cairo_surface_destroy (surface);
			if (ink < 32) {
				fail_stage = "curve-ink";
			} else {
				total_ink += ink;
			}
		}
	}

	/* --- fade wedges --- */
	if (!fail_stage) {
		cairo_surface_t* surface =
		    cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 128, 48);
		if (!surface || cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
			fail_stage = "fade-surface";
			if (surface) {
				cairo_surface_destroy (surface);
			}
		} else {
			paint_fade_surface (surface, 128, 48, 0.2f, 0.8f, 0.35f, 0.65f,
			                    0.40f, 0.70f, 0.95f, 0.45f);
			cairo_surface_flush (surface);
			uint32_t ink = count_inked (surface);
			cairo_surface_destroy (surface);
			if (ink < 32) {
				fail_stage = "fade-ink";
			} else {
				total_ink += ink;
			}
		}
	}

	/* --- MIDI notes --- */
	if (!fail_stage) {
		CairoMidiNoteBlit notes[3] = {
			{ 0.10f, 0.20f, 0.35f, 0.45f, 0.35f, 0.65f, 0.90f, 0.75f, false },
			{ 0.40f, 0.50f, 0.70f, 0.80f, 0.95f, 0.55f, 0.20f, 0.90f, true },
			{ 0.72f, 0.15f, 0.90f, 0.35f, 0.35f, 0.65f, 0.90f, 0.70f, false },
		};
		cairo_surface_t* surface =
		    cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 160, 64);
		if (!surface || cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
			fail_stage = "notes-surface";
			if (surface) {
				cairo_surface_destroy (surface);
			}
		} else {
			paint_notes_surface (surface, 160, 64, notes, 3, 1.f);
			cairo_surface_flush (surface);
			uint32_t ink = count_inked (surface);
			cairo_surface_destroy (surface);
			if (ink < 64) {
				fail_stage = "notes-ink";
			} else {
				total_ink += ink;
			}
		}
	}

	if (fail_stage) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-cairo-wave FAILED (%s)\n",
		              fail_stage);
		std::fflush (stderr);
		return false;
	}

	std::fprintf (stderr,
	              "airdour_win_shell: smoke-cairo-wave ok (cairo %s; peaks+curve+fades+notes inked=%u; no GTK chrome)\n",
	              cairo_version_string (), total_ink);
	std::fflush (stderr);
	return true;
}

#endif /* AIRDOUR_HAVE_CAIRO */
