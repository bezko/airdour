/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Compact D2D undo/redo history list. Click an undo row to jump via
 * Session::undo(n). Redo shows next label; click = redo(1). Ctrl+Z/Y stay primary.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

enum class HistoryHit {
	None,
	UndoBtn,
	RedoBtn,
	UndoRow, /* hit_index = 0..kMaxRows-1 (0 = newest) */
	RedoNext
};

class HistoryPanel
{
public:
	static constexpr int kMaxRows = 8;
	static constexpr int kNameLen = 40;

	HistoryPanel ();

	void layout (const D2D1_RECT_F& bounds, float dpi_scale);

	void set_available (bool ok) { _available = ok; }
	bool available () const { return _available; }

	void set_depths (uint32_t undo_n, uint32_t redo_n)
	{
		_undo_depth = undo_n;
		_redo_depth = redo_n;
	}

	void set_next_names (const wchar_t* undo_name, const wchar_t* redo_name);
	void set_undo_rows (int count, const wchar_t names[][kNameLen]);

	HistoryHit hit_test (float x, float y, int* out_row = nullptr) const;

	void draw (ID2D1RenderTarget* rt,
	           ID2D1SolidColorBrush* brand,
	           ID2D1SolidColorBrush* body,
	           ID2D1SolidColorBrush* accent,
	           ID2D1SolidColorBrush* pane,
	           ID2D1SolidColorBrush* edge,
	           IDWriteTextFormat* fmt_title,
	           IDWriteTextFormat* fmt_meta,
	           float dpi_scale) const;

	const D2D1_RECT_F& bounds () const { return _panel; }

private:
	D2D1_RECT_F _panel;
	D2D1_RECT_F _undo_btn;
	D2D1_RECT_F _redo_btn;
	D2D1_RECT_F _redo_next_row;
	D2D1_RECT_F _undo_rows[kMaxRows];
	int         _row_count;
	wchar_t     _row_name[kMaxRows][kNameLen];
	wchar_t     _next_undo[kNameLen];
	wchar_t     _next_redo[kNameLen];
	uint32_t    _undo_depth;
	uint32_t    _redo_depth;
	bool        _available;
};
