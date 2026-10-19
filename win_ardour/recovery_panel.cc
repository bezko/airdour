/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "recovery_panel.h"

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

RecoveryPanel::RecoveryPanel ()
	: _card ()
	, _restore_btn ()
	, _discard_btn ()
	, _visible (false)
{
	_label[0] = L'\0';
	_path[0] = L'\0';
}

void
RecoveryPanel::layout (float view_w, float view_h, float dpi_scale)
{
	float pad = 24.f * dpi_scale;
	float card_w = std::min (view_w - pad * 2.f, 480.f * dpi_scale);
	float card_h = std::min (view_h - pad * 2.f, 220.f * dpi_scale);
	float cx = (view_w - card_w) * 0.5f;
	float cy = (view_h - card_h) * 0.5f;
	_card = D2D1::RectF (cx, cy, cx + card_w, cy + card_h);

	float btn_h = 28.f * dpi_scale;
	float gap = 10.f * dpi_scale;
	float btn_w = (card_w - 32.f * dpi_scale - gap) * 0.5f;
	float y = _card.bottom - 16.f * dpi_scale - btn_h;
	float x = _card.left + 16.f * dpi_scale;
	_restore_btn = D2D1::RectF (x, y, x + btn_w, y + btn_h);
	_discard_btn = D2D1::RectF (x + btn_w + gap, y, x + btn_w + gap + btn_w, y + btn_h);
}

void
RecoveryPanel::set_session_hint (const wchar_t* label, const wchar_t* path)
{
	if (label && label[0]) {
		wcsncpy_s (_label, label, _TRUNCATE);
	} else {
		wcsncpy_s (_label, L"session", _TRUNCATE);
	}
	if (path && path[0]) {
		wcsncpy_s (_path, kPathLen, path, _TRUNCATE);
	} else {
		_path[0] = L'\0';
	}
}

RecoveryHit
RecoveryPanel::hit_test (float x, float y) const
{
	if (!_visible) {
		return RecoveryHit::None;
	}
	if (contains (_restore_btn, x, y)) {
		return RecoveryHit::Restore;
	}
	if (contains (_discard_btn, x, y)) {
		return RecoveryHit::Discard;
	}
	return RecoveryHit::None;
}

void
RecoveryPanel::draw (ID2D1RenderTarget* rt,
                     ID2D1SolidColorBrush* brand,
                     ID2D1SolidColorBrush* body,
                     ID2D1SolidColorBrush* accent,
                     ID2D1SolidColorBrush* pane,
                     ID2D1SolidColorBrush* edge,
                     IDWriteTextFormat* fmt_title,
                     IDWriteTextFormat* fmt_meta,
                     float dpi_scale) const
{
	if (!rt || !_visible) {
		return;
	}

	pane->SetOpacity (0.82f);
	D2D1_SIZE_F sz = rt->GetSize ();
	rt->FillRectangle (D2D1::RectF (0, 0, sz.width, sz.height), pane);
	pane->SetOpacity (1.f);

	rt->FillRectangle (_card, pane);
	rt->DrawRectangle (_card, accent, 1.5f * dpi_scale);

	DWRITE_TEXT_ALIGNMENT ta = fmt_title->GetTextAlignment ();
	fmt_title->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_LEADING);
	rt->DrawTextW (L"Airdour — recover session?", 26, fmt_title,
	               D2D1::RectF (_card.left + 16.f * dpi_scale, _card.top + 12.f * dpi_scale,
	                            _card.right - 16.f * dpi_scale, _card.top + 40.f * dpi_scale),
	               brand);
	fmt_title->SetTextAlignment (ta);

	wchar_t line[160];
	_snwprintf_s (line, _TRUNCATE, L"Autosave is newer than the last clean open.");
	rt->DrawTextW (line, (UINT32)wcslen (line), fmt_meta,
	               D2D1::RectF (_card.left + 16.f * dpi_scale, _card.top + 48.f * dpi_scale,
	                            _card.right - 16.f * dpi_scale, _card.top + 70.f * dpi_scale),
	               body);

	_snwprintf_s (line, _TRUNCATE, L"%s", _label);
	rt->DrawTextW (line, (UINT32)wcslen (line), fmt_meta,
	               D2D1::RectF (_card.left + 16.f * dpi_scale, _card.top + 74.f * dpi_scale,
	                            _card.right - 16.f * dpi_scale, _card.top + 94.f * dpi_scale),
	               brand);
	if (_path[0]) {
		rt->DrawTextW (_path, (UINT32)wcslen (_path), fmt_meta,
		               D2D1::RectF (_card.left + 16.f * dpi_scale, _card.top + 94.f * dpi_scale,
		                            _card.right - 16.f * dpi_scale, _restore_btn.top - 8.f * dpi_scale),
		               body);
	}

	auto draw_btn = [&] (const D2D1_RECT_F& r, const wchar_t* text, bool primary) {
		rt->FillRectangle (r, primary ? accent : edge);
		DWRITE_TEXT_ALIGNMENT t = fmt_meta->GetTextAlignment ();
		DWRITE_PARAGRAPH_ALIGNMENT p = fmt_meta->GetParagraphAlignment ();
		fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
		fmt_meta->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		rt->DrawTextW (text, (UINT32)wcslen (text), fmt_meta, r, brand);
		fmt_meta->SetTextAlignment (t);
		fmt_meta->SetParagraphAlignment (p);
	};
	draw_btn (_restore_btn, L"Restore", true);
	draw_btn (_discard_btn, L"Discard", false);
}
