/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "transport_bar.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

bool
contains (const D2D1_RECT_F& r, float x, float y)
{
	return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

inline float
clampf (float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

TransportBar::TransportBar ()
	: _bounds ()
	, _clock_bounds ()
	, _dsp_bounds ()
	, _shuttle_btn ()
	, _tempo_dn_btn ()
	, _tempo_up_btn ()
	, _open_btn ()
	, _overflow_btn ()
	, _menu_bounds ()
	, _overflow_open (false)
	, _dpi_scale (1.f)
	, _hover (TransportHit::None)
	, _pressed (TransportHit::None)
	, _hover_x (0.f)
	, _hover_y (0.f)
	, _playing (false)
	, _recording (false)
	, _record_armed (false)
	, _loop_playing (false)
	, _loop_active (false)
	, _punch_enabled (false)
	, _snap_enabled (false)
	, _snap_density (1)
	, _midi_step_enabled (false)
	, _midi_chord_quality (0)
	, _midi_arp_enabled (false)
	, _midi_arp_mode (0)
	, _midi_scale_lock (false)
	, _midi_scale_kind (0)
	, _midi_scale_root (0)
	, _freeze_active (false)
	, _count_in_enabled (false)
	, _click_enabled (false)
	, _click_gain (1.f)
	, _group_active (false)
	, _group_relative (false)
	, _spectrum_enabled (false)
	, _correlation_enabled (false)
	, _loudness_enabled (false)
	, _dsp_load_pct (0.f)
	, _tempo_bpm (120.f)
	, _shuttle_norm (0.f)
	, _bbt_bars (1)
	, _bbt_beats (1)
	, _bbt_ticks (0)
	, _tc_hours (0)
	, _tc_minutes (0)
	, _tc_seconds (0)
	, _tc_frames (0)
{
}

void
TransportBar::set_snap_density (int density_0_to_3)
{
	if (density_0_to_3 < 0) {
		density_0_to_3 = 0;
	}
	if (density_0_to_3 > 3) {
		density_0_to_3 = 3;
	}
	_snap_density = density_0_to_3;
}

const wchar_t*
TransportBar::snap_btn_label () const
{
	if (!_snap_enabled) {
		return L"Snap";
	}
	static const wchar_t* labels[] = { L"Bar", L"Beat", L"1/2", L"1/4" };
	int d = _snap_density;
	if (d < 0 || d > 3) {
		d = 1;
	}
	return labels[d];
}

bool
TransportBar::snap_btn_center (float* out_x, float* out_y) const
{
	if (!out_x || !out_y) {
		return false;
	}
	for (const HitSlot& s : _hit_rects) {
		if (s.hit == TransportHit::Snap && s.rect.right > s.rect.left) {
			*out_x = 0.5f * (s.rect.left + s.rect.right);
			*out_y = 0.5f * (s.rect.top + s.rect.bottom);
			return true;
		}
	}
	return false;
}

void
TransportBar::clear_layout ()
{
	_primary.clear ();
	_rows.clear ();
	_files.clear ();
	_hit_rects.clear ();
	_menu_rows.clear ();
	_clock_bounds = D2D1::RectF ();
	_dsp_bounds = D2D1::RectF ();
	_shuttle_btn = D2D1::RectF ();
	_tempo_dn_btn = D2D1::RectF ();
	_tempo_up_btn = D2D1::RectF ();
	_open_btn = D2D1::RectF ();
	_overflow_btn = D2D1::RectF ();
	_menu_bounds = D2D1::RectF ();
}

void
TransportBar::add_hit (TransportHit hit, const D2D1_RECT_F& r)
{
	_hit_rects.push_back (HitSlot { hit, r });
}

void
TransportBar::layout (const D2D1_RECT_F& bounds, float dpi_scale,
                      float viewport_bottom)
{
	_bounds = bounds;
	_dpi_scale = (dpi_scale > 0.f) ? dpi_scale : 1.f;
	clear_layout ();

	const float dp = _dpi_scale;
	const float W = bounds.right - bounds.left;
	const float H = bounds.bottom - bounds.top;
	if (W < 2.f || H < 2.f) {
		return;
	}

	const float logical_w = W / dp;
	const float pad = 10.f * dp;
	const float top = bounds.top + 9.f * dp;
	const float bh = std::max (24.f * dp, H - 18.f * dp);
	const float gap = 5.f * dp;

	const bool show_files = logical_w >= 940.f;
	const float clock_w = (logical_w >= 1100.f ? 228.f
	                      : (logical_w >= 940.f ? 206.f : 178.f)) * dp;
	const float primary_scale = (logical_w >= 860.f ? 1.f : 0.8f);

	const float overflow_w = 38.f * dp;
	const float file_w = 4.f * 28.f * dp + 3.f * 4.f * dp;
	const float right_w = clock_w + 12.f * dp + overflow_w + 9.f * dp
	                      + (show_files ? file_w + 9.f * dp : 0.f);
	const float primary_total = 350.f * dp * primary_scale;
	const float divider_w = 17.f * dp;
	const float mid = W - 2.f * pad - primary_total - divider_w - right_w;

	const float row_gap = 4.f * dp;
	const float min_row_w = 44.f * dp;
	int per_row = 0;
	float row_w = 0.f;
	if (mid >= 3.f * min_row_w) {
		per_row = (int)((mid + row_gap) / (58.f * dp + row_gap));
		per_row = std::max (3, std::min (6, per_row));
		row_w = clampf ((mid - (per_row - 1) * row_gap) / (float)per_row,
		                min_row_w, 64.f * dp);
	}

	/* Primary cluster: Play / Stop / Record / Loop / Punch / Click / Count. */
	float x = bounds.left + pad;
	struct PrimaryDef {
		TransportHit hit;
		float width;
	};
	const PrimaryDef primary[] = {
		{ TransportHit::Play,    56.f },
		{ TransportHit::Stop,    48.f },
		{ TransportHit::Rec,     48.f },
		{ TransportHit::Loop,    48.f },
		{ TransportHit::Punch,   48.f },
		{ TransportHit::Click,   36.f },
		{ TransportHit::CountIn, 36.f },
	};
	for (const PrimaryDef& p : primary) {
		const float w = p.width * dp * primary_scale;
		const D2D1_RECT_F r = D2D1::RectF (x, top, x + w, top + bh);
		_primary.push_back (HitSlot { p.hit, r });
		add_hit (p.hit, r);
		x = r.right + gap;
	}

	/* Two compact rows of frequent edit / MIDI actions. */
	const TransportHit edit_rows[] = {
		TransportHit::Split, TransportHit::Fade, TransportHit::Mark,
		TransportHit::Snap, TransportHit::Freeze, TransportHit::Stretch,
	};
	const TransportHit midi_rows[] = {
		TransportHit::MidiStep, TransportHit::MidiChord, TransportHit::MidiArp,
		TransportHit::MidiScale, TransportHit::Quantize, TransportHit::MidiTransUp,
	};
	std::vector<TransportHit> hidden_edit;
	std::vector<TransportHit> hidden_midi;

	const float rows_x = x + 9.f * dp;
	const float row_y0 = top + 1.f * dp;
	const float row_h = (bh - row_gap) * 0.5f;

	auto place_row = [&] (const TransportHit* items, int count,
	                      std::vector<TransportHit>& hidden, float row_y) {
		for (int i = 0; i < 6; ++i) {
			if (i >= per_row || i >= count) {
				hidden.push_back (items[i]);
				continue;
			}
			const D2D1_RECT_F r = D2D1::RectF (
				rows_x + i * (row_w + row_gap), row_y,
				rows_x + i * (row_w + row_gap) + row_w, row_y + row_h);
			_rows.push_back (HitSlot { items[i], r });
			add_hit (items[i], r);
		}
	};
	if (per_row > 0) {
		place_row (edit_rows, 6, hidden_edit, row_y0);
		place_row (midi_rows, 6, hidden_midi, row_y0 + row_h + row_gap);
	} else {
		for (int i = 0; i < 6; ++i) {
			hidden_edit.push_back (edit_rows[i]);
			hidden_midi.push_back (midi_rows[i]);
		}
	}

	/* Right cluster: clock + file actions + overflow. */
	float right_x = bounds.right - pad;
	_overflow_btn = D2D1::RectF (right_x - overflow_w,
	                             top + (bh - overflow_w) * 0.5f,
	                             right_x,
	                             top + (bh + overflow_w) * 0.5f);
	add_hit (TransportHit::Overflow, _overflow_btn);
	right_x = _overflow_btn.left - 9.f * dp;

	if (show_files) {
		const float fw = 28.f * dp;
		const float fgap = 4.f * dp;
		const float cluster_w = 4.f * fw + 3.f * fgap;
		float fx = right_x - cluster_w;
		const TransportHit file_hits[] = {
			TransportHit::Import, TransportHit::Save,
			TransportHit::Open, TransportHit::Export,
		};
		for (int i = 0; i < 4; ++i) {
			const D2D1_RECT_F r = D2D1::RectF (fx + i * (fw + fgap),
			                                   top + (bh - fw) * 0.5f,
			                                   fx + i * (fw + fgap) + fw,
			                                   top + (bh + fw) * 0.5f);
			_files.push_back (HitSlot { file_hits[i], r });
			add_hit (file_hits[i], r);
			if (file_hits[i] == TransportHit::Open) {
				_open_btn = r;
			}
		}
		right_x -= cluster_w + 9.f * dp;
	} else {
		_open_btn = _overflow_btn;
	}

	_clock_bounds = D2D1::RectF (right_x - clock_w, top, right_x, top + bh);

	const float ccx0 = _clock_bounds.left;
	const float ccy0 = _clock_bounds.top;
	const float ccx1 = _clock_bounds.right;
	const float ccy1 = _clock_bounds.bottom;
	_tempo_up_btn = D2D1::RectF (ccx1 - 19.f * dp, ccy0 + 4.f * dp,
	                             ccx1 - 3.f * dp, ccy0 + 22.f * dp);
	_tempo_dn_btn = D2D1::RectF (ccx1 - 39.f * dp, ccy0 + 4.f * dp,
	                             ccx1 - 23.f * dp, ccy0 + 22.f * dp);
	add_hit (TransportHit::TempoDown, _tempo_dn_btn);
	add_hit (TransportHit::TempoUp, _tempo_up_btn);

	_shuttle_btn = D2D1::RectF (ccx0 + 8.f * dp, ccy1 - 15.f * dp,
	                            ccx1 - 8.f * dp, ccy1 - 5.f * dp);
	add_hit (TransportHit::Shuttle, _shuttle_btn);

	/* Overflow groups: secondary actions plus any rows that did not fit. */
	std::vector<MenuGroup> groups;
	{
		MenuGroup g;
		g.group = Group::Edit;
		g.title = L"EDIT";
		g.items = {
			TransportHit::PitchUp, TransportHit::PitchDown,
			TransportHit::Reverse, TransportHit::Normalize,
			TransportHit::StripSilence, TransportHit::Consolidate,
		};
		g.items.insert (g.items.end (), hidden_edit.begin (), hidden_edit.end ());
		groups.push_back (g);
	}
	{
		MenuGroup g;
		g.group = Group::Midi;
		g.title = L"MIDI";
		g.items = {
			TransportHit::MidiTransDown,
			TransportHit::MidiVelUp, TransportHit::MidiVelDown,
			TransportHit::MidiLenUp, TransportHit::MidiLenDown,
			TransportHit::MidiHumanize, TransportHit::MidiPanic,
			TransportHit::MidiPgmUp, TransportHit::MidiPgmDown,
			TransportHit::MidiBankUp, TransportHit::MidiBankDown,
		};
		g.items.insert (g.items.end (), hidden_midi.begin (), hidden_midi.end ());
		groups.push_back (g);
	}
	{
		MenuGroup g;
		g.group = Group::Track;
		g.title = L"TRACKS";
		g.items = {
			TransportHit::AddAudio, TransportHit::AddMidi,
			TransportHit::RemoveTrack, TransportHit::TrackUp,
			TransportHit::TrackDown, TransportHit::Group,
		};
		groups.push_back (g);
	}
	{
		MenuGroup g;
		g.group = Group::View;
		g.title = L"VIEW";
		g.items = {
			TransportHit::FitSession, TransportHit::FitSelection,
			TransportHit::FitRegions, TransportHit::Spectrum,
			TransportHit::Phase, TransportHit::Loudness,
		};
		groups.push_back (g);
	}
	if (!show_files) {
		MenuGroup g;
		g.group = Group::File;
		g.title = L"FILE";
		g.items = {
			TransportHit::Import, TransportHit::Save,
			TransportHit::Open, TransportHit::Export,
		};
		groups.push_back (g);
	}
	{
		MenuGroup g;
		g.group = Group::App;
		g.title = L"APPLICATION";
		g.items = {
			TransportHit::Help, TransportHit::Prefs, TransportHit::About,
		};
		groups.push_back (g);
	}

	build_menu (groups, viewport_bottom);
}

void
TransportBar::build_menu (std::vector<MenuGroup>& groups, float viewport_bottom)
{
	_menu_rows.clear ();
	_menu_bounds = D2D1::RectF ();
	if (groups.empty ()) {
		return;
	}

	const float dp = _dpi_scale;
	const float pad = 10.f * dp;
	const float row_h = 26.f * dp;
	const float head_h = 20.f * dp;
	const float col_w = 190.f * dp;
	const float col_gap = 8.f * dp;
	const float W = _bounds.right - _bounds.left;

	const int max_cols = std::max (1, std::min (4,
	    (int)((W - 2.f * pad + col_gap) / (col_w + col_gap))));

	float total_h = pad * 2.f;
	for (const MenuGroup& g : groups) {
		if (g.items.empty ()) {
			continue;
		}
		total_h += head_h + (float)g.items.size () * row_h;
	}

	const float avail_h = (viewport_bottom > 0.f)
	                      ? viewport_bottom - _bounds.bottom - 12.f * dp
	                      : 0.f;
	int cols = max_cols;
	if (avail_h > 0.f) {
		for (int c = 1; c <= max_cols; ++c) {
			if (total_h / (float)c <= avail_h) {
				cols = c;
				break;
			}
		}
	}

	std::vector<std::vector<const MenuGroup*> > bins ((size_t)cols);
	std::vector<float> heights ((size_t)cols, 0.f);
	for (const MenuGroup& g : groups) {
		if (g.items.empty ()) {
			continue;
		}
		int best = 0;
		for (int i = 1; i < cols; ++i) {
			if (heights[(size_t)i] < heights[(size_t)best]) {
				best = i;
			}
		}
		bins[(size_t)best].push_back (&g);
		heights[(size_t)best] += head_h + (float)g.items.size () * row_h;
	}

	float tallest = 0.f;
	for (float h : heights) {
		tallest = std::max (tallest, h);
	}
	const float menu_w = pad * 2.f + (float)cols * col_w
	                     + (float)(cols - 1) * col_gap;
	const float menu_h = tallest + pad * 2.f;

	float menu_x = _overflow_btn.right - menu_w;
	menu_x = std::max (_bounds.left + 4.f * dp, menu_x);
	float menu_y = _bounds.bottom + 6.f * dp;
	if (viewport_bottom > 0.f && menu_y + menu_h > viewport_bottom - 8.f * dp) {
		menu_y = std::max (_bounds.top + 2.f * dp,
		                   viewport_bottom - 8.f * dp - menu_h);
	}
	_menu_bounds = D2D1::RectF (menu_x, menu_y, menu_x + menu_w, menu_y + menu_h);

	for (int c = 0; c < cols; ++c) {
		float x = _menu_bounds.left + pad + (float)c * (col_w + col_gap);
		float y = _menu_bounds.top + pad;
		for (const MenuGroup* g : bins[(size_t)c]) {
			MenuRow head;
			head.heading = true;
			head.text = g->title;
			head.hit = TransportHit::None;
			head.rect = D2D1::RectF (x, y, x + col_w, y + head_h);
			_menu_rows.push_back (head);
			y += head_h;
			for (TransportHit hit : g->items) {
				MenuRow row;
				row.heading = false;
				row.text = nullptr;
				row.hit = hit;
				row.rect = D2D1::RectF (x, y, x + col_w, y + row_h);
				_menu_rows.push_back (row);
				y += row_h;
			}
		}
	}
}

void
TransportBar::set_overflow_open (bool on)
{
	if (!on && _overflow_open) {
		_hover = TransportHit::None;
		_pressed = TransportHit::None;
	}
	_overflow_open = on;
}

bool
TransportBar::menu_contains (float x, float y) const
{
	return _overflow_open && contains (_menu_bounds, x, y);
}

float
TransportBar::shuttle_speed_at_x (float x) const
{
	const float w = _shuttle_btn.right - _shuttle_btn.left;
	if (w < 1.f) {
		return 0.f;
	}
	float t = (x - _shuttle_btn.left) / w;
	t = std::max (0.f, std::min (1.f, t));
	const float n = (t - 0.5f) * 2.f;
	if (std::fabs (n) < 0.08f) {
		return 0.f;
	}
	const float mag = std::fabs (n);
	const float speed = (mag * mag) * 8.f;
	return (n < 0.f) ? -speed : speed;
}

bool
TransportBar::file_button_rect (size_t index, D2D1_RECT_F* out) const
{
	if (index >= _files.size ()) {
		return false;
	}
	if (out) {
		*out = _files[index].rect;
	}
	return true;
}

TransportHit
TransportBar::hit_test (float x, float y) const
{
	if (_overflow_open) {
		for (const MenuRow& row : _menu_rows) {
			if (!row.heading && contains (row.rect, x, y)) {
				return row.hit;
			}
		}
		if (contains (_overflow_btn, x, y)) {
			return TransportHit::Overflow;
		}
		return TransportHit::None;
	}
	for (const HitSlot& s : _hit_rects) {
		if (contains (s.rect, x, y)) {
			return s.hit;
		}
	}
	return TransportHit::None;
}

bool
TransportBar::update_hover (float x, float y)
{
	_hover_x = x;
	_hover_y = y;
	TransportHit h = TransportHit::None;
	if (_overflow_open) {
		if (contains (_menu_bounds, x, y) || contains (_overflow_btn, x, y)) {
			h = hit_test (x, y);
		}
	} else if (contains (_bounds, x, y)) {
		h = hit_test (x, y);
	}
	if (h != _hover) {
		_hover = h;
		return true;
	}
	return false;
}

bool
TransportBar::clear_hover ()
{
	if (_hover == TransportHit::None) {
		return false;
	}
	_hover = TransportHit::None;
	return true;
}

bool
TransportBar::hover_tooltip (wchar_t* buf, size_t n) const
{
	if (_hover == TransportHit::None) {
		return false;
	}
	if (_hover == TransportHit::Shuttle) {
		_snwprintf_s (buf, n, _TRUNCATE, L"Shuttle — drag to scrub");
		return true;
	}
	label_for (_hover, buf, n);
	return buf[0] != L'\0';
}

const wchar_t*
TransportBar::glyph_for (TransportHit hit) const
{
	switch (hit) {
	case TransportHit::Stop:   return ui::glyph::Stop;
	case TransportHit::Rec:    return ui::glyph::Record;
	case TransportHit::Loop:   return ui::glyph::Repeat;
	case TransportHit::Punch:  return ui::glyph::Flag;
	case TransportHit::Click:  return ui::glyph::Volume;
	case TransportHit::CountIn: return ui::glyph::Clock;
	case TransportHit::Import: return ui::glyph::Import;
	case TransportHit::Save:   return ui::glyph::Save;
	case TransportHit::Open:   return ui::glyph::Open;
	case TransportHit::Export: return ui::glyph::Share;
	case TransportHit::Overflow: return ui::glyph::More;
	case TransportHit::PitchUp:
	case TransportHit::MidiTransUp:
	case TransportHit::MidiVelUp:
	case TransportHit::MidiLenUp:
	case TransportHit::MidiPgmUp:
	case TransportHit::MidiBankUp:
	case TransportHit::TrackUp: return ui::glyph::Up;
	case TransportHit::PitchDown:
	case TransportHit::MidiTransDown:
	case TransportHit::MidiVelDown:
	case TransportHit::MidiLenDown:
	case TransportHit::MidiPgmDown:
	case TransportHit::MidiBankDown:
	case TransportHit::TrackDown: return ui::glyph::Down;
	case TransportHit::MidiPanic: return ui::glyph::Warning;
	case TransportHit::AddAudio:
	case TransportHit::AddMidi: return ui::glyph::Add;
	case TransportHit::RemoveTrack: return ui::glyph::Remove;
	case TransportHit::FitSession:
	case TransportHit::FitSelection:
	case TransportHit::FitRegions: return ui::glyph::ZoomFit;
	case TransportHit::Help:   return ui::glyph::Help;
	case TransportHit::Prefs:  return ui::glyph::Settings;
	case TransportHit::About:  return ui::glyph::Info;
	default: return nullptr;
	}
}

bool
TransportBar::is_latched (TransportHit hit) const
{
	switch (hit) {
	case TransportHit::Play:    return _playing;
	case TransportHit::Rec:     return _recording || _record_armed;
	case TransportHit::Loop:    return _loop_playing || _loop_active;
	case TransportHit::Punch:   return _punch_enabled;
	case TransportHit::Snap:    return _snap_enabled;
	case TransportHit::Freeze:  return _freeze_active;
	case TransportHit::Click:   return _click_enabled;
	case TransportHit::CountIn: return _count_in_enabled;
	case TransportHit::MidiStep:  return _midi_step_enabled;
	case TransportHit::MidiChord: return _midi_chord_quality != 0;
	case TransportHit::MidiArp:   return _midi_arp_enabled;
	case TransportHit::MidiScale: return _midi_scale_lock;
	case TransportHit::Group:     return _group_active || _group_relative;
	case TransportHit::Spectrum:  return _spectrum_enabled;
	case TransportHit::Phase:     return _correlation_enabled;
	case TransportHit::Loudness:  return _loudness_enabled;
	default: return false;
	}
}

ui::ColorToken
TransportBar::latch_color (TransportHit hit) const
{
	switch (hit) {
	case TransportHit::Rec:    return ui::ColorToken::Record;
	case TransportHit::Loop:   return ui::ColorToken::Loop;
	case TransportHit::Punch:  return ui::ColorToken::Punch;
	default: return ui::ColorToken::Accent;
	}
}

void
TransportBar::label_for (TransportHit hit, wchar_t* buf, size_t n) const
{
	switch (hit) {
	case TransportHit::Play:
		_snwprintf_s (buf, n, _TRUNCATE, L"%s", _playing ? L"Pause" : L"Play");
		break;
	case TransportHit::Stop:
		_snwprintf_s (buf, n, _TRUNCATE, L"Stop");
		break;
	case TransportHit::Rec:
		_snwprintf_s (buf, n, _TRUNCATE, L"Record");
		break;
	case TransportHit::Loop:
		_snwprintf_s (buf, n, _TRUNCATE, L"Loop");
		break;
	case TransportHit::Punch:
		_snwprintf_s (buf, n, _TRUNCATE, L"Punch");
		break;
	case TransportHit::Click:
		_snwprintf_s (buf, n, _TRUNCATE, L"Click");
		break;
	case TransportHit::CountIn:
		_snwprintf_s (buf, n, _TRUNCATE, L"Count");
		break;
	case TransportHit::Split:       _snwprintf_s (buf, n, _TRUNCATE, L"Split"); break;
	case TransportHit::Fade:        _snwprintf_s (buf, n, _TRUNCATE, L"Fade"); break;
	case TransportHit::Mark:        _snwprintf_s (buf, n, _TRUNCATE, L"Marker"); break;
	case TransportHit::Snap:
		_snwprintf_s (buf, n, _TRUNCATE, L"%s", snap_btn_label ());
		break;
	case TransportHit::Freeze:      _snwprintf_s (buf, n, _TRUNCATE, L"Freeze"); break;
	case TransportHit::Stretch:     _snwprintf_s (buf, n, _TRUNCATE, L"Stretch"); break;
	case TransportHit::PitchUp:     _snwprintf_s (buf, n, _TRUNCATE, L"Pitch +1"); break;
	case TransportHit::PitchDown:   _snwprintf_s (buf, n, _TRUNCATE, L"Pitch −1"); break;
	case TransportHit::Reverse:     _snwprintf_s (buf, n, _TRUNCATE, L"Reverse"); break;
	case TransportHit::Normalize:   _snwprintf_s (buf, n, _TRUNCATE, L"Normalize"); break;
	case TransportHit::StripSilence: _snwprintf_s (buf, n, _TRUNCATE, L"Strip silence"); break;
	case TransportHit::Consolidate: _snwprintf_s (buf, n, _TRUNCATE, L"Consolidate"); break;
	case TransportHit::Quantize:    _snwprintf_s (buf, n, _TRUNCATE, L"Quantize MIDI"); break;
	case TransportHit::MidiTransUp:   _snwprintf_s (buf, n, _TRUNCATE, L"Transpose +1"); break;
	case TransportHit::MidiTransDown: _snwprintf_s (buf, n, _TRUNCATE, L"Transpose −1"); break;
	case TransportHit::MidiVelUp:     _snwprintf_s (buf, n, _TRUNCATE, L"Velocity +"); break;
	case TransportHit::MidiVelDown:   _snwprintf_s (buf, n, _TRUNCATE, L"Velocity −"); break;
	case TransportHit::MidiLenUp:     _snwprintf_s (buf, n, _TRUNCATE, L"Note length +"); break;
	case TransportHit::MidiLenDown:   _snwprintf_s (buf, n, _TRUNCATE, L"Note length −"); break;
	case TransportHit::MidiHumanize:  _snwprintf_s (buf, n, _TRUNCATE, L"Humanize"); break;
	case TransportHit::MidiStep:      _snwprintf_s (buf, n, _TRUNCATE, L"Step input"); break;
	case TransportHit::MidiChord:     _snwprintf_s (buf, n, _TRUNCATE, L"Chord"); break;
	case TransportHit::MidiArp:       _snwprintf_s (buf, n, _TRUNCATE, L"Arpeggiator"); break;
	case TransportHit::MidiScale:     _snwprintf_s (buf, n, _TRUNCATE, L"Scale lock"); break;
	case TransportHit::MidiPanic:     _snwprintf_s (buf, n, _TRUNCATE, L"All notes off"); break;
	case TransportHit::MidiPgmUp:     _snwprintf_s (buf, n, _TRUNCATE, L"Program +"); break;
	case TransportHit::MidiPgmDown:   _snwprintf_s (buf, n, _TRUNCATE, L"Program −"); break;
	case TransportHit::MidiBankUp:    _snwprintf_s (buf, n, _TRUNCATE, L"Bank +"); break;
	case TransportHit::MidiBankDown:  _snwprintf_s (buf, n, _TRUNCATE, L"Bank −"); break;
	case TransportHit::TempoDown:     _snwprintf_s (buf, n, _TRUNCATE, L"Tempo −"); break;
	case TransportHit::TempoUp:       _snwprintf_s (buf, n, _TRUNCATE, L"Tempo +"); break;
	case TransportHit::AddAudio:      _snwprintf_s (buf, n, _TRUNCATE, L"Add audio track"); break;
	case TransportHit::AddMidi:       _snwprintf_s (buf, n, _TRUNCATE, L"Add MIDI track"); break;
	case TransportHit::RemoveTrack:   _snwprintf_s (buf, n, _TRUNCATE, L"Remove track"); break;
	case TransportHit::TrackUp:       _snwprintf_s (buf, n, _TRUNCATE, L"Move track up"); break;
	case TransportHit::TrackDown:     _snwprintf_s (buf, n, _TRUNCATE, L"Move track down"); break;
	case TransportHit::Group:
		_snwprintf_s (buf, n, _TRUNCATE, L"%s", _group_relative ? L"Group (relative)" : L"Group");
		break;
	case TransportHit::FitSession:   _snwprintf_s (buf, n, _TRUNCATE, L"Fit session"); break;
	case TransportHit::FitSelection: _snwprintf_s (buf, n, _TRUNCATE, L"Fit selection"); break;
	case TransportHit::FitRegions:   _snwprintf_s (buf, n, _TRUNCATE, L"Fit all regions"); break;
	case TransportHit::Help:         _snwprintf_s (buf, n, _TRUNCATE, L"Keyboard shortcuts"); break;
	case TransportHit::Prefs:        _snwprintf_s (buf, n, _TRUNCATE, L"Preferences"); break;
	case TransportHit::About:        _snwprintf_s (buf, n, _TRUNCATE, L"About Airdour"); break;
	case TransportHit::Spectrum:     _snwprintf_s (buf, n, _TRUNCATE, L"Spectrum"); break;
	case TransportHit::Phase:        _snwprintf_s (buf, n, _TRUNCATE, L"Phase correlation"); break;
	case TransportHit::Loudness:     _snwprintf_s (buf, n, _TRUNCATE, L"Loudness (LUFS)"); break;
	case TransportHit::Import:       _snwprintf_s (buf, n, _TRUNCATE, L"Import audio"); break;
	case TransportHit::Save:         _snwprintf_s (buf, n, _TRUNCATE, L"Save session"); break;
	case TransportHit::Open:         _snwprintf_s (buf, n, _TRUNCATE, L"Open session"); break;
	case TransportHit::Export:       _snwprintf_s (buf, n, _TRUNCATE, L"Export WAV"); break;
	case TransportHit::Overflow:     _snwprintf_s (buf, n, _TRUNCATE, L"More actions"); break;
	default:                         _snwprintf_s (buf, n, _TRUNCATE, L""); break;
	}
}

void
TransportBar::row_label_for (TransportHit hit, wchar_t* buf, size_t n) const
{
	switch (hit) {
	case TransportHit::Mark:  _snwprintf_s (buf, n, _TRUNCATE, L"Mark"); break;
	case TransportHit::Snap:
		_snwprintf_s (buf, n, _TRUNCATE, L"%s", snap_btn_label ());
		break;
	case TransportHit::Freeze:
		_snwprintf_s (buf, n, _TRUNCATE, L"%s", _freeze_active ? L"Unfr" : L"Freeze");
		break;
	case TransportHit::MidiStep:
		_snwprintf_s (buf, n, _TRUNCATE, L"%s", _midi_step_enabled ? L"Step•" : L"Step");
		break;
	case TransportHit::MidiChord:
		if (_midi_chord_quality == 1) {
			_snwprintf_s (buf, n, _TRUNCATE, L"Maj");
		} else if (_midi_chord_quality == 2) {
			_snwprintf_s (buf, n, _TRUNCATE, L"Min");
		} else {
			_snwprintf_s (buf, n, _TRUNCATE, L"Chord");
		}
		break;
	case TransportHit::MidiArp:
		if (_midi_arp_enabled) {
			if (_midi_arp_mode == 1) {
				_snwprintf_s (buf, n, _TRUNCATE, L"Arp Dn");
			} else if (_midi_arp_mode == 2) {
				_snwprintf_s (buf, n, _TRUNCATE, L"Arp UD");
			} else {
				_snwprintf_s (buf, n, _TRUNCATE, L"Arp Up");
			}
		} else {
			_snwprintf_s (buf, n, _TRUNCATE, L"Arp");
		}
		break;
	case TransportHit::MidiScale:
		if (_midi_scale_lock) {
			static const wchar_t* roots[12] = {
				L"C", L"C#", L"D", L"Eb", L"E", L"F",
				L"F#", L"G", L"Ab", L"A", L"Bb", L"B",
			};
			int r = std::max (0, std::min (11, _midi_scale_root));
			_snwprintf_s (buf, n, _TRUNCATE, L"%s%s",
			              roots[r], _midi_scale_kind == 1 ? L"m" : L"");
		} else {
			_snwprintf_s (buf, n, _TRUNCATE, L"Scale");
		}
		break;
	case TransportHit::MidiTransUp: _snwprintf_s (buf, n, _TRUNCATE, L"Tr+"); break;
	case TransportHit::Quantize:    _snwprintf_s (buf, n, _TRUNCATE, L"Quant"); break;
	case TransportHit::Fade:        _snwprintf_s (buf, n, _TRUNCATE, L"Fade"); break;
	case TransportHit::Stretch:     _snwprintf_s (buf, n, _TRUNCATE, L"Stretch"); break;
	default:                        label_for (hit, buf, n); break;
	}
}

void
TransportBar::draw_primary (UiDraw& ui, TransportHit hit,
                            const D2D1_RECT_F& r) const
{
	wchar_t buf[48];
	label_for (hit, buf, 48);
	UiDraw::ButtonStyle st;
	st.glyph = glyph_for (hit);
	st.label = buf;
	st.show_label = (hit != TransportHit::Click && hit != TransportHit::CountIn);
	st.latched = is_latched (hit);
	st.latch = latch_color (hit);
	st.hovered = (_hover == hit);
	st.pressed = (_pressed == hit);
	if (hit == TransportHit::Play) {
		st.glyph = _playing ? ui::glyph::Pause : ui::glyph::Play;
	}
	ui.button (r, st);
}

void
TransportBar::draw_row (UiDraw& ui, TransportHit hit,
                        const D2D1_RECT_F& r) const
{
	wchar_t buf[32];
	row_label_for (hit, buf, 32);
	const bool on = is_latched (hit);
	const bool hovered = (_hover == hit);
	const bool pressed = (_pressed == hit);
	ui::ColorToken bg = on ? latch_color (hit) : ui::ColorToken::Control;
	if (!on) {
		if (pressed) {
			bg = ui::ColorToken::ControlPressed;
		} else if (hovered) {
			bg = ui::ColorToken::ControlHover;
		}
	}
	const ui::ColorToken border = (on || hovered || pressed)
	    ? ui::ColorToken::BorderStrong : ui::ColorToken::Border;
	const ui::ColorToken ink = on ? ui::ColorToken::AccentInk
	                              : (hovered ? ui::ColorToken::TextPrimary
	                                         : ui::ColorToken::TextSecondary);
	ui.fill_round (r, 5.f, bg);
	ui.stroke_round (r, 5.f, border, 1.f);
	ui.text_vcenter (buf, ui::TextRole::Body, ink, r,
	                 DWRITE_TEXT_ALIGNMENT_CENTER);
}

void
TransportBar::draw_file (UiDraw& ui, TransportHit hit,
                         const D2D1_RECT_F& r) const
{
	UiDraw::ButtonStyle st;
	st.glyph = glyph_for (hit);
	st.label = nullptr;
	st.show_label = false;
	st.hovered = (_hover == hit);
	st.pressed = (_pressed == hit);
	ui.button (r, st);
}

void
TransportBar::draw_clock (UiDraw& ui, float playhead_sec) const
{
	const float dp = _dpi_scale;
	const float cw = _clock_bounds.right - _clock_bounds.left;

	ui.fill_round (_clock_bounds, 7.f, ui::ColorToken::Control, 0.45f);
	ui.stroke_round (_clock_bounds, 7.f, ui::ColorToken::Border, 1.f, 0.9f);

	int m = (int)(playhead_sec / 60.f);
	float s = playhead_sec - (float)m * 60.f;
	if (s < 0.f) {
		s = 0.f;
	}
	wchar_t tbuf[32];
	_snwprintf_s (tbuf, _TRUNCATE, L"%02d:%05.2f", m, s);
	const D2D1_RECT_F l1 = D2D1::RectF (
		_clock_bounds.left + 10.f * dp, _clock_bounds.top + 2.f * dp,
		_tempo_dn_btn.left - 6.f * dp, _clock_bounds.top + 32.f * dp);
	ui.text_vcenter (tbuf, ui::TextRole::Clock, ui::ColorToken::TextPrimary, l1);

	auto mini = [&] (const D2D1_RECT_F& r, const wchar_t* text) {
		ui.fill_round (r, 3.f, ui::ColorToken::Control);
		ui.stroke_round (r, 3.f, ui::ColorToken::Border, 1.f);
		ui.text_vcenter (text, ui::TextRole::Caption,
		                 ui::ColorToken::TextSecondary, r,
		                 DWRITE_TEXT_ALIGNMENT_CENTER);
	};
	mini (_tempo_dn_btn, L"\u2212");
	mini (_tempo_up_btn, L"+");

	int bpm_i = (int)(_tempo_bpm + 0.5f);
	wchar_t l2[192];
	if (cw >= 228.f * dp) {
		_snwprintf_s (l2, _TRUNCATE,
		              L"%d|%d|%03d  ·  %02d:%02d:%02d:%02d  ·  %d BPM",
		              _bbt_bars, _bbt_beats, _bbt_ticks,
		              _tc_hours, _tc_minutes, _tc_seconds, _tc_frames,
		              bpm_i);
	} else {
		_snwprintf_s (l2, _TRUNCATE, L"%d|%d|%03d  ·  %d BPM",
		              _bbt_bars, _bbt_beats, _bbt_ticks, bpm_i);
	}
	const D2D1_RECT_F l2r = D2D1::RectF (
		_clock_bounds.left + 10.f * dp, _clock_bounds.top + 29.f * dp,
		_clock_bounds.right - 10.f * dp, _shuttle_btn.top - 1.f * dp);
	ui.text_vcenter (l2, ui::TextRole::Caption, ui::ColorToken::TextMuted, l2r);

	/* Shuttle strip: center detent, thumb from −1..+1. */
	const bool shuttling = std::fabs (_shuttle_norm) > 0.05f;
	ui.slider (_shuttle_btn, (_shuttle_norm + 1.f) * 0.5f,
	           shuttling ? ui::ColorToken::Accent : ui::ColorToken::TextMuted,
	           _hover == TransportHit::Shuttle);
	const float mid_x = (_shuttle_btn.left + _shuttle_btn.right) * 0.5f;
	ui.divider (D2D1::RectF (mid_x, _shuttle_btn.top - 1.f * dp,
	                         mid_x + std::max (1.f, 1.f * dp),
	                         _shuttle_btn.bottom + 1.f * dp));
}

void
TransportBar::draw_menu (UiDraw& ui) const
{
	if (!_overflow_open || _menu_rows.empty ()) {
		return;
	}
	const float dp = _dpi_scale;
	ui.shadow (_menu_bounds, 8.f);
	ui.fill_round (_menu_bounds, 8.f, ui::ColorToken::Card, 0.99f);
	ui.stroke_round (_menu_bounds, 8.f, ui::ColorToken::BorderStrong, 1.f);

	for (const MenuRow& row : _menu_rows) {
		if (row.heading) {
			ui.text_vcenter (row.text, ui::TextRole::Caption,
			                 ui::ColorToken::TextMuted,
			                 D2D1::RectF (row.rect.left + 10.f * dp, row.rect.top,
			                              row.rect.right - 10.f * dp, row.rect.bottom));
			continue;
		}
		wchar_t buf[64];
		label_for (row.hit, buf, 64);
		const bool hovered = (_hover == row.hit);
		const bool pressed = (_pressed == row.hit);
		if (hovered || pressed) {
			ui.fill_round (D2D1::RectF (row.rect.left + 4.f * dp, row.rect.top,
			                            row.rect.right - 4.f * dp, row.rect.bottom),
			               4.f,
			               pressed ? ui::ColorToken::ControlPressed
			                       : ui::ColorToken::ControlHover,
			               0.9f);
		}
		const wchar_t* glyph = glyph_for (row.hit);
		const float text_x = row.rect.left + (glyph ? 34.f : 12.f) * dp;
		if (glyph) {
			ui.text_vcenter (glyph, ui::TextRole::Glyph,
			                 ui::ColorToken::TextSecondary,
			                 D2D1::RectF (row.rect.left + 10.f * dp, row.rect.top,
			                              row.rect.left + 30.f * dp, row.rect.bottom));
		}
		ui.text_vcenter (buf, ui::TextRole::Body, ui::ColorToken::TextPrimary,
		                 D2D1::RectF (text_x, row.rect.top,
		                              row.rect.right - 26.f * dp, row.rect.bottom));
		if (is_latched (row.hit)) {
			ui.text_vcenter (ui::glyph::Check, ui::TextRole::Glyph,
			                 ui::ColorToken::Accent,
			                 D2D1::RectF (row.rect.right - 24.f * dp, row.rect.top,
			                              row.rect.right - 8.f * dp, row.rect.bottom),
			                 DWRITE_TEXT_ALIGNMENT_TRAILING);
		}
	}
}

void
TransportBar::draw_overlay (UiDraw& ui) const
{
	if (ui.ready ()) {
		draw_menu (ui);
	}
}

void
TransportBar::draw (UiDraw& ui, float playhead_sec) const
{
	if (!ui.ready ()) {
		return;
	}
	const float dp = _dpi_scale;

	ui.fill_round (_bounds, 0.f, ui::ColorToken::SurfaceRaised, 0.55f);
	ui.divider (D2D1::RectF (_bounds.left, _bounds.bottom - 1.f * dp,
	                         _bounds.right, _bounds.bottom));

	for (const HitSlot& s : _primary) {
		draw_primary (ui, s.hit, s.rect);
	}

	if (!_primary.empty ()) {
		const float dx = _primary.back ().rect.right + 8.f * dp;
		ui.divider (D2D1::RectF (dx, _bounds.top + 18.f * dp,
		                         dx + std::max (1.f, dp),
		                         _bounds.bottom - 18.f * dp));
	}

	for (const HitSlot& s : _rows) {
		draw_row (ui, s.hit, s.rect);
	}

	draw_clock (ui, playhead_sec);

	for (const HitSlot& s : _files) {
		draw_file (ui, s.hit, s.rect);
	}

	{
		UiDraw::ButtonStyle st;
		st.glyph = ui::glyph::More;
		st.label = nullptr;
		st.show_label = false;
		st.latched = _overflow_open;
		st.latch = ui::ColorToken::Accent;
		st.hovered = (_hover == TransportHit::Overflow);
		st.pressed = (_pressed == TransportHit::Overflow);
		ui.button (_overflow_btn, st);
	}
}
