/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Direct2D mixer track strip: arm + mute/solo + pan + send + FX + fader + meter (not GTK).
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

#include "level_meter.h"

class MixerStrip
{
public:
	MixerStrip ();

	void set_label (const wchar_t* label);
	/** Optional processor name shown under the track label (e.g. ACE Reverb). */
	void set_fx_label (const wchar_t* label);
	void set_fx_bypassed (bool bypassed);
	void set_fx_param (float value_0_1);
	void layout (const D2D1_RECT_F& strip_bounds, float dpi_scale);

	void  set_gain (float g);
	float gain () const { return _gain; }

	void set_armed (bool armed) { _armed = armed; }
	bool armed () const { return _armed; }

	void set_muted (bool muted) { _muted = muted; }
	bool muted () const { return _muted; }
	void set_soloed (bool soloed) { _soloed = soloed; }
	bool soloed () const { return _soloed; }

	/** Pan azimuth 0=L … 0.5=C … 1=R (audio strips only). */
	void  set_pan (float pan_0_1);
	float pan () const { return _pan; }
	bool  hit_pan (float x, float y) const;
	bool  drag_pan_to (float x);

	/** Aux send level 0..1 (gain coeff) to session Aux bus (audio strips only). */
	void  set_send (float send_0_1);
	float send () const { return _send; }
	bool  hit_send (float x, float y) const;
	bool  drag_send_to (float x);

	/** MIDI strip: hide rec-arm / FX / pan / send chrome; keep M/S + fader + meter. */
	void set_midi_strip (bool midi) { _midi_strip = midi; }
	bool is_midi_strip () const { return _midi_strip; }

	/** True when strip has a non-empty FX label (insert/bypass is live). */
	bool has_fx () const { return _fx_label[0] != L'\0'; }
	bool fx_bypassed () const { return _fx_bypassed; }
	float fx_param () const { return _fx_param; }
	bool hit_fx (float x, float y) const;
	bool hit_fx_param (float x, float y) const;
	/** Map x into Blend 0..1; returns true if value changed. */
	bool drag_fx_param_to (float x);

	void        set_meter_peak (float peak, bool clipped = false);
	void        meter_tick (float dt);
	void        clear_meter_clip ();
	bool        meter_clipped () const { return _meter.clipped (); }
	bool        hit_meter (float x, float y) const;
	LevelMeter& meter () { return _meter; }

	bool hit_fader (float x, float y) const;
	bool hit_label (float x, float y) const;
	bool hit_arm (float x, float y) const;
	bool hit_mute (float x, float y) const;
	bool hit_solo (float x, float y) const;
	bool drag_to (float y);

	void draw (ID2D1RenderTarget* rt,
	           ID2D1SolidColorBrush* brand,
	           ID2D1SolidColorBrush* body,
	           ID2D1SolidColorBrush* accent,
	           ID2D1SolidColorBrush* pane,
	           ID2D1SolidColorBrush* edge,
	           ID2D1SolidColorBrush* meter_hot,
	           IDWriteTextFormat* fmt_title,
	           IDWriteTextFormat* fmt_meta,
	           float dpi_scale) const;

	const D2D1_RECT_F& bounds () const { return _strip; }

private:
	wchar_t     _label[32];
	wchar_t     _fx_label[40];
	D2D1_RECT_F _strip;
	D2D1_RECT_F _label_rc;
	D2D1_RECT_F _track;
	D2D1_RECT_F _arm_btn;
	D2D1_RECT_F _mute_btn;
	D2D1_RECT_F _solo_btn;
	D2D1_RECT_F _pan_track;
	D2D1_RECT_F _send_track;
	D2D1_RECT_F _fx_btn;
	D2D1_RECT_F _fx_param_track;
	LevelMeter  _meter;
	float       _gain;
	float       _pan;
	float       _send;
	float       _fx_param;
	bool        _armed;
	bool        _muted;
	bool        _soloed;
	bool        _fx_bypassed;
	bool        _midi_strip;
};
