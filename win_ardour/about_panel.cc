/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "about_panel.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

bool
contains (const D2D1_RECT_F& r, float x, float y)
{
	return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

} // namespace

AboutPanel::AboutPanel ()
	: _card ()
	, _close_btn ()
	, _about_visible (false)
	, _splash_visible (false)
{
}

void
AboutPanel::layout (float view_w, float view_h, float dpi_scale)
{
	float pad = 24.f * dpi_scale;
	float card_w = std::min (view_w - pad * 2.f, 440.f * dpi_scale);
	float card_h = std::min (view_h - pad * 2.f, 280.f * dpi_scale);
	float cx = (view_w - card_w) * 0.5f;
	float cy = (view_h - card_h) * 0.5f;
	_card = D2D1::RectF (cx, cy, cx + card_w, cy + card_h);
	_close_btn = D2D1::RectF (_card.right - 70.f * dpi_scale, _card.top + 8.f * dpi_scale,
	                          _card.right - 12.f * dpi_scale, _card.top + 30.f * dpi_scale);
}

bool
AboutPanel::hit_dismiss_about (float x, float y) const
{
	if (!_about_visible) {
		return false;
	}
	if (contains (_close_btn, x, y)) {
		return true;
	}
	/* Outside card → dismiss; inside card (not Close) → keep open. */
	return !contains (_card, x, y);
}

void
AboutPanel::draw_about (ID2D1RenderTarget* rt,
                        ID2D1SolidColorBrush* brand,
                        ID2D1SolidColorBrush* body,
                        ID2D1SolidColorBrush* accent,
                        ID2D1SolidColorBrush* pane,
                        ID2D1SolidColorBrush* edge,
                        IDWriteTextFormat* fmt_title,
                        IDWriteTextFormat* fmt_meta,
                        float dpi_scale) const
{
	if (!rt || !_about_visible) {
		return;
	}

	pane->SetOpacity (0.78f);
	D2D1_SIZE_F sz = rt->GetSize ();
	rt->FillRectangle (D2D1::RectF (0, 0, sz.width, sz.height), pane);
	pane->SetOpacity (1.f);

	rt->FillRectangle (_card, pane);
	rt->DrawRectangle (_card, accent, 1.5f * dpi_scale);

	DWRITE_TEXT_ALIGNMENT ta = fmt_title->GetTextAlignment ();
	fmt_title->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_LEADING);
	rt->DrawTextW (L"Airdour", 7, fmt_title,
	               D2D1::RectF (_card.left + 16.f * dpi_scale, _card.top + 10.f * dpi_scale,
	                            _close_btn.left - 8.f * dpi_scale, _card.top + 36.f * dpi_scale),
	               brand);
	fmt_title->SetTextAlignment (ta);

	auto draw_btn = [&] (const D2D1_RECT_F& r, const wchar_t* label) {
		rt->FillRectangle (r, edge);
		DWRITE_TEXT_ALIGNMENT t = fmt_meta->GetTextAlignment ();
		DWRITE_PARAGRAPH_ALIGNMENT p = fmt_meta->GetParagraphAlignment ();
		fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
		fmt_meta->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		rt->DrawTextW (label, (UINT32)wcslen (label), fmt_meta, r, brand);
		fmt_meta->SetTextAlignment (t);
		fmt_meta->SetParagraphAlignment (p);
	};
	draw_btn (_close_btn, L"Close");

	wchar_t line[128];
	float y = _card.top + 48.f * dpi_scale;
	float x0 = _card.left + 18.f * dpi_scale;
	float x1 = _card.right - 18.f * dpi_scale;
	float row = 22.f * dpi_scale;

	_snwprintf_s (line, _TRUNCATE, L"Version  %s", kVersion);
	rt->DrawTextW (line, (UINT32)wcslen (line), fmt_meta,
	               D2D1::RectF (x0, y, x1, y + row), body);
	y += row;

	rt->DrawTextW (L"Windows-native shell", 20, fmt_meta,
	               D2D1::RectF (x0, y, x1, y + row), body);
	y += row;

	rt->DrawTextW (L"Audio  ·  WASAPI shared (Dummy for headless)", 44, fmt_meta,
	               D2D1::RectF (x0, y, x1, y + row), body);
	y += row;

	rt->DrawTextW (L"UI  ·  Win32 + Direct2D / DirectWrite", 37, fmt_meta,
	               D2D1::RectF (x0, y, x1, y + row), body);
	y += row;

	rt->DrawTextW (L"No GTK", 6, fmt_meta,
	               D2D1::RectF (x0, y, x1, y + row), accent);
	y += row * 1.4f;

	rt->DrawTextW (L"Brand first. Session on the UI thread; audio on RT.", 50, fmt_meta,
	               D2D1::RectF (x0, y, x1, y + row), body);
}

void
AboutPanel::draw_splash (ID2D1RenderTarget* rt,
                         ID2D1SolidColorBrush* brand,
                         ID2D1SolidColorBrush* body,
                         ID2D1SolidColorBrush* accent,
                         ID2D1SolidColorBrush* pane,
                         IDWriteTextFormat* fmt_brand,
                         IDWriteTextFormat* fmt_meta,
                         float dpi_scale,
                         float pulse) const
{
	if (!rt || !_splash_visible) {
		return;
	}

	D2D1_SIZE_F sz = rt->GetSize ();
	pane->SetOpacity (0.92f);
	rt->FillRectangle (D2D1::RectF (0, 0, sz.width, sz.height), pane);
	pane->SetOpacity (1.f);

	float cx = sz.width * 0.5f;
	float cy = sz.height * 0.42f;
	float brand_h = 72.f * dpi_scale;
	D2D1_RECT_F brand_r = D2D1::RectF (cx - 200.f * dpi_scale, cy - brand_h * 0.5f,
	                                   cx + 200.f * dpi_scale, cy + brand_h * 0.5f);

	brand->SetOpacity (0.92f + 0.08f * pulse);
	DWRITE_TEXT_ALIGNMENT ta = fmt_brand->GetTextAlignment ();
	DWRITE_PARAGRAPH_ALIGNMENT pa = fmt_brand->GetParagraphAlignment ();
	fmt_brand->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
	fmt_brand->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	rt->DrawTextW (L"Airdour", 7, fmt_brand, brand_r, brand);
	fmt_brand->SetTextAlignment (ta);
	fmt_brand->SetParagraphAlignment (pa);
	brand->SetOpacity (1.f);

	float rule_w = (120.f + 40.f * pulse) * dpi_scale;
	rt->FillRectangle (D2D1::RectF (cx - rule_w * 0.5f, brand_r.bottom + 4.f * dpi_scale,
	                                cx + rule_w * 0.5f, brand_r.bottom + 6.f * dpi_scale),
	                   accent);

	wchar_t sub[96];
	_snwprintf_s (sub, _TRUNCATE, L"v%s  ·  WASAPI shared  ·  Direct2D", kVersion);
	rt->DrawTextW (sub, (UINT32)wcslen (sub), fmt_meta,
	               D2D1::RectF (cx - 220.f * dpi_scale, brand_r.bottom + 16.f * dpi_scale,
	                            cx + 220.f * dpi_scale, brand_r.bottom + 40.f * dpi_scale),
	               body);

	rt->DrawTextW (L"Click or Esc to continue", 24, fmt_meta,
	               D2D1::RectF (cx - 160.f * dpi_scale, sz.height - 48.f * dpi_scale,
	                            cx + 160.f * dpi_scale, sz.height - 24.f * dpi_scale),
	               body);
}
