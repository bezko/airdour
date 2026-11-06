/*
 * Copyright (C) 2026 Airdour contributors
 *
 * MIDI sequencing helpers extracted from engine_host (Phase 0/1):
 * multi-MIDI tracks, piano-roll viewport, VST3 instrument attach smokes.
 */

#include "engine_host.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE

#include "engine_host_priv.h"

#include "ardour/midi_track.h"
#include "ardour/plugin.h"
#include "ardour/plugin_insert.h"
#include "ardour/plugin_manager.h"
#include "ardour/port.h"
#include "ardour/session.h"
#include "pbd/mutex.h"

namespace {

std::wstring
utf8_to_wide_local (const std::string& s)
{
	if (s.empty ()) {
		return std::wstring ();
	}
	int n = MultiByteToWideChar (CP_UTF8, 0, s.c_str (), (int)s.size (), nullptr, 0);
	if (n <= 0) {
		return std::wstring ();
	}
	std::wstring out ((size_t)n, L'\0');
	MultiByteToWideChar (CP_UTF8, 0, s.c_str (), (int)s.size (), &out[0], n);
	return out;
}

} // namespace

bool
EngineHost::set_active_midi_track (int index)
{
	if (!_impl) {
		return false;
	}
	if (_impl->midi_tracks.empty ()) {
		if (!ensure_midi_track ()) {
			return false;
		}
	}
	if (index < 0 || index >= (int)_impl->midi_tracks.size ()) {
		_snap.detail = L"active MIDI track out of range";
		return false;
	}
	_impl->active_midi_track = index;
	_impl->sync_active_midi_from_index ();
	_snap.active_midi_track = index;
	refresh_midi_snap ();
	_snap.detail = L"active MIDI track set";
	return true;
}

bool
EngineHost::set_midi_pitch_viewport (int lo, int hi)
{
	lo = std::max (0, std::min (127, lo));
	hi = std::max (0, std::min (127, hi));
	if (hi < lo) {
		std::swap (lo, hi);
	}
	/* Keep at least one octave visible when possible. */
	if (hi - lo < 11) {
		hi = std::min (127, lo + 11);
		lo = std::max (0, hi - 11);
	}
	_snap.midi_pitch_lo = lo;
	_snap.midi_pitch_hi = hi;
	_snap.detail = L"piano-roll viewport set";
	return true;
}

bool
EngineHost::scroll_midi_pitch_viewport (int delta_semitones)
{
	int lo = _snap.midi_pitch_lo + delta_semitones;
	int hi = _snap.midi_pitch_hi + delta_semitones;
	int span = _snap.midi_pitch_hi - _snap.midi_pitch_lo;
	if (lo < 0) {
		lo = 0;
		hi = std::min (127, span);
	}
	if (hi > 127) {
		hi = 127;
		lo = std::max (0, 127 - span);
	}
	return set_midi_pitch_viewport (lo, hi);
}

