/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "recent_panel.h"

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

RecentPanel::RecentPanel ()
	: _menu ()
	, _browse_btn ()
	, _clear_btn ()
	, _row_count (0)
	, _visible (false)
{
	for (int i = 0; i < kMaxRows; ++i) {
		_row_btn[i] = D2D1::RectF ();
		_label[i][0] = L'\0';
		_path[i][0] = L'\0';
	}
}

void
RecentPanel::layout_under (const D2D1_RECT_F& open_btn, float view_w, float view_h, float dpi_scale)
{
	float pad = 6.f * dpi_scale;
	float row_h = 26.f * dpi_scale;
	float menu_w = std::min (420.f * dpi_scale, view_w - 16.f * dpi_scale);
	int rows = _row_count;
	float menu_h = pad * 2.f + row_h /* Browse */
	               + (rows > 0 ? rows * row_h : row_h) /* rows or empty hint */
	               + row_h /* Clear */;
	float left = open_btn.left;
	if (left + menu_w > view_w - 8.f * dpi_scale) {
		left = std::max (8.f * dpi_scale, view_w - menu_w - 8.f * dpi_scale);
	}
	float top = open_btn.bottom + 4.f * dpi_scale;
	if (top + menu_h > view_h - 8.f * dpi_scale) {
		top = std::max (8.f * dpi_scale, open_btn.top - menu_h - 4.f * dpi_scale);
	}
	_menu = D2D1::RectF (left, top, left + menu_w, top + menu_h);

	float y = _menu.top + pad;
	float x0 = _menu.left + pad;
	float x1 = _menu.right - pad;
	_browse_btn = D2D1::RectF (x0, y, x1, y + row_h);
	y = _browse_btn.bottom;
	for (int i = 0; i < kMaxRows; ++i) {
		if (i < rows) {
			_row_btn[i] = D2D1::RectF (x0, y, x1, y + row_h);
			y = _row_btn[i].bottom;
		} else {
			_row_btn[i] = D2D1::RectF ();
		}
	}
	if (rows == 0) {
		y += row_h; /* empty hint line */
	}
	_clear_btn = D2D1::RectF (x0, y, x1, y + row_h);
}

void
RecentPanel::set_rows (int count,
                       const wchar_t labels[][kLabelLen],
                       const wchar_t paths[][kPathLen])
{
	_row_count = std::max (0, std::min (count, kMaxRows));
	for (int i = 0; i < kMaxRows; ++i) {
		if (i < _row_count && labels && paths) {
			wcsncpy_s (_label[i], labels[i], _TRUNCATE);
			wcsncpy_s (_path[i], paths[i], _TRUNCATE);
		} else {
			_label[i][0] = L'\0';
			_path[i][0] = L'\0';
		}
	}
}

int
RecentPanel::row_index (RecentHit hit)
{
	switch (hit) {
	case RecentHit::Row0: return 0;
	case RecentHit::Row1: return 1;
	case RecentHit::Row2: return 2;
	case RecentHit::Row3: return 3;
	case RecentHit::Row4: return 4;
	case RecentHit::Row5: return 5;
	case RecentHit::Row6: return 6;
	case RecentHit::Row7: return 7;
	default: return -1;
	}
}

RecentHit
RecentPanel::hit_test (float x, float y) const
{
	if (!_visible) {
		return RecentHit::None;
	}
	if (contains (_browse_btn, x, y)) {
		return RecentHit::Browse;
	}
	for (int i = 0; i < _row_count; ++i) {
		if (contains (_row_btn[i], x, y)) {
			return static_cast<RecentHit> ((int)RecentHit::Row0 + i);
		}
	}
	if (contains (_clear_btn, x, y)) {
		return RecentHit::Clear;
	}
	if (contains (_menu, x, y)) {
		return RecentHit::None;
	}
	return RecentHit::Outside;
}

void
RecentPanel::draw (UiDraw& ui) const
{
	if (!ui.ready () || !_visible) {
		return;
	}

	const float s = ui.dpi_scale ();
	ui.shadow (_menu, 8.f);
	ui.fill_round (_menu, 8.f, ui::ColorToken::SurfaceRaised);
	ui.stroke_round (_menu, 8.f, ui::ColorToken::BorderStrong, 1.f);

	const float pad = 6.f * s;
	auto row_text = [&] (const D2D1_RECT_F& r, const wchar_t* text,
	                     ui::ColorToken color, ui::TextRole role) {
		ui.text_vcenter (text, role, color,
		                 D2D1::RectF (r.left + pad + 2.f * s, r.top,
		                              r.right - pad, r.bottom));
	};

	/* Browse row — primary action. */
	ui.fill_round (_browse_btn, 5.f, ui::ColorToken::Accent, 0.14f);
	row_text (_browse_btn, L"Browse…", ui::ColorToken::Accent, ui::TextRole::Body);

	if (_row_count == 0) {
		const D2D1_RECT_F hint = D2D1::RectF (_browse_btn.left, _browse_btn.bottom,
		                                      _browse_btn.right, _clear_btn.top);
		row_text (hint, L"(no recent sessions)", ui::ColorToken::TextMuted,
		          ui::TextRole::Caption);
	} else {
		for (int i = 0; i < _row_count; ++i) {
			wchar_t line[160];
			if (_path[i][0]) {
				_snwprintf_s (line, _TRUNCATE, L"%s  —  %s", _label[i], _path[i]);
			} else {
				wcsncpy_s (line, _label[i], _TRUNCATE);
			}
			row_text (_row_btn[i], line, ui::ColorToken::TextSecondary,
			          ui::TextRole::Body);
		}
		ui.divider (D2D1::RectF (_clear_btn.left + pad, _clear_btn.top,
		                          _clear_btn.right - pad,
		                          _clear_btn.top + std::max (1.f, s)));
	}

	ui.fill_round (_clear_btn, 5.f, ui::ColorToken::Control, 0.5f);
	row_text (_clear_btn, L"Clear recent", ui::ColorToken::TextSecondary,
	          ui::TextRole::Body);
}
