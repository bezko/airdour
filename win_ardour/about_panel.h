/*
 * Copyright (C) 2026 Airdour contributors
 *
 * D2D About card + brief startup splash. Brand Airdour — no GTK.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

class AboutPanel
{
public:
	static constexpr const wchar_t* kVersion = L"0.1.0";

	AboutPanel ();

	void layout (float view_w, float view_h, float dpi_scale);

	void set_about_visible (bool on) { _about_visible = on; }
	bool about_visible () const { return _about_visible; }

	void set_splash_visible (bool on) { _splash_visible = on; }
	bool splash_visible () const { return _splash_visible; }

	/** True if (x,y) hits the About Close control (About mode only). */
	bool hit_close (float x, float y) const;
	/** True if click should dismiss About (close or outside card). */
	bool hit_dismiss_about (float x, float y) const;

	void draw_about (ID2D1RenderTarget* rt,
	                 ID2D1SolidColorBrush* brand,
	                 ID2D1SolidColorBrush* body,
	                 ID2D1SolidColorBrush* accent,
	                 ID2D1SolidColorBrush* pane,
	                 ID2D1SolidColorBrush* edge,
	                 IDWriteTextFormat* fmt_title,
	                 IDWriteTextFormat* fmt_meta,
	                 float dpi_scale) const;

	void draw_splash (ID2D1RenderTarget* rt,
	                  ID2D1SolidColorBrush* brand,
	                  ID2D1SolidColorBrush* body,
	                  ID2D1SolidColorBrush* accent,
	                  ID2D1SolidColorBrush* pane,
	                  IDWriteTextFormat* fmt_brand,
	                  IDWriteTextFormat* fmt_meta,
	                  float dpi_scale,
	                  float pulse) const;

private:
	D2D1_RECT_F _card;
	D2D1_RECT_F _close_btn;
	bool        _about_visible;
	bool        _splash_visible;
};