bool
EngineHost::ensure_midi_track_count (int count)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	count = std::max (1, std::min (EngineSnapshot::kMeters, count));
	if (!ensure_midi_track ()) {
		return false;
	}
	/* Seed vector from legacy single pointer. */
	if (_impl->midi_tracks.empty () && _impl->midi_track) {
		_impl->midi_tracks.push_back (_impl->midi_track);
		_impl->midi_regions.push_back (_impl->midi_region);
		_impl->active_midi_track = 0;
	}
	while ((int)_impl->midi_tracks.size () < count) {
		try {
			ARDOUR::ChanCount midi_io (ARDOUR::DataType::MIDI, 1);
			ARDOUR::ChanCount outs = midi_io;
			outs.set (ARDOUR::DataType::AUDIO, 2);
			char name[32];
			std::snprintf (name, sizeof (name), "MIDI %d", (int)_impl->midi_tracks.size () + 1);
			std::list<std::shared_ptr<ARDOUR::MidiTrack>> created =
				_impl->session->new_midi_track (
					midi_io, outs, true, ARDOUR::PluginInfoPtr (),
					(ARDOUR::Plugin::PresetRecord*)0,
					std::shared_ptr<ARDOUR::RouteGroup> (), 1, name,
					ARDOUR::PresentationInfo::max_order, ARDOUR::Normal, true, false);
			if (created.empty () || !created.front ()) {
				_snap.detail = L"MIDI multi create failed";
				return false;
			}
			_impl->midi_tracks.push_back (created.front ());
			_impl->midi_regions.push_back (std::shared_ptr<ARDOUR::MidiRegion> ());
		} catch (...) {
			_snap.detail = L"MIDI multi exception";
			return false;
		}
	}
	_impl->sync_active_midi_from_index ();
	_snap.midi_track_count = (int)_impl->midi_tracks.size ();
	_snap.midi_track_ok = _snap.midi_track_count > 0;
	_snap.active_midi_track = _impl->active_midi_track;
	refresh_midi_snap ();
	return _snap.midi_track_count >= count;
}

bool
EngineHost::smoke_midi_multi ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-midi-multi — no Session";
		return false;
	}
	if (!ensure_midi_track_count (2)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-multi FAILED (ensure 2)\n");
		std::fflush (stderr);
		return false;
	}
	if (_snap.midi_track_count < 2) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-multi FAILED (count=%d)\n",
			      _snap.midi_track_count);
		std::fflush (stderr);
		return false;
	}
	if (!set_active_midi_track (1) || _snap.active_midi_track != 1) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-multi FAILED (active)\n");
		std::fflush (stderr);
		return false;
	}
	if (!set_midi_pitch_viewport (36, 84) || _snap.midi_pitch_lo != 36) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-multi FAILED (viewport)\n");
		std::fflush (stderr);
		return false;
	}
	(void)set_active_midi_track (0);
	(void)set_midi_pitch_viewport (48, 71);
	std::fprintf (stderr, "airdour_win_shell: smoke-midi-multi ok (tracks=%d)\n",
		      _snap.midi_track_count);
	std::fflush (stderr);
	_snap.detail = L"smoke-midi-multi ok";
	return true;
}

