/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Compact D2D monitor strip: Dim / Mono / Mute / Boost for Session MonitorProcessor.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

enum class MonitorHit {
	None,
	Dim,
	Mono,
	Mute,
	Boost,
	AuxReturn,
	MasterLim
};

class MonitorStrip
{
public:
	MonitorStrip ();

	void layout (const D2D1_RECT_F& bounds, float dpi_scale);

	void set_available (bool ok) { _available = ok; }
	bool available () const { return _available; }

	void set_dim (bool on) { _dim = on; }
	bool dim () const { return _dim; }
	void set_dim_db (float db) { _dim_db = db; }
	float dim_db () const { return _dim_db; }
	void set_mono (bool on) { _mono = on; }
	bool mono () const { return _mono; }
	void set_mute (bool on) { _mute = on; }
	bool mute () const { return _mute; }
	void set_solo_boost_db (float db) { _solo_boost_db = db; }
	float solo_boost_db () const { return _solo_boost_db; }

	/** Aux bus return gain 0..1 (unity=1). Shown when aux_ok. */
	void  set_aux_ok (bool ok) { _aux_ok = ok; }
	bool  aux_ok () const { return _aux_ok; }
	void  set_aux_return (float g);
	float aux_return () const { return _aux_return; }
	bool  drag_aux_return_to (float y);
	bool  hit_aux_return (float x, float y) const;

	void set_master_lim_ok (bool ok) { _master_lim_ok = ok; }
	bool master_lim_ok () const { return _master_lim_ok; }
	void set_master_lim_bypassed (bool on) { _master_lim_bypassed = on; }
	bool master_lim_bypassed () const { return _master_lim_bypassed; }

	MonitorHit hit_test (float x, float y) const;

	void draw (ID2D1RenderTarget* rt,
	           ID2D1SolidColorBrush* brand,
	           ID2D1SolidColorBrush* body,
	           ID2D1SolidColorBrush* accent,
	           ID2D1SolidColorBrush* pane,
	           ID2D1SolidColorBrush* edge,
	           IDWriteTextFormat* fmt_title,
	           IDWriteTextFormat* fmt_meta,
	           float dpi_scale) const;

	const D2D1_RECT_F& bounds () const { return _strip; }

private:
	D2D1_RECT_F _strip;
	D2D1_RECT_F _dim_btn;
	D2D1_RECT_F _mono_btn;
	D2D1_RECT_F _mute_btn;
	D2D1_RECT_F _boost_btn;
	D2D1_RECT_F _aux_track;
	D2D1_RECT_F _lim_btn;
	bool        _available;
	bool        _aux_ok;
	bool        _dim;
	float       _dim_db;
	bool        _mono;
	bool        _mute;
	float       _solo_boost_db;
	float       _aux_return;
	bool        _master_lim_ok;
	bool        _master_lim_bypassed;
};
