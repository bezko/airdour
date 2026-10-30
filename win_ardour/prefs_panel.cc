/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "prefs_panel.h"

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

PrefsPanel::PrefsPanel ()
	: _card ()
	, _close_btn ()
	, _be_dummy ()
	, _be_wasapi ()
	, _per_auto ()
	, _per_480 ()
	, _per_512 ()
	, _click_btn ()
	, _snap_btn ()
	, _vst3_rescan_btn ()
	, _vst3_bypass_btn ()
	, _vst3_param_reset_btn ()
	, _vst3_param_reset1_btn ()
	, _vst3_param_reset2_btn ()
	, _vst3_param_reset3_btn ()
	, _vst3_unload_btn ()
	, _save_btn ()
	, _visible (false)
	, _click_on (false)
	, _snap_on (false)
	, _vst3_ok (false)
	, _vst3_bypassed (false)
	, _cur_period (0)
	, _cur_sr (0)
	, _pref_period (0)
{
	wcsncpy_s (_cur_backend, L"Dummy", _TRUNCATE);
	wcsncpy_s (_pref_backend, L"Dummy", _TRUNCATE);
	_vst3_hint[0] = L'\0';
	_vst3_scan_count = 0;
	_vst3_catalog_count = 0;
	_vst3_loaded_name[0] = L'\0';
	for (int i = 0; i < kMaxCatalog; ++i) {
		_vst3_catalog[i][0] = L'\0';
		_vst3_catalog_row[i] = D2D1::RectF ();
	}
	_status[0] = L'\0';
}

void
PrefsPanel::layout (float view_w, float view_h, float dpi_scale)
{
	float pad = 24.f * dpi_scale;
	float card_w = std::min (view_w - pad * 2.f, 700.f * dpi_scale);
	float card_h = std::min (view_h - pad * 2.f, 520.f * dpi_scale);
	float cx = (view_w - card_w) * 0.5f;
	float cy = (view_h - card_h) * 0.5f;
	_card = D2D1::RectF (cx, cy, cx + card_w, cy + card_h);

	float x = _card.left + 16.f * dpi_scale;
	float y = _card.top + 44.f * dpi_scale;
	float btn_h = 24.f * dpi_scale;
	float gap = 6.f * dpi_scale;
	float half = (card_w - 32.f * dpi_scale - gap) * 0.5f;

	_be_dummy = D2D1::RectF (x, y, x + half, y + btn_h);
	_be_wasapi = D2D1::RectF (x + half + gap, y, x + half + gap + half, y + btn_h);
	y = _be_dummy.bottom + 28.f * dpi_scale;

	float third = (card_w - 32.f * dpi_scale - gap * 2.f) / 3.f;
	_per_auto = D2D1::RectF (x, y, x + third, y + btn_h);
	_per_480 = D2D1::RectF (_per_auto.right + gap, y, _per_auto.right + gap + third, y + btn_h);
	_per_512 = D2D1::RectF (_per_480.right + gap, y, _per_480.right + gap + third, y + btn_h);
	y = _per_auto.bottom + 28.f * dpi_scale;

	_click_btn = D2D1::RectF (x, y, x + half, y + btn_h);
	_snap_btn = D2D1::RectF (x + half + gap, y, x + half + gap + half, y + btn_h);
	/* Path hint + catalog rows sit between click/snap and Rescan. */
	float cat_y = _click_btn.bottom + 44.f * dpi_scale;
	float row_h = 15.f * dpi_scale;
	float cat_left = _card.left + 16.f * dpi_scale;
	float cat_right = _card.right - 16.f * dpi_scale;
	for (int i = 0; i < kMaxCatalog; ++i) {
		_vst3_catalog_row[i] = D2D1::RectF (cat_left, cat_y, cat_right, cat_y + row_h);
		cat_y += row_h;
	}
	y = cat_y + 8.f * dpi_scale;

	_vst3_rescan_btn = D2D1::RectF (x, y, x + 120.f * dpi_scale, y + btn_h);
	_vst3_bypass_btn = D2D1::RectF (_vst3_rescan_btn.right + gap, y,
	                                _vst3_rescan_btn.right + gap + 100.f * dpi_scale,
	                                y + btn_h);
	if (_vst3_ok) {
		float rw = 64.f * dpi_scale;
		_vst3_param_reset_btn = D2D1::RectF (_vst3_bypass_btn.right + gap, y,
		                                     _vst3_bypass_btn.right + gap + rw,
		                                     y + btn_h);
		_vst3_param_reset1_btn = D2D1::RectF (_vst3_param_reset_btn.right + gap, y,
		                                      _vst3_param_reset_btn.right + gap + rw,
		                                      y + btn_h);
		_vst3_param_reset2_btn = D2D1::RectF (_vst3_param_reset1_btn.right + gap, y,
		                                      _vst3_param_reset1_btn.right + gap + rw,
		                                      y + btn_h);
		_vst3_param_reset3_btn = D2D1::RectF (_vst3_param_reset2_btn.right + gap, y,
		                                      _vst3_param_reset2_btn.right + gap + rw,
		                                      y + btn_h);
		_vst3_unload_btn = D2D1::RectF (_vst3_param_reset3_btn.right + gap, y,
		                                _vst3_param_reset3_btn.right + gap + 72.f * dpi_scale,
		                                y + btn_h);
	} else {
		_vst3_param_reset_btn = D2D1::RectF (0.f, 0.f, 0.f, 0.f);
		_vst3_param_reset1_btn = D2D1::RectF (0.f, 0.f, 0.f, 0.f);
		_vst3_param_reset2_btn = D2D1::RectF (0.f, 0.f, 0.f, 0.f);
		_vst3_param_reset3_btn = D2D1::RectF (0.f, 0.f, 0.f, 0.f);
		_vst3_unload_btn = D2D1::RectF (0.f, 0.f, 0.f, 0.f);
	}

	_save_btn = D2D1::RectF (x, _card.bottom - 40.f * dpi_scale,
	                         x + 100.f * dpi_scale, _card.bottom - 14.f * dpi_scale);
	_close_btn = D2D1::RectF (_card.right - 70.f * dpi_scale, _card.top + 8.f * dpi_scale,
	                          _card.right - 12.f * dpi_scale, _card.top + 30.f * dpi_scale);
}

