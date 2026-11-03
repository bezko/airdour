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

/* Keep the head of a long device name; endpoint suffixes are similar. */
const wchar_t*
truncate_name (const wchar_t* src, wchar_t* buf, size_t cap, size_t max_chars)
{
	size_t n = src ? wcslen (src) : 0;
	if (n <= max_chars || cap < max_chars + 2) {
		return src;
	}
	wcsncpy_s (buf, cap, src, max_chars);
	buf[max_chars] = L'\u2026';
	buf[max_chars + 1] = L'\0';
	return buf;
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
	, _vst3_unload_btn ()
	, _out_prev ()
	, _out_next ()
	, _in_prev ()
	, _in_next ()
	, _save_btn ()
	, _restart_btn ()
	, _visible (false)
	, _click_on (false)
	, _snap_on (false)
	, _vst3_ok (false)
	, _vst3_param_count (0)
	, _vst3_bypassed (false)
	, _separate_io (false)
	, _cur_period (0)
	, _cur_sr (0)
	, _pref_period (0)
	, _sys_in (0)
	, _sys_out (0)
{
	wcsncpy_s (_cur_backend, L"Dummy", _TRUNCATE);
	wcsncpy_s (_pref_backend, L"Dummy", _TRUNCATE);
	wcsncpy_s (_cur_in, L"Default", _TRUNCATE);
	wcsncpy_s (_cur_out, L"Default", _TRUNCATE);
	wcsncpy_s (_pref_in, L"Default", _TRUNCATE);
	wcsncpy_s (_pref_out, L"Default", _TRUNCATE);
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
	float card_w = std::min (view_w - pad * 2.f, 760.f * dpi_scale);
	float card_h = std::min (view_h - pad * 2.f, 640.f * dpi_scale);
	float cx = (view_w - card_w) * 0.5f;
	float cy = (view_h - card_h) * 0.5f;
	_card = D2D1::RectF (cx, cy, cx + card_w, cy + card_h);

	float x = _card.left + 16.f * dpi_scale;
	float y = _card.top + 58.f * dpi_scale; /* clears title + header divider */
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
	y = _snap_btn.bottom + 28.f * dpi_scale;

	const float arrow = 28.f * dpi_scale;
	const float right = _card.right - 16.f * dpi_scale;
	_out_prev = D2D1::RectF (x, y, x + arrow, y + btn_h);
	_out_next = D2D1::RectF (right - arrow, y, right, y + btn_h);
	y = _out_prev.bottom + 30.f * dpi_scale;

	_in_prev = D2D1::RectF (x, y, x + arrow, y + btn_h);
	_in_next = D2D1::RectF (right - arrow, y, right, y + btn_h);

	/* Path hint + catalog rows sit below device rows. */
	float cat_y = _in_prev.bottom + 52.f * dpi_scale;
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
	{
		float cursor = _vst3_bypass_btn.right;
		int n = 0;
		if (_vst3_ok) {
			n = _vst3_param_count;
			if (n < 0) {
				n = 0;
			}
			if (n > EngineSnapshot::kMaxVst3Params) {
				n = EngineSnapshot::kMaxVst3Params;
			}
		}
		float rw = 56.f * dpi_scale;
		for (int i = 0; i < EngineSnapshot::kMaxVst3Params; ++i) {
			if (i < n) {
				_vst3_param_reset_btn[i] = D2D1::RectF (cursor + gap, y,
				                                        cursor + gap + rw,
				                                        y + btn_h);
				cursor = _vst3_param_reset_btn[i].right;
			} else {
				_vst3_param_reset_btn[i] = D2D1::RectF (0.f, 0.f, 0.f, 0.f);
			}
		}
		if (_vst3_ok) {
			_vst3_unload_btn = D2D1::RectF (cursor + gap, y,
			                                cursor + gap + 64.f * dpi_scale,
			                                y + btn_h);
		} else {
			_vst3_unload_btn = D2D1::RectF (0.f, 0.f, 0.f, 0.f);
		}
	}

	_save_btn = D2D1::RectF (x, _card.bottom - 40.f * dpi_scale,
	                         x + 100.f * dpi_scale, _card.bottom - 14.f * dpi_scale);
	_restart_btn = D2D1::RectF (_save_btn.right + 10.f * dpi_scale, _save_btn.top,
	                            _save_btn.right + 140.f * dpi_scale, _save_btn.bottom);
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
PrefsPanel::set_current_devices (const wchar_t* in, const wchar_t* out)
{
	wcsncpy_s (_cur_in, (in && in[0]) ? in : L"Default", _TRUNCATE);
	wcsncpy_s (_cur_out, (out && out[0]) ? out : L"Default", _TRUNCATE);
}

void
PrefsPanel::set_pref_devices (const wchar_t* in, const wchar_t* out)
{
	wcsncpy_s (_pref_in, (in && in[0]) ? in : L"Default", _TRUNCATE);
	wcsncpy_s (_pref_out, (out && out[0]) ? out : L"Default", _TRUNCATE);
}

void
PrefsPanel::set_systemic_latency (uint32_t in_frames, uint32_t out_frames)
{
	_sys_in = in_frames;
	_sys_out = out_frames;
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
	if (contains (_out_prev, x, y)) {
		return PrefsHit::OutDevicePrev;
	}
	if (contains (_out_next, x, y)) {
		return PrefsHit::OutDeviceNext;
	}
	if (_separate_io && contains (_in_prev, x, y)) {
		return PrefsHit::InDevicePrev;
	}
	if (_separate_io && contains (_in_next, x, y)) {
		return PrefsHit::InDeviceNext;
	}
	/* Catalog rows are handled via hit_vst3_catalog_index before PrefsHit. */
	if (contains (_vst3_rescan_btn, x, y)) {
		return PrefsHit::Vst3Rescan;
	}
	if (contains (_vst3_bypass_btn, x, y)) {
		return PrefsHit::Vst3Bypass;
	}
	if (_vst3_ok && contains (_vst3_unload_btn, x, y)) {
		return PrefsHit::Vst3Unload;
	}
	if (contains (_restart_btn, x, y)) {
		return PrefsHit::RestartAudio;
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

void
PrefsPanel::set_vst3_param_count (int n)
{
	if (n < 0) {
		n = 0;
	}
	if (n > EngineSnapshot::kMaxVst3Params) {
		n = EngineSnapshot::kMaxVst3Params;
	}
	_vst3_param_count = n;
}

int
PrefsPanel::hit_vst3_param_reset (float x, float y) const
{
	if (!_visible || !_vst3_ok) {
		return -1;
	}
	const int n = (_vst3_param_count < EngineSnapshot::kMaxVst3Params)
	                  ? _vst3_param_count
	                  : EngineSnapshot::kMaxVst3Params;
	for (int i = 0; i < n; ++i) {
		const D2D1_RECT_F& r = _vst3_param_reset_btn[i];
		if (r.right > r.left && contains (r, x, y)) {
			return i;
		}
	}
	return -1;
}

bool
PrefsPanel::vst3_param_reset_btn_center (int index, float* out_x, float* out_y) const
{
	if (!out_x || !out_y || index < 0 || index >= EngineSnapshot::kMaxVst3Params) {
		return false;
	}
	if (!_vst3_ok || index >= _vst3_param_count) {
		return false;
	}
	const D2D1_RECT_F& r = _vst3_param_reset_btn[index];
	if (r.right <= r.left) {
		return false;
	}
	*out_x = 0.5f * (r.left + r.right);
	*out_y = 0.5f * (r.top + r.bottom);
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
PrefsPanel::draw (UiDraw& ui) const
{
	if (!ui.ready () || !_visible) {
		return;
	}

	const float s = ui.dpi_scale ();
	const D2D1_SIZE_F sz = ui.rt ()->GetSize ();
	ui.modal_scrim (D2D1::RectF (0.f, 0.f, sz.width, sz.height), 0.62f);

	const float radius = ui::theme ().metrics.radius_lg;
	ui.shadow (_card, radius + 2.f);
	ui.fill_round (_card, radius, ui::ColorToken::Card);
	ui.stroke_round (_card, radius, ui::ColorToken::BorderStrong, 1.f);

	/* Accent tab + title. */
	const float inset = 18.f * s;
	ui.fill_round (D2D1::RectF (_card.left + inset, _card.top + 10.f * s,
	                             _card.left + inset + 3.f * s, _card.top + 30.f * s),
	                1.5f, ui::ColorToken::Accent);
	ui.text_vcenter (L"Preferences", ui::TextRole::Title, ui::ColorToken::TextPrimary,
	                 D2D1::RectF (_card.left + inset + 12.f * s, _card.top,
	                              _close_btn.left - 8.f * s, _card.top + 40.f * s));
	ui.divider (D2D1::RectF (_card.left + inset, _card.top + 42.f * s,
	                          _card.right - inset, _card.top + 42.f * s + std::max (1.f, s)));

	auto draw_btn = [&] (const D2D1_RECT_F& r, const wchar_t* label, bool on) {
		UiDraw::ButtonStyle st;
		st.label = label;
		st.latched = on;
		ui.button (r, st);
	};

	auto draw_label = [&] (float y, const wchar_t* text) {
		ui.text_vcenter (text, ui::TextRole::Caption, ui::ColorToken::TextMuted,
		                 D2D1::RectF (_card.left + inset, y - 14.f * s,
		                              _card.right - inset, y - 2.f * s));
	};

	auto draw_name = [&] (const D2D1_RECT_F& prev, const D2D1_RECT_F& next, const wchar_t* text) {
		const D2D1_RECT_F r = D2D1::RectF (prev.right + 6.f * s, prev.top,
		                                   next.left - 6.f * s, prev.bottom);
		ui.fill_round (r, 5.f, ui::ColorToken::Control, 0.7f);
		ui.stroke_round (r, 5.f, ui::ColorToken::Border, 1.f, 0.8f);
		wchar_t shortbuf[64];
		const wchar_t* label = truncate_name (text, shortbuf, 64, 52);
		ui.text_vcenter (label, ui::TextRole::Body, ui::ColorToken::TextPrimary, r,
		                 DWRITE_TEXT_ALIGNMENT_CENTER);
	};

	UiDraw::ButtonStyle close_st;
	close_st.label = L"Close";
	ui.button (_close_btn, close_st);

	wchar_t line[200];
	_snwprintf_s (line, _TRUNCATE, L"Preferred backend  (now: %s)", _cur_backend);
	draw_label (_be_dummy.top - 8.f * s, line);

	bool be_dummy = (_wcsicmp (_pref_backend, L"WASAPI") != 0);
	draw_btn (_be_dummy, L"Dummy", be_dummy);
	draw_btn (_be_wasapi, L"WASAPI", !be_dummy);

	float ms = (_cur_sr > 0 && _cur_period > 0)
	           ? (1000.f * (float)_cur_period / (float)_cur_sr) : 0.f;
	_snwprintf_s (line, _TRUNCATE,
	              L"Preferred period  (now: %u fr · %.2f ms @ %u Hz)",
	              (unsigned)_cur_period, ms, (unsigned)_cur_sr);
	draw_label (_per_auto.top - 8.f * s, line);

	draw_btn (_per_auto, L"Auto", _pref_period == 0);
	draw_btn (_per_480, L"480", _pref_period == 480);
	draw_btn (_per_512, L"512", _pref_period == 512);

	draw_label (_click_btn.top - 8.f * s, L"Defaults (persist)");

	draw_btn (_click_btn, _click_on ? L"Click ON" : L"Click OFF", _click_on);
	draw_btn (_snap_btn, _snap_on ? L"Snap ON" : L"Snap OFF", _snap_on);

	_snwprintf_s (line, _TRUNCATE, L"Output device  (now: %s)", _cur_out);
	draw_label (_out_prev.top - 8.f * s, line);
	draw_btn (_out_prev, L"<", false);
	draw_btn (_out_next, L">", false);
	draw_name (_out_prev, _out_next, _pref_out);

	_snwprintf_s (line, _TRUNCATE, L"Input device  (now: %s)",
	              _separate_io ? _cur_in : L"shared with output");
	draw_label (_in_prev.top - 8.f * s, line);
	draw_btn (_in_prev, L"<", false);
	draw_btn (_in_next, L">", false);
	draw_name (_in_prev, _in_next, _separate_io ? _pref_in : L"Default");

	float ms_in = (_cur_sr > 0) ? (1000.f * (float)_sys_in / (float)_cur_sr) : 0.f;
	float ms_out = (_cur_sr > 0) ? (1000.f * (float)_sys_out / (float)_cur_sr) : 0.f;
	_snwprintf_s (line, _TRUNCATE,
	              L"Systemic latency  out %.2f ms · in %.2f ms  (%u / %u fr)",
	              ms_out, ms_in, (unsigned)_sys_out, (unsigned)_sys_in);
	ui.text_vcenter (line, ui::TextRole::Caption, ui::ColorToken::TextSecondary,
	                 D2D1::RectF (_card.left + inset, _in_prev.bottom + 8.f * s,
	                              _card.right - inset, _in_prev.bottom + 24.f * s));

	_snwprintf_s (line, _TRUNCATE, L"VST3 path hint · scanned %d", _vst3_scan_count);
	ui.text_vcenter (line, ui::TextRole::Caption, ui::ColorToken::TextMuted,
	                 D2D1::RectF (_card.left + inset, _vst3_catalog_row[0].top - 30.f * s,
	                              _card.right - inset, _vst3_catalog_row[0].top - 16.f * s));
	ui.text_vcenter (_vst3_hint[0] ? _vst3_hint : L"(VST3_PATH unset · config @default@)",
	                 ui::TextRole::Caption, ui::ColorToken::TextSecondary,
	                 D2D1::RectF (_card.left + inset, _vst3_catalog_row[0].top - 16.f * s,
	                              _card.right - inset, _vst3_catalog_row[0].top - 2.f * s));
	if (_vst3_catalog_count <= 0) {
		ui.text_vcenter (L"(none scanned)", ui::TextRole::Caption,
		                 ui::ColorToken::TextMuted, _vst3_catalog_row[0]);
	} else {
		const int loaded_i = vst3_loaded_catalog_index ();
		for (int i = 0; i < _vst3_catalog_count; ++i) {
			if (i == loaded_i) {
				ui.fill_round (_vst3_catalog_row[i], 3.f, ui::ColorToken::Accent, 0.28f);
			}
			_snwprintf_s (line, _TRUNCATE, L"· %s%s", _vst3_catalog[i],
			              (i == loaded_i) ? L"  ←" : L"");
			ui.text_vcenter (line, ui::TextRole::Caption, ui::ColorToken::TextPrimary,
			                 _vst3_catalog_row[i]);
		}
	}
	draw_btn (_vst3_rescan_btn, L"Rescan VST3", false);
	draw_btn (_vst3_bypass_btn,
	          _vst3_bypassed ? L"Bypassed" : L"Bypass",
	          _vst3_ok && _vst3_bypassed);
	for (int i = 0; i < EngineSnapshot::kMaxVst3Params; ++i) {
		const D2D1_RECT_F& r = _vst3_param_reset_btn[i];
		if (_vst3_ok && r.right > r.left) {
			wchar_t lab[16];
			if (i == 0) {
				draw_btn (r, L"Reset", false);
			} else {
				_snwprintf_s (lab, _TRUNCATE, L"Reset-%d", i);
				draw_btn (r, lab, false);
			}
		}
	}
	if (_vst3_ok && _vst3_unload_btn.right > _vst3_unload_btn.left) {
		draw_btn (_vst3_unload_btn, L"Unload", false);
	}

	draw_btn (_save_btn, L"Save", true);
	draw_btn (_restart_btn, L"Restart audio", false);
	if (_status[0]) {
		ui.text_vcenter (_status, ui::TextRole::Caption, ui::ColorToken::Accent,
		                 D2D1::RectF (_restart_btn.right + 12.f * s, _save_btn.top,
		                              _card.right - inset, _save_btn.bottom));
	}
}
