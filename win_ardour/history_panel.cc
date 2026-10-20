/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "history_panel.h"

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
	float y = _panel.top + 26.f * dpi_scale;
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
HistoryPanel::draw (ID2D1RenderTarget* rt,
                    ID2D1SolidColorBrush* brand,
                    ID2D1SolidColorBrush* body,
                    ID2D1SolidColorBrush* accent,
                    ID2D1SolidColorBrush* pane,
                    ID2D1SolidColorBrush* edge,
                    IDWriteTextFormat* fmt_title,
                    IDWriteTextFormat* fmt_meta,
                    float dpi_scale) const
{
	if (!rt) {
		return;
	}

	rt->FillRectangle (_panel, pane);
	rt->DrawRectangle (_panel, edge, 1.f);
	rt->FillRectangle (
		D2D1::RectF (_panel.left, _panel.top, _panel.left + 3.f * dpi_scale, _panel.bottom),
		accent);

	DWRITE_TEXT_ALIGNMENT ta = fmt_title->GetTextAlignment ();
	fmt_title->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
	rt->DrawTextW (L"History", 7, fmt_title,
	               D2D1::RectF (_panel.left, _panel.top + 3.f * dpi_scale,
	                            _panel.right, _panel.top + 24.f * dpi_scale),
	               brand);
	fmt_title->SetTextAlignment (ta);

	DWRITE_TEXT_ALIGNMENT ta_m = fmt_meta->GetTextAlignment ();
	DWRITE_PARAGRAPH_ALIGNMENT pa_m = fmt_meta->GetParagraphAlignment ();
	fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
	fmt_meta->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

	auto draw_btn = [&] (const D2D1_RECT_F& r, const wchar_t* label, bool enabled) {
		rt->FillRectangle (r, enabled ? accent : edge);
		rt->DrawTextW (label, (UINT32)wcslen (label), fmt_meta, r, brand);
	};

	if (!_available) {
		rt->FillRectangle (_undo_btn, edge);
		rt->DrawTextW (L"—", 1, fmt_meta, _undo_btn, body);
		fmt_meta->SetTextAlignment (ta_m);
		fmt_meta->SetParagraphAlignment (pa_m);
		return;
	}

	draw_btn (_undo_btn, L"Undo", _undo_depth > 0);
	draw_btn (_redo_btn, L"Redo", _redo_depth > 0);

	fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_LEADING);
	fmt_meta->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

	wchar_t meta[64];
	_snwprintf_s (meta, _TRUNCATE, L"U %u · R %u", (unsigned)_undo_depth, (unsigned)_redo_depth);
	/* Depths drawn under buttons inside first row margin if empty. */

	for (int i = 0; i < _row_count; ++i) {
		bool top = (i == 0);
		rt->FillRectangle (_undo_rows[i], top ? accent : edge);
		if (top) {
			accent->SetOpacity (0.35f);
			rt->FillRectangle (_undo_rows[i], accent);
			accent->SetOpacity (1.f);
		}
		wchar_t line[48];
		_snwprintf_s (line, _TRUNCATE, L"%d  %s", i + 1, _row_name[i]);
		rt->DrawTextW (line, (UINT32)wcslen (line), fmt_meta,
		               D2D1::RectF (_undo_rows[i].left + 4.f * dpi_scale, _undo_rows[i].top,
		                            _undo_rows[i].right - 2.f * dpi_scale, _undo_rows[i].bottom),
		               brand);
	}

	if (_row_count == 0) {
		rt->DrawTextW (meta, (UINT32)wcslen (meta), fmt_meta, _undo_rows[0], body);
	}

	if (_redo_depth > 0) {
		rt->FillRectangle (_redo_next_row, edge);
		wchar_t rline[56];
		if (_next_redo[0]) {
			_snwprintf_s (rline, _TRUNCATE, L"↷ %s", _next_redo);
		} else {
			_snwprintf_s (rline, _TRUNCATE, L"↷ redo ×%u", (unsigned)_redo_depth);
		}
		rt->DrawTextW (rline, (UINT32)wcslen (rline), fmt_meta,
		               D2D1::RectF (_redo_next_row.left + 4.f * dpi_scale, _redo_next_row.top,
		                            _redo_next_row.right - 2.f * dpi_scale, _redo_next_row.bottom),
		               brand);
	}

	fmt_meta->SetTextAlignment (ta_m);
	fmt_meta->SetParagraphAlignment (pa_m);
}