void
PrefsPanel::set_current_backend (const wchar_t* name)
{
	if (name && name[0]) {
		wcsncpy_s (_cur_backend, name, _TRUNCATE);
	}
}

void
PrefsPanel::set_pref_backend (const wchar_t* name)
{
	if (name && name[0]) {
		wcsncpy_s (_pref_backend, name, _TRUNCATE);
	}
}

void
PrefsPanel::set_current_period (uint32_t frames, uint32_t sample_rate)
{
	_cur_period = frames;
	_cur_sr = sample_rate;
}

void
PrefsPanel::set_pref_period (uint32_t frames)
{
	_pref_period = frames;
}

void
PrefsPanel::set_vst3_hint (const wchar_t* text)
{
	if (text && text[0]) {
		wcsncpy_s (_vst3_hint, kHintLen, text, _TRUNCATE);
	} else {
		_vst3_hint[0] = L'\0';
	}
}

void
PrefsPanel::set_vst3_catalog (int scan_count, const wchar_t names[][kCatalogNameLen], int name_count)
{
	_vst3_scan_count = std::max (0, scan_count);
	_vst3_catalog_count = 0;
	for (int i = 0; i < kMaxCatalog; ++i) {
		_vst3_catalog[i][0] = L'\0';
	}
	if (!names || name_count <= 0) {
		return;
	}
	const int n = std::min (name_count, kMaxCatalog);
	for (int i = 0; i < n; ++i) {
		if (names[i][0]) {
			wcsncpy_s (_vst3_catalog[i], kCatalogNameLen, names[i], _TRUNCATE);
			_vst3_catalog_count = i + 1;
		}
	}
}

void
PrefsPanel::set_vst3_loaded_name (const wchar_t* name)
{
	if (name && name[0]) {
		wcsncpy_s (_vst3_loaded_name, kCatalogNameLen, name, _TRUNCATE);
	} else {
		_vst3_loaded_name[0] = L'\0';
	}
}

void
PrefsPanel::set_status (const wchar_t* text)
{
	if (text) {
		wcsncpy_s (_status, text, _TRUNCATE);
	} else {
		_status[0] = L'\0';
	}
}