bool
EngineHost::attach_vst3_instrument_by_index (int index)
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"VST3 instrument — no Session";
		return false;
	}
	if (!ensure_midi_track ()) {
		return false;
	}
	try {
		if (_snap.vst3_scan_count <= 0) {
			if (!rescan_vst3 ()) {
				return false;
			}
		}
		if (_snap.vst3_scan_count <= 0) {
			_snap.detail = L"VST3 instrument — empty catalog";
			return true; /* soft PASS */
		}
		if (index < 0 || index >= _snap.vst3_scan_count) {
			_snap.detail = L"VST3 instrument index out of range";
			return false;
		}
		ARDOUR::PluginInfoPtr info;
		int at = 0;
		const ARDOUR::PluginInfoList& plugs = ARDOUR::PluginManager::instance ().vst3_plugin_info ();
		for (ARDOUR::PluginInfoList::const_iterator i = plugs.begin (); i != plugs.end (); ++i) {
			if (!*i) {
				continue;
			}
			if (at == index) {
				info = *i;
				break;
			}
			++at;
		}
		if (!info) {
			_snap.detail = L"VST3 instrument missing";
			return false;
		}
		if (!info->is_instrument ()) {
			/* Find next instrument from index. */
			at = 0;
			info.reset ();
			for (ARDOUR::PluginInfoList::const_iterator i = plugs.begin (); i != plugs.end (); ++i) {
				if (!*i) {
					continue;
				}
				if (at >= index && (*i)->is_instrument ()) {
					info = *i;
					break;
				}
				++at;
			}
			if (!info) {
				_snap.detail = L"VST3 instrument — none in catalog";
				return true; /* soft PASS */
			}
		}

		std::shared_ptr<ARDOUR::MidiTrack> mt = _impl->midi_track;
		ARDOUR::PluginPtr plugin = info->load (*_impl->session);
		if (!plugin) {
			_snap.detail = L"VST3 instrument load failed";
			return false;
		}
		std::shared_ptr<ARDOUR::PluginInsert> pi (
			new ARDOUR::PluginInsert (*_impl->session, *mt, plugin));
		pi->set_strict_io (true);
		if (mt->add_processor (pi, ARDOUR::PreFader) != 0) {
			_snap.detail = L"VST3 instrument add_processor failed";
			return false;
		}
		bool to_master = false;
		std::shared_ptr<ARDOUR::Route> master = _impl->session->master_out ();
		if (master && mt->output () && master->input ()) {
			ARDOUR::ChanCount want = mt->output ()->n_ports ();
			want.set (ARDOUR::DataType::AUDIO, 2);
			{
				PBD::Mutex::Lock lm (_impl->engine->process_lock ());
				mt->output ()->ensure_io (want, false);
			}
			uint32_t n_out = mt->output ()->n_ports ().n_audio ();
			uint32_t n_in = master->input ()->n_ports ().n_audio ();
			uint32_t n = std::min (n_out, n_in);
			for (uint32_t i = 0; i < n; ++i) {
				std::shared_ptr<ARDOUR::Port> our = mt->output ()->nth (i);
				std::shared_ptr<ARDOUR::Port> theirs = master->input ()->nth (i);
				if (our && theirs) {
					mt->output ()->connect (our, theirs->name ());
					to_master = true;
				}
			}
		}
		_impl->midi_synth_ok = true;
		_impl->midi_synth_to_master = to_master;
		_impl->midi_synth_name = info->name.empty () ? "VST3 Instrument" : info->name;
		_snap.midi_synth_ok = true;
		_snap.midi_synth_to_master = to_master;
		_snap.midi_synth_name = utf8_to_wide_local (_impl->midi_synth_name);
		_snap.detail = L"VST3 instrument attached";
		std::fprintf (stderr, "airdour_win_shell: VST3 instrument \"%s\" (to_master=%d)\n",
			      _impl->midi_synth_name.c_str (), to_master ? 1 : 0);
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: attach_vst3_instrument: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_vst3_instrument ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-vst3-instrument — no Session";
		return false;
	}
	if (!rescan_vst3 ()) {
		return false;
	}
	if (_snap.vst3_scan_count <= 0) {
		std::fprintf (stderr,
			      "airdour_win_shell: smoke-vst3-instrument ok (scan-only; set VST3_PATH)\n");
		std::fflush (stderr);
		return true;
	}
	if (!attach_vst3_instrument_by_index (0)) {
		/* Soft PASS when catalog has only FX. */
		if (_snap.detail.find (L"none in catalog") != std::wstring::npos || _snap.detail.find (L"empty catalog") != std::wstring::npos) {
			std::fprintf (stderr,
				      "airdour_win_shell: smoke-vst3-instrument ok (no instruments)\n");
			std::fflush (stderr);
			return true;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-vst3-instrument FAILED\n");
		std::fflush (stderr);
		return false;
	}
	std::fprintf (stderr, "airdour_win_shell: smoke-vst3-instrument ok\n");
	std::fflush (stderr);
	return true;
}

#else /* !AIRDOUR_WIN_SHELL_WITH_ENGINE */

bool
EngineHost::set_active_midi_track (int)
{
	return false;
}
bool
EngineHost::set_midi_pitch_viewport (int, int)
{
	return false;
}
bool
EngineHost::scroll_midi_pitch_viewport (int)
{
	return false;
}
bool
EngineHost::ensure_midi_track_count (int)
{
	return false;
}
bool
EngineHost::smoke_midi_multi ()
{
	return false;
}
bool
EngineHost::attach_vst3_instrument_by_index (int)
{
	return false;
}
bool
EngineHost::smoke_vst3_instrument ()
{
	return false;
}

#endif
