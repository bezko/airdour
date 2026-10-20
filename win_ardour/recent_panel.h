/*
 * Copyright (C) 2026 Airdour contributors
 *
 * D2D recent-sessions menu anchored under Open. Brand Airdour — no GTK.
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

enum class RecentHit {
	None,
	Outside, /* dismiss menu */
	Browse,
	Clear,
	Row0,
	Row1,
	Row2,
	Row3,
	Row4,
	Row5,
	Row6,
	Row7
};

class RecentPanel
{
public:
	static constexpr int kMaxRows = 8;
	static constexpr int kLabelLen = 48;
	static constexpr int kPathLen = 96;

	RecentPanel ();

	void layout_under (const D2D1_RECT_F& open_btn, float view_w, float view_h, float dpi_scale);

	void set_visible (bool on) { _visible = on; }
	bool visible () const { return _visible; }

	void set_rows (int count,
	               const wchar_t labels[][kLabelLen],
	               const wchar_t paths[][kPathLen]);

	RecentHit hit_test (float x, float y) const;
	/** Row index 0..count-1 for Row* hits; -1 otherwise. */
	static int row_index (RecentHit hit);

	void draw (ID2D1RenderTarget* rt,
	           ID2D1SolidColorBrush* brand,
	           ID2D1SolidColorBrush* body,
	           ID2D1SolidColorBrush* accent,
	           ID2D1SolidColorBrush* pane,
	           ID2D1SolidColorBrush* edge,
	           IDWriteTextFormat* fmt_meta,
	           float dpi_scale) const;

private:
	D2D1_RECT_F _menu;
	D2D1_RECT_F _browse_btn;
	D2D1_RECT_F _clear_btn;
	D2D1_RECT_F _row_btn[kMaxRows];
	int         _row_count;
	bool        _visible;
	wchar_t     _label[kMaxRows][kLabelLen];
	wchar_t     _path[kMaxRows][kPathLen];
};
