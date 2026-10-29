/*
 * Copyright (C) 2026 Airdour contributors
 *
 * UiDraw: shared Direct2D drawing context with theme brushes, DPI-aware
 * DirectWrite formats and reusable control primitives.
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
#include <d2d1helper.h>
#include <dwrite.h>

#include "ui_theme.h"

class UiDraw
{
public:
	static constexpr int kBrushCount = ui::kColorTokenCount;
	static constexpr int kFormatCount = ui::kTextRoleCount;

	UiDraw ();
	~UiDraw ();

	UiDraw (const UiDraw&) = delete;
	UiDraw& operator= (const UiDraw&) = delete;

	bool attach (ID2D1RenderTarget* rt, IDWriteFactory* dw, float dpi_scale);
	void detach ();
	void rebuild_text_formats (float dpi_scale);
	bool ready () const { return _rt && _brushes[0] && _formats[0]; }

	ID2D1RenderTarget* rt () const { return _rt; }
	ID2D1SolidColorBrush* brush (ui::ColorToken token) const;
	IDWriteTextFormat* fmt (ui::TextRole role) const;
	float dp (float dip) const { return dip * _dpi_scale; }
	float dpi_scale () const { return _dpi_scale; }

	/* Surfaces */
	void fill_round (const D2D1_RECT_F& r, float radius_dip, ui::ColorToken c,
	                 float opacity = 1.f);
	void stroke_round (const D2D1_RECT_F& r, float radius_dip, ui::ColorToken c,
	                   float width_dip = 1.f, float opacity = 1.f);
	void panel (const D2D1_RECT_F& r, float radius_dip = 6.f, bool raised = true);
	void card (const D2D1_RECT_F& r);
	void shadow (const D2D1_RECT_F& r, float radius_dip = 10.f);
	void divider (const D2D1_RECT_F& r);

	/* Text */
	void text (const wchar_t* s, ui::TextRole role, ui::ColorToken c,
	           const D2D1_RECT_F& r,
	           DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
	void text_n (const wchar_t* s, UINT32 len, ui::TextRole role, ui::ColorToken c,
	             const D2D1_RECT_F& r,
	             DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
	void text_vcenter (const wchar_t* s, ui::TextRole role, ui::ColorToken c,
	                   const D2D1_RECT_F& r,
	                   DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
	void text_vcenter_n (const wchar_t* s, UINT32 len, ui::TextRole role,
	                     ui::ColorToken c, const D2D1_RECT_F& r,
	                     DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);

	/* Controls */
	struct ButtonStyle {
		const wchar_t* label = nullptr;
		const wchar_t* glyph = nullptr;
		bool latched = false;
		bool hovered = false;
		bool pressed = false;
		bool show_label = true;
		ui::ColorToken latch = ui::ColorToken::Accent;
	};
	void button (const D2D1_RECT_F& r, const ButtonStyle& st);
	void slider (const D2D1_RECT_F& r, float t01, ui::ColorToken fill,
	             bool hovered = false);
	void meter (const D2D1_RECT_F& r, float peak01, float rms01, float hold01,
	            bool clipped, bool draw_ticks = false);

	/* Overlays */
	void modal_scrim (const D2D1_RECT_F& view, float opacity = 0.62f);
	void tooltip (const wchar_t* text, D2D1_POINT_2F cursor,
	              const D2D1_RECT_F& window);

private:
	bool make_format (const wchar_t* preferred, const wchar_t* fallback,
	                  DWRITE_FONT_WEIGHT weight, float size_dip,
	                  IDWriteTextFormat** out);
	void release_brushes ();
	void release_formats ();

	ID2D1RenderTarget* _rt;
	IDWriteFactory* _dw;
	float _dpi_scale;
	ID2D1SolidColorBrush* _brushes[kBrushCount];
	IDWriteTextFormat* _formats[kFormatCount];
};