PrefsHit
PrefsPanel::hit_test (float x, float y) const
{
	if (!_visible) {
		return PrefsHit::None;
	}
	if (contains (_close_btn, x, y)) {
		return PrefsHit::Close;
	}
	if (contains (_be_dummy, x, y)) {
		return PrefsHit::BackendDummy;
	}
	if (contains (_be_wasapi, x, y)) {
		return PrefsHit::BackendWasapi;
	}
	if (contains (_per_auto, x, y)) {
		return PrefsHit::PeriodAuto;
	}
	if (contains (_per_480, x, y)) {
		return PrefsHit::Period480;
	}
	if (contains (_per_512, x, y)) {
		return PrefsHit::Period512;
	}
	if (contains (_click_btn, x, y)) {
		return PrefsHit::ClickToggle;
	}
	if (contains (_snap_btn, x, y)) {
		return PrefsHit::SnapToggle;
	}
	/* Catalog rows are handled via hit_vst3_catalog_index before PrefsHit. */
	if (contains (_vst3_rescan_btn, x, y)) {
		return PrefsHit::Vst3Rescan;
	}
	if (contains (_vst3_bypass_btn, x, y)) {
		return PrefsHit::Vst3Bypass;
	}
	if (_vst3_ok && contains (_vst3_param_reset_btn, x, y)) {
		return PrefsHit::Vst3ParamReset;
	}
	if (_vst3_ok && contains (_vst3_param_reset1_btn, x, y)) {
		return PrefsHit::Vst3ParamReset1;
	}
	if (_vst3_ok && contains (_vst3_param_reset2_btn, x, y)) {
		return PrefsHit::Vst3ParamReset2;
	}
	if (_vst3_ok && contains (_vst3_param_reset3_btn, x, y)) {
		return PrefsHit::Vst3ParamReset3;
	}
	if (_vst3_ok && contains (_vst3_unload_btn, x, y)) {
		return PrefsHit::Vst3Unload;
	}
	if (contains (_save_btn, x, y)) {
		return PrefsHit::Save;
	}
	if (contains (_card, x, y)) {
		return PrefsHit::None; /* eat clicks on card */
	}
	return PrefsHit::Close; /* click outside → close */
}

int
PrefsPanel::hit_vst3_catalog_index (float x, float y) const
{
	if (!_visible || _vst3_catalog_count <= 0) {
		return -1;
	}
	for (int i = 0; i < _vst3_catalog_count; ++i) {
		if (contains (_vst3_catalog_row[i], x, y)) {
			return i;
		}
	}
	return -1;
}

bool
PrefsPanel::catalog_row_center (int index, float* out_x, float* out_y) const
{
	if (index < 0 || index >= kMaxCatalog || !out_x || !out_y) {
		return false;
	}
	const D2D1_RECT_F& r = _vst3_catalog_row[index];
	*out_x = 0.5f * (r.left + r.right);
	*out_y = 0.5f * (r.top + r.bottom);
	return true;
}

bool
PrefsPanel::vst3_bypass_btn_center (float* out_x, float* out_y) const
{
	if (!out_x || !out_y) {
		return false;
	}
	*out_x = 0.5f * (_vst3_bypass_btn.left + _vst3_bypass_btn.right);
	*out_y = 0.5f * (_vst3_bypass_btn.top + _vst3_bypass_btn.bottom);
	return _vst3_bypass_btn.right > _vst3_bypass_btn.left;
}

bool
PrefsPanel::vst3_param_reset_btn_center (float* out_x, float* out_y) const
{
	if (!out_x || !out_y) {
		return false;
	}
	if (!_vst3_ok || _vst3_param_reset_btn.right <= _vst3_param_reset_btn.left) {
		return false;
	}
	*out_x = 0.5f * (_vst3_param_reset_btn.left + _vst3_param_reset_btn.right);
	*out_y = 0.5f * (_vst3_param_reset_btn.top + _vst3_param_reset_btn.bottom);
	return true;
}

bool
PrefsPanel::vst3_param_reset1_btn_center (float* out_x, float* out_y) const
{
	if (!out_x || !out_y) {
		return false;
	}
	if (!_vst3_ok || _vst3_param_reset1_btn.right <= _vst3_param_reset1_btn.left) {
		return false;
	}
	*out_x = 0.5f * (_vst3_param_reset1_btn.left + _vst3_param_reset1_btn.right);
	*out_y = 0.5f * (_vst3_param_reset1_btn.top + _vst3_param_reset1_btn.bottom);
	return true;
}

bool
PrefsPanel::vst3_param_reset2_btn_center (float* out_x, float* out_y) const
{
	if (!out_x || !out_y) {
		return false;
	}
	if (!_vst3_ok || _vst3_param_reset2_btn.right <= _vst3_param_reset2_btn.left) {
		return false;
	}
	*out_x = 0.5f * (_vst3_param_reset2_btn.left + _vst3_param_reset2_btn.right);
	*out_y = 0.5f * (_vst3_param_reset2_btn.top + _vst3_param_reset2_btn.bottom);
	return true;
}

