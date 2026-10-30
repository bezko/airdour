/*
 * Copyright (C) 2026 Airdour contributors
 *
 * D2D Preferences overlay: preferred backend/period, click + snap defaults,
 * VST3 path hint. Persist via RC Config + WinShellPrefs instant.xml.
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

enum class PrefsHit {
	None,
	Close,
	BackendDummy,
	BackendWasapi,
	PeriodAuto,
	Period480,
	Period512,
	ClickToggle,
	SnapToggle,
	Vst3Rescan,
	Vst3Bypass,
	Save
};

class PrefsPanel
{
public:
	static constexpr int kHintLen = 96;
	static constexpr int kMaxCatalog = 8;
	static constexpr int kCatalogNameLen = 40;

	PrefsPanel ();

	void layout (float view_w, float view_h, float dpi_scale);

	void set_visible (bool on) { _visible = on; }
	bool visible () const { return _visible; }

	void set_current_backend (const wchar_t* name);
	void set_pref_backend (const wchar_t* name); /* "Dummy" or "WASAPI" */
	void set_current_period (uint32_t frames, uint32_t sample_rate);
	void set_pref_period (uint32_t frames);
	void set_click_default (bool on) { _click_on = on; }
	void set_snap_default (bool on) { _snap_on = on; }
	void set_vst3_hint (const wchar_t* text);
	/** scan_count = full list size; names[0..name_count) shown under hint. */
	void set_vst3_catalog (int scan_count, const wchar_t names[][kCatalogNameLen], int name_count);
	/** Live insert name for accent highlight (empty → no highlight). */
	void set_vst3_loaded_name (const wchar_t* name);
	void set_vst3_bypassed (bool on) { _vst3_bypassed = on; }
	void set_vst3_ok (bool on) { _vst3_ok = on; }
	void set_status (const wchar_t* text);

	PrefsHit hit_test (float x, float y) const;
	/** Center of Bypass button after layout (for smoke). */
	bool vst3_bypass_btn_center (float* out_x, float* out_y) const;
	/** Catalog name row under (x,y), or −1. Rows laid out even when empty. */
	int hit_vst3_catalog_index (float x, float y) const;
	/** Geometric center of catalog row `index` (0..kMaxCatalog-1) after layout. */
	bool catalog_row_center (int index, float* out_x, float* out_y) const;
	/** Catalog row matching loaded name, or −1. */
	int vst3_loaded_catalog_index () const;

	void draw (ID2D1RenderTarget* rt,
	           ID2D1SolidColorBrush* brand,
	           ID2D1SolidColorBrush* body,
	           ID2D1SolidColorBrush* accent,
	           ID2D1SolidColorBrush* pane,
	           ID2D1SolidColorBrush* edge,
	           IDWriteTextFormat* fmt_title,
	           IDWriteTextFormat* fmt_meta,
	           float dpi_scale) const;

private:
	D2D1_RECT_F _card;
	D2D1_RECT_F _close_btn;
	D2D1_RECT_F _be_dummy;
	D2D1_RECT_F _be_wasapi;
	D2D1_RECT_F _per_auto;
	D2D1_RECT_F _per_480;
	D2D1_RECT_F _per_512;
	D2D1_RECT_F _click_btn;
	D2D1_RECT_F _snap_btn;
	D2D1_RECT_F _vst3_rescan_btn;
	D2D1_RECT_F _vst3_bypass_btn;
	D2D1_RECT_F _vst3_catalog_row[kMaxCatalog];
	D2D1_RECT_F _save_btn;
	bool        _visible;
	bool        _click_on;
	bool        _snap_on;
	bool        _vst3_ok;
	bool        _vst3_bypassed;
	uint32_t    _cur_period;
	uint32_t    _cur_sr;
	uint32_t    _pref_period;
	wchar_t     _cur_backend[32];
	wchar_t     _pref_backend[32];
	wchar_t     _vst3_hint[kHintLen];
	int         _vst3_scan_count;
	int         _vst3_catalog_count;
	wchar_t     _vst3_catalog[kMaxCatalog][kCatalogNameLen];
	wchar_t     _vst3_loaded_name[kCatalogNameLen];
	wchar_t     _status[80];
};
