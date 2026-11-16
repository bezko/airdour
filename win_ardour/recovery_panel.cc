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
RecoveryPanel::draw (UiDraw& ui) const
{
	if (!ui.ready () || !_visible) {
		return;
	}

	const float s = ui.dpi_scale ();
	const D2D1_SIZE_F sz = ui.rt ()->GetSize ();
	ui.modal_scrim (D2D1::RectF (0.f, 0.f, sz.width, sz.height), 0.66f);

	const float radius = ui::theme ().metrics.radius_lg;
	ui.shadow (_card, radius + 2.f);
	ui.fill_round (_card, radius, ui::ColorToken::Card);
	ui.stroke_round (_card, radius, ui::ColorToken::BorderStrong, 1.f);

	/* Attention tab + title. */
	const float inset = 18.f * s;
	ui.fill_round (D2D1::RectF (_card.left + inset, _card.top + 13.f * s,
	                             _card.left + inset + 3.f * s, _card.top + 33.f * s),
	                1.5f, ui::ColorToken::Solo);
	ui.text_vcenter (L"Recover session?", ui::TextRole::Title,
	                 ui::ColorToken::TextPrimary,
	                 D2D1::RectF (_card.left + inset + 12.f * s, _card.top,
	                              _card.right - inset, _card.top + 42.f * s));
	ui.divider (D2D1::RectF (_card.left + inset, _card.top + 44.f * s,
	                          _card.right - inset, _card.top + 44.f * s + std::max (1.f, s)));

	const float x0 = _card.left + inset;
	const float x1 = _card.right - inset;
	ui.text_vcenter (L"Autosave is newer than the last clean open.",
	                 ui::TextRole::Caption, ui::ColorToken::TextSecondary,
	                 D2D1::RectF (x0, _card.top + 52.f * s, x1, _card.top + 74.f * s));
	ui.text_vcenter (_label, ui::TextRole::Body, ui::ColorToken::TextPrimary,
	                 D2D1::RectF (x0, _card.top + 76.f * s, x1, _card.top + 98.f * s));
	if (_path[0]) {
		ui.text_vcenter (_path, ui::TextRole::Caption, ui::ColorToken::TextMuted,
		                 D2D1::RectF (x0, _card.top + 98.f * s,
		                              x1, _restore_btn.top - 6.f * s));
	}

	UiDraw::ButtonStyle restore_st;
	restore_st.label = L"Restore";
	restore_st.latched = true;
	ui.button (_restore_btn, restore_st);

	UiDraw::ButtonStyle discard_st;
	discard_st.label = L"Discard";
	ui.button (_discard_btn, discard_st);
}