bool
PrefsPanel::vst3_param_reset3_btn_center (float* out_x, float* out_y) const
{
	if (!out_x || !out_y) {
		return false;
	}
	if (!_vst3_ok || _vst3_param_reset3_btn.right <= _vst3_param_reset3_btn.left) {
		return false;
	}
	*out_x = 0.5f * (_vst3_param_reset3_btn.left + _vst3_param_reset3_btn.right);
	*out_y = 0.5f * (_vst3_param_reset3_btn.top + _vst3_param_reset3_btn.bottom);
	return true;
}

bool
PrefsPanel::vst3_unload_btn_center (float* out_x, float* out_y) const
{
	if (!out_x || !out_y) {
		return false;
	}
	if (!_vst3_ok || _vst3_unload_btn.right <= _vst3_unload_btn.left) {
		return false;
	}
	*out_x = 0.5f * (_vst3_unload_btn.left + _vst3_unload_btn.right);
	*out_y = 0.5f * (_vst3_unload_btn.top + _vst3_unload_btn.bottom);
	return true;
}

int
PrefsPanel::vst3_loaded_catalog_index () const
{
	if (_vst3_loaded_name[0] == L'\0' || _vst3_catalog_count <= 0) {
		return -1;
	}
	for (int i = 0; i < _vst3_catalog_count; ++i) {
		if (_wcsicmp (_vst3_catalog[i], _vst3_loaded_name) == 0) {
			return i;
		}
	}
	return -1;
}

