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

#include <cstdint>

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include "level_meter.h"

class MixerStrip
{
public:
	MixerStrip ();

	void set_label (const wchar_t* label);
	/** PresentationInfo RGBA (RRGGBBAA); 0 = use accent for left bar. */
	void set_color (uint32_t rgba_rrggbbaa);
	uint32_t color () const { return _color_rgba; }
	/** Optional processor name shown under the track label (e.g. ACE Reverb). */
	void set_fx_label (const wchar_t* label);
	void set_fx_bypassed (bool bypassed);
	void set_fx_param (float value_0_1);
	/** FX param track count: 1=Reverb, 2=Comp, 3=EQ. Extra tracks reserved in layout. */
	void set_fx_param_count (int count_1_to_3);
	void set_fx_dual (bool dual); /* legacy: dual → count 2 */
	void set_fx_param2 (float value_0_1);
	void set_fx_param3 (float value_0_1);
	void layout (const D2D1_RECT_F& strip_bounds, float dpi_scale);

	void  set_gain (float g);
	float gain () const { return _gain; }

	void set_armed (bool armed) { _armed = armed; }
	bool armed () const { return _armed; }
	/** Record-safe latch (Rs badge on Arm). */
	void set_record_safe (bool safe) { _record_safe = safe; }
	bool record_safe () const { return _record_safe; }

	/** 0=Auto 1=Input 2=Disk — Mon button (audio strips). */
	void set_monitor_choice (int choice_0_1_2);
	int  monitor_choice () const { return _monitor_choice; }
	/** Group monitoring peers sharing this choice (badge shows total ·N). */
	void set_monitor_peer_count (int peers) { _monitor_peer_count = peers < 0 ? 0 : peers; }
	int  monitor_peer_count () const { return _monitor_peer_count; }

	void set_muted (bool muted) { _muted = muted; }
	bool muted () const { return _muted; }
	/** 0=Main (Mn) 1=AllPoints (All) — mute points badge. */
	void set_mute_points (int points_0_1) { _mute_points = (points_0_1 != 0) ? 1 : 0; }
	int  mute_points () const { return _mute_points; }
	void set_soloed (bool soloed) { _soloed = soloed; }
	bool soloed () const { return _soloed; }
	void set_solo_isolated (bool isolated) { _solo_isolated = isolated; }
	bool solo_isolated () const { return _solo_isolated; }
	void set_solo_safe (bool safe) { _solo_safe = safe; }
	bool solo_safe () const { return _solo_safe; }
	void set_has_comment (bool has) { _has_comment = has; }
	bool has_comment () const { return _has_comment; }
	void set_phase_inverted (bool inverted) { _phase_inverted = inverted; }
	bool phase_inverted () const { return _phase_inverted; }

	/** Preamp trim UI 0..1 (0.5 = unity / 0 dB; audio strips only). */
	void  set_trim (float trim_0_1);
	float trim () const { return _trim; }
	bool  hit_trim (float x, float y) const;
	bool  drag_trim_to (float x);
	/** 0=Solo (S), 1=PFL (P), 2=AFL (A) — RC listen mode glyph on Solo button. */
	void set_listen_mode (int mode_0_1_2) { _listen_mode = mode_0_1_2; }
	int  listen_mode () const { return _listen_mode; }
	/** Route::active — dim strip when false. */
	void set_route_active (bool active) { _route_active = active; }
	bool route_active () const { return _route_active; }

	/** Pan azimuth 0=L … 0.5=C … 1=R (audio strips only). */
	void  set_pan (float pan_0_1);
	float pan () const { return _pan; }
	bool  hit_pan (float x, float y) const;
	bool  drag_pan_to (float x);

	/**
	 * Stereo pan width −1..+1 (1=full) when pan_width_control is present.
	 * Shift+drag pan maps x as 0..1 iface → 2v−1.
	 */
	void  set_pan_width (float width_m1_to_1);
	float pan_width () const { return _pan_width; }
	void  set_pan_width_ok (bool ok) { _pan_width_ok = ok; }
	bool  pan_width_ok () const { return _pan_width_ok; }
	bool  drag_pan_width_to (float x);

	/** Aux send level 0..1 (gain coeff) to session Aux bus (audio strips only). */
	void  set_send (float send_0_1);
	float send () const { return _send; }
	void  set_send_muted (bool muted) { _send_muted = muted; }
	bool  send_muted () const { return _send_muted; }
	bool  hit_send (float x, float y) const;
	bool  drag_send_to (float x);

	/** Aux send pan azimuth 0..1 when send panner present. Shift+drag send. */
	void  set_send_pan (float pan_0_1);
	float send_pan () const { return _send_pan; }
	void  set_send_pan_ok (bool ok) { _send_pan_ok = ok; }
	bool  send_pan_ok () const { return _send_pan_ok; }
	bool  drag_send_pan_to (float x);

	/** MIDI strip: hide rec-arm / FX / pan / send chrome; keep M/S + fader + meter. */
	void set_midi_strip (bool midi) { _midi_strip = midi; }
	bool is_midi_strip () const { return _midi_strip; }

	/** Compact mixer: hide pan / send / FX (keep Arm + M/S + fader + meter). */
	void set_narrow (bool narrow) { _narrow = narrow; }
	bool narrow () const { return _narrow; }

