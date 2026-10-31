/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "history_panel.h"

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

HistoryPanel::HistoryPanel ()
	: _panel ()
	, _undo_btn ()
	, _redo_btn ()
	, _redo_next_row ()
	, _row_count (0)
	, _undo_depth (0)
	, _redo_depth (0)
	, _available (false)
{
	_next_undo[0] = L'\0';
	_next_redo[0] = L'\0';
	for (int i = 0; i < kMaxRows; ++i) {
		_undo_rows[i] = D2D1::RectF ();
		_row_name[i][0] = L'\0';
	}
}

void
HistoryPanel::layout (const D2D1_RECT_F& bounds, float dpi_scale)
{
	_panel = bounds;
	float pad = 6.f * dpi_scale;
	float y = _panel.top + 32.f * dpi_scale;
	float btn_h = 22.f * dpi_scale;
	float gap = 4.f * dpi_scale;
	float half = (_panel.right - _panel.left - pad * 3.f) * 0.5f;
	_undo_btn = D2D1::RectF (_panel.left + pad, y, _panel.left + pad + half, y + btn_h);
	_redo_btn = D2D1::RectF (_undo_btn.right + pad, y, _panel.right - pad, y + btn_h);
	y = _undo_btn.bottom + gap + 2.f * dpi_scale;

	float row_h = 18.f * dpi_scale;
	for (int i = 0; i < kMaxRows; ++i) {
		_undo_rows[i] = D2D1::RectF (_panel.left + pad, y, _panel.right - pad, y + row_h);
		y = _undo_rows[i].bottom + 2.f * dpi_scale;
	}
	_redo_next_row = D2D1::RectF (_panel.left + pad, y + 4.f * dpi_scale,
	                              _panel.right - pad, y + 4.f * dpi_scale + row_h);
}

void
HistoryPanel::set_next_names (const wchar_t* undo_name, const wchar_t* redo_name)
{
	if (undo_name && undo_name[0]) {
		wcsncpy_s (_next_undo, kNameLen, undo_name, _TRUNCATE);
	} else {
		_next_undo[0] = L'\0';
	}
	if (redo_name && redo_name[0]) {
		wcsncpy_s (_next_redo, kNameLen, redo_name, _TRUNCATE);
	} else {
		_next_redo[0] = L'\0';
	}
}

void
HistoryPanel::set_undo_rows (int count, const wchar_t names[][kNameLen])
{
	_row_count = 0;
	if (!names || count <= 0) {
		return;
	}
	if (count > kMaxRows) {
		count = kMaxRows;
	}
	for (int i = 0; i < count; ++i) {
		wcsncpy_s (_row_name[i], kNameLen, names[i], _TRUNCATE);
	}
	_row_count = count;
}

HistoryHit
HistoryPanel::hit_test (float x, float y, int* out_row) const
{
	if (out_row) {
		*out_row = -1;
	}
	if (!_available) {
		return HistoryHit::None;
	}
	if (contains (_undo_btn, x, y)) {
		return HistoryHit::UndoBtn;
	}
	if (contains (_redo_btn, x, y)) {
		return HistoryHit::RedoBtn;
	}
	for (int i = 0; i < _row_count; ++i) {
		if (contains (_undo_rows[i], x, y)) {
			if (out_row) {
				*out_row = i;
			}
			return HistoryHit::UndoRow;
		}
	}
	if (_redo_depth > 0 && contains (_redo_next_row, x, y)) {
		return HistoryHit::RedoNext;
	}
	return HistoryHit::None;
}

void
HistoryPanel::draw (UiDraw& ui) const
{
	const D2D1_RECT_F& p = _panel;
	const float s = ui.dpi_scale ();
	const float hair = std::max (1.f, s);

	ui.card (p);

	const float header_h = 26.f * s;
	const D2D1_RECT_F title_rc = D2D1::RectF (p.left + 12.f * s, p.top,
	                                          p.right - 12.f * s, p.top + header_h);
	ui.text_vcenter (L"History", ui::TextRole::Section,
	                 _available ? ui::ColorToken::TextPrimary
	                            : ui::ColorToken::TextMuted,
	                 title_rc);
	wchar_t meta[64];
	_snwprintf_s (meta, _TRUNCATE, L"%u · %u",
	              (unsigned)_undo_depth, (unsigned)_redo_depth);
	ui.text_vcenter (meta, ui::TextRole::Caption, ui::ColorToken::TextMuted,
	                 title_rc, DWRITE_TEXT_ALIGNMENT_TRAILING);
	ui.divider (D2D1::RectF (p.left + 8.f * s, p.top + header_h,
	                         p.right - 8.f * s, p.top + header_h + hair));

	auto draw_btn = [&] (const D2D1_RECT_F& r, const wchar_t* label,
	                     bool enabled, bool latched) {
		if (enabled) {
			UiDraw::ButtonStyle st;
			st.label = label;
			st.latched = latched;
			ui.button (r, st);
		} else {
			ui.fill_round (r, 4.f, ui::ColorToken::Control, 0.4f);
			ui.stroke_round (r, 4.f, ui::ColorToken::Border, 1.f, 0.6f);
			ui.text_vcenter (label, ui::TextRole::Body,
			                 ui::ColorToken::TextDisabled, r,
			                 DWRITE_TEXT_ALIGNMENT_CENTER);
		}
	};

	draw_btn (_undo_btn, L"Undo", _undo_depth > 0, false);
	draw_btn (_redo_btn, L"Redo", _redo_depth > 0, false);

	for (int i = 0; i < _row_count; ++i) {
		const bool top = (i == 0);
		if (top) {
			ui.fill_round (_undo_rows[i], 3.f, ui::ColorToken::Control, 0.85f);
			ui.fill_round (D2D1::RectF (_undo_rows[i].left, _undo_rows[i].top + 2.f * s,
			                             _undo_rows[i].left + 2.f * s,
			                             _undo_rows[i].bottom - 2.f * s),
			                1.f, ui::ColorToken::Accent, 0.9f);
		}
		wchar_t line[56];
		_snwprintf_s (line, _TRUNCATE, L"%d  %s", i + 1, _row_name[i]);
		ui.text_vcenter (line, ui::TextRole::Body,
		                 top ? ui::ColorToken::TextPrimary
		                     : ui::ColorToken::TextSecondary,
		                 D2D1::RectF (_undo_rows[i].left + 8.f * s, _undo_rows[i].top,
		                              _undo_rows[i].right - 2.f * s, _undo_rows[i].bottom));
	}

	if (_row_count == 0) {
		ui.text_vcenter (meta, ui::TextRole::Caption, ui::ColorToken::TextMuted,
		                 _undo_rows[0]);
	}

	if (_redo_depth > 0) {
		ui.fill_round (_redo_next_row, 3.f, ui::ColorToken::Control, 0.32f);
		wchar_t rline[64];
		if (_next_redo[0]) {
			_snwprintf_s (rline, _TRUNCATE, L"↷ %s", _next_redo);
		} else {
			_snwprintf_s (rline, _TRUNCATE, L"↷ redo ×%u", (unsigned)_redo_depth);
		}
		ui.text_vcenter (rline, ui::TextRole::Caption,
		                 ui::ColorToken::TextSecondary,
		                 D2D1::RectF (_redo_next_row.left + 8.f * s, _redo_next_row.top,
		                              _redo_next_row.right - 2.f * s,
		                              _redo_next_row.bottom));
	}
}