void
PrefsPanel::draw (ID2D1RenderTarget* rt,
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

	pane->SetOpacity (0.78f);
	D2D1_SIZE_F sz = rt->GetSize ();
	rt->FillRectangle (D2D1::RectF (0, 0, sz.width, sz.height), pane);
	pane->SetOpacity (1.f);

	rt->FillRectangle (_card, pane);
	rt->DrawRectangle (_card, accent, 1.5f * dpi_scale);

	DWRITE_TEXT_ALIGNMENT ta = fmt_title->GetTextAlignment ();
	fmt_title->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_LEADING);
	rt->DrawTextW (L"Airdour — Preferences", 22, fmt_title,
	               D2D1::RectF (_card.left + 16.f * dpi_scale, _card.top + 8.f * dpi_scale,
	                            _close_btn.left - 8.f * dpi_scale, _card.top + 34.f * dpi_scale),
	               brand);
	fmt_title->SetTextAlignment (ta);

	auto draw_btn = [&] (const D2D1_RECT_F& r, const wchar_t* label, bool on) {
		rt->FillRectangle (r, on ? accent : edge);
		DWRITE_TEXT_ALIGNMENT t = fmt_meta->GetTextAlignment ();
		DWRITE_PARAGRAPH_ALIGNMENT p = fmt_meta->GetParagraphAlignment ();
		fmt_meta->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_CENTER);
		fmt_meta->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		rt->DrawTextW (label, (UINT32)wcslen (label), fmt_meta, r, brand);
		fmt_meta->SetTextAlignment (t);
		fmt_meta->SetParagraphAlignment (p);
	};

	draw_btn (_close_btn, L"Close", false);

	wchar_t line[160];
	float label_y = _card.top + 36.f * dpi_scale;
	_snwprintf_s (line, _TRUNCATE, L"Preferred backend  (now: %s)", _cur_backend);
	rt->DrawTextW (line, (UINT32)wcslen (line), fmt_meta,
	               D2D1::RectF (_card.left + 16.f * dpi_scale, label_y,
	                            _card.right - 16.f * dpi_scale, label_y + 16.f * dpi_scale),
	               body);

	bool be_dummy = (_wcsicmp (_pref_backend, L"WASAPI") != 0);
	draw_btn (_be_dummy, L"Dummy", be_dummy);
	draw_btn (_be_wasapi, L"WASAPI", !be_dummy);

	float ms = (_cur_sr > 0 && _cur_period > 0)
		? (1000.f * (float)_cur_period / (float)_cur_sr) : 0.f;
	_snwprintf_s (line, _TRUNCATE,
	              L"Preferred period  (now: %u fr · %.2f ms @ %u Hz)",
	              (unsigned)_cur_period, ms, (unsigned)_cur_sr);
	rt->DrawTextW (line, (UINT32)wcslen (line), fmt_meta,
	               D2D1::RectF (_card.left + 16.f * dpi_scale, _be_dummy.bottom + 8.f * dpi_scale,
	                            _card.right - 16.f * dpi_scale, _per_auto.top - 4.f * dpi_scale),
	               body);

	draw_btn (_per_auto, L"Auto", _pref_period == 0);
	draw_btn (_per_480, L"480", _pref_period == 480);
	draw_btn (_per_512, L"512", _pref_period == 512);

	_snwprintf_s (line, _TRUNCATE, L"Defaults (persist)");
	rt->DrawTextW (line, (UINT32)wcslen (line), fmt_meta,
	               D2D1::RectF (_card.left + 16.f * dpi_scale, _per_auto.bottom + 8.f * dpi_scale,
	                            _card.right - 16.f * dpi_scale, _click_btn.top - 4.f * dpi_scale),
	               body);

	draw_btn (_click_btn, _click_on ? L"Click ON" : L"Click OFF", _click_on);
	draw_btn (_snap_btn, _snap_on ? L"Snap ON" : L"Snap OFF", _snap_on);

	_snwprintf_s (line, _TRUNCATE, L"VST3 path hint · scanned %d", _vst3_scan_count);
	rt->DrawTextW (line, (UINT32)wcslen (line), fmt_meta,
	               D2D1::RectF (_card.left + 16.f * dpi_scale, _click_btn.bottom + 10.f * dpi_scale,
	                            _card.right - 16.f * dpi_scale, _click_btn.bottom + 26.f * dpi_scale),
	               body);
	rt->DrawTextW (_vst3_hint[0] ? _vst3_hint : L"(VST3_PATH unset · config @default@)",
	               (UINT32)wcslen (_vst3_hint[0] ? _vst3_hint : L"(VST3_PATH unset · config @default@)"),
	               fmt_meta,
	               D2D1::RectF (_card.left + 16.f * dpi_scale, _click_btn.bottom + 26.f * dpi_scale,
	                            _card.right - 16.f * dpi_scale, _click_btn.bottom + 42.f * dpi_scale),
	               brand);
	if (_vst3_catalog_count <= 0) {
		rt->DrawTextW (L"(none scanned)", 14, fmt_meta, _vst3_catalog_row[0], body);
	} else {
		const int loaded_i = vst3_loaded_catalog_index ();
		for (int i = 0; i < _vst3_catalog_count; ++i) {
			if (i == loaded_i && accent) {
				accent->SetOpacity (0.28f);
				rt->FillRectangle (_vst3_catalog_row[i], accent);
				accent->SetOpacity (1.f);
			}
			_snwprintf_s (line, _TRUNCATE, L"· %s%s", _vst3_catalog[i],
			              (i == loaded_i) ? L"  ←" : L"");
			rt->DrawTextW (line, (UINT32)wcslen (line), fmt_meta, _vst3_catalog_row[i], brand);
		}
	}
	draw_btn (_vst3_rescan_btn, L"Rescan VST3", false);
	draw_btn (_vst3_bypass_btn,
	          _vst3_bypassed ? L"Bypassed" : L"Bypass",
	          _vst3_ok && _vst3_bypassed);
	if (_vst3_ok && _vst3_param_reset_btn.right > _vst3_param_reset_btn.left) {
		draw_btn (_vst3_param_reset_btn, L"Reset", false);
	}
	if (_vst3_ok && _vst3_param_reset1_btn.right > _vst3_param_reset1_btn.left) {
		draw_btn (_vst3_param_reset1_btn, L"Reset-1", false);
	}
	if (_vst3_ok && _vst3_param_reset2_btn.right > _vst3_param_reset2_btn.left) {
		draw_btn (_vst3_param_reset2_btn, L"Reset-2", false);
	}
	if (_vst3_ok && _vst3_param_reset3_btn.right > _vst3_param_reset3_btn.left) {
		draw_btn (_vst3_param_reset3_btn, L"Reset-3", false);
	}
	if (_vst3_ok && _vst3_unload_btn.right > _vst3_unload_btn.left) {
		draw_btn (_vst3_unload_btn, L"Unload", false);
	}

	draw_btn (_save_btn, L"Save", true);
	if (_status[0]) {
		rt->DrawTextW (_status, (UINT32)wcslen (_status), fmt_meta,
		               D2D1::RectF (_save_btn.right + 12.f * dpi_scale, _save_btn.top,
		                            _card.right - 16.f * dpi_scale, _save_btn.bottom),
		               body);
	}
}