	/** Session RouteGroup membership + Shift-pick for upcoming Grp. */
	void set_in_group (bool in_group) { _in_group = in_group; }
	bool in_group () const { return _in_group; }
	void set_group_picked (bool picked) { _group_picked = picked; }
	bool group_picked () const { return _group_picked; }
	/** Group color RRGGBBAA when RouteGroup shares color (0 = use accent). */
	void set_group_rgba (uint32_t rgba) { _group_rgba = rgba; }
	uint32_t group_rgba () const { return _group_rgba; }
	/** CoreSelection selected (group select share may light peers). */
	void set_selected (bool selected) { _selected = selected; }
	bool selected () const { return _selected; }

	/** True when strip has a non-empty FX label (insert/bypass is live). */
	bool has_fx () const { return _fx_label[0] != L'\0'; }
	bool fx_bypassed () const { return _fx_bypassed; }
	bool fx_dual () const { return _fx_param_count >= 2; }
	int  fx_param_count () const { return _fx_param_count; }
	float fx_param () const { return _fx_param; }
	float fx_param2 () const { return _fx_param2; }
	float fx_param3 () const { return _fx_param3; }
	bool hit_fx (float x, float y) const;
	/** × on FX row when set_fx_can_unload(true) — VST3 Audio 2 unload. */
	bool hit_fx_unload (float x, float y) const;
	/** B chip left of × when set_fx_can_unload(true) — VST3 Audio 2 bypass. */
	bool hit_fx_bypass (float x, float y) const;
	/** "1" chip — reset VST3 param1 (between B and ×). */
	bool hit_fx_param_reset1 (float x, float y) const;
	/** "2" chip — reset VST3 param2 (between B and ×). */
	bool hit_fx_param_reset2 (float x, float y) const;
	/** "3" chip — reset VST3 param3 (between B and ×). */
	bool hit_fx_param_reset3 (float x, float y) const;
	void set_fx_can_unload (bool on) { _fx_can_unload = on; }
	bool fx_can_unload () const { return _fx_can_unload; }
	bool hit_fx_param (float x, float y) const;
	bool hit_fx_param2 (float x, float y) const;
	bool hit_fx_param3 (float x, float y) const;
	/** Map x into param 0..1; returns true if value changed. */
	bool drag_fx_param_to (float x);
	bool drag_fx_param2_to (float x);
	bool drag_fx_param3_to (float x);

	void        set_meter_peak (float peak, bool clipped = false);
	void        meter_tick (float dt);
	void        clear_meter_clip ();
	bool        meter_clipped () const { return _meter.clipped (); }
	bool        hit_meter (float x, float y) const;
	LevelMeter& meter () { return _meter; }
	/** 0=In 1=Pre 2=Post 3=Out — badge near meter. */
	void set_meter_point (int point_0_3);
	int  meter_point () const { return _meter_point; }
	/** 0=Peak 1=K14 — badge near meter. */
	void set_meter_type (int type_0_1);
	int  meter_type () const { return _meter_type; }

	bool hit_fader (float x, float y) const;
	bool hit_label (float x, float y) const;
	bool hit_arm (float x, float y) const;
	bool hit_mon (float x, float y) const;
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
	uint32_t    _color_rgba;
	D2D1_RECT_F _strip;
	D2D1_RECT_F _label_rc;
	D2D1_RECT_F _track;
	D2D1_RECT_F _arm_btn;
	D2D1_RECT_F _mon_btn;
	D2D1_RECT_F _mute_btn;
	D2D1_RECT_F _solo_btn;
	D2D1_RECT_F _trim_track;
	D2D1_RECT_F _pan_track;
	D2D1_RECT_F _send_track;
	D2D1_RECT_F _fx_btn;
	D2D1_RECT_F _fx_bypass_btn;
	D2D1_RECT_F _fx_param_reset1_btn;
	D2D1_RECT_F _fx_param_reset2_btn;
	D2D1_RECT_F _fx_param_reset3_btn;
	D2D1_RECT_F _fx_unload_btn;
	D2D1_RECT_F _fx_param_track;
	D2D1_RECT_F _fx_param_track2;
	D2D1_RECT_F _fx_param_track3;
	LevelMeter  _meter;
	int         _meter_point; /* 0=In 1=Pre 2=Post 3=Out */
	int         _meter_type; /* 0=Peak 1=K14 */
	float       _gain;
	float       _trim;
	float       _pan;
	float       _pan_width; /* −1..+1; 1=full stereo */
	bool        _pan_width_ok;
	float       _send;
	bool        _send_muted;
	float       _send_pan;
	bool        _send_pan_ok;
	float       _fx_param;
	float       _fx_param2;
	float       _fx_param3;
	bool        _armed;
	int         _monitor_choice; /* 0=Auto 1=Input 2=Disk */
	int         _monitor_peer_count;
	bool        _record_safe;
	bool        _muted;
	int         _mute_points; /* 0=Main 1=All */
	bool        _soloed;
	bool        _solo_isolated;
	bool        _solo_safe;
	bool        _has_comment;
	bool        _phase_inverted;
	int         _listen_mode; /* 0=S 1=P 2=A */
	bool        _route_active;
	bool        _fx_bypassed;
	bool        _fx_can_unload;
	int         _fx_param_count; /* 1..3 */
	bool        _midi_strip;
	bool        _narrow;
	bool        _in_group;
	bool        _group_picked;
	uint32_t    _group_rgba;
	bool        _selected;
};
