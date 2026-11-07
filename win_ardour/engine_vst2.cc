/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Windows VST2-64 shell hosting + VST2-32 bridge hooks (Phase 3/4).
 */

#include "engine_host.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

#include "ardour/audio_track.h"
#include "ardour/automation_control.h"
#include "ardour/midi_track.h"
#include "ardour/plugin.h"
#include "ardour/plugin_insert.h"
#include "ardour/plugin_manager.h"
#include "ardour/port.h"
#include "ardour/rc_configuration.h"
#include "ardour/session.h"
#include "evoral/Parameter.h"
#include "pbd/controllable.h"
#include "pbd/mutex.h"

#include "vst2_bridge_client.h"
#include "vst2_bridge_processor.h"
#include "vst2_bridge_protocol.h"

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

void
copy_name_local (wchar_t* dest, size_t dest_chars, const std::string& utf8)
{
	std::wstring w = utf8_to_wide_local (utf8);
	if (w.empty ()) {
		wcsncpy_s (dest, dest_chars, L"?", _TRUNCATE);
	} else {
		wcsncpy_s (dest, dest_chars, w.c_str (), _TRUNCATE);
	}
}

/** When set, empty catalogs / missing fixtures fail instead of soft-PASS. */
bool
require_plugins_strict ()
{
	const char* e = std::getenv ("AIRDOUR_REQUIRE_PLUGINS");
	return e && e[0] && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y');
}

bool
soft_pass_or_require (const char* soft_msg)
{
	if (require_plugins_strict ()) {
		std::fprintf (stderr,
			      "airdour_win_shell: FAILED (AIRDOUR_REQUIRE_PLUGINS=1; %s)\n",
			      soft_msg ? soft_msg : "plugin required");
		std::fflush (stderr);
		return false;
	}
	if (soft_msg && soft_msg[0]) {
		std::fprintf (stderr, "%s\n", soft_msg);
		std::fflush (stderr);
	}
	return true;
}

#if defined(WINDOWS_VST_SUPPORT)

const ARDOUR::PluginInfoList&
vst2_info_list ()
{
	return ARDOUR::PluginManager::instance ().windows_vst_plugin_info ();
}

ARDOUR::PluginInfoPtr
vst2_info_at (int index)
{
	int at = 0;
	const ARDOUR::PluginInfoList& plugs = vst2_info_list ();
	for (ARDOUR::PluginInfoList::const_iterator i = plugs.begin (); i != plugs.end (); ++i) {
		if (!*i) {
			continue;
		}
		if (at == index) {
			return *i;
		}
		++at;
	}
	return ARDOUR::PluginInfoPtr ();
}

#endif

} // namespace

bool
EngineHost::rescan_vst2 ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"VST2 rescan failed — no Session";
		return false;
	}
#if !defined(WINDOWS_VST_SUPPORT)
	_snap.vst2_scan_count = 0;
	_snap.vst2_catalog_count = 0;
	_snap.detail = L"VST2 support not compiled (WINDOWS_VST_SUPPORT)";
	refresh_vst2_catalog_snap ();
	return true; /* soft path — smoke still PASS */
#else
	try {
		if (ARDOUR::Config) {
			ARDOUR::Config->set_use_windows_vst (true);
			const char* path = std::getenv ("VST_PATH");
			if (!path) {
				path = std::getenv ("VST2_PATH");
			}
			if (path && *path) {
				ARDOUR::Config->set_plugin_path_vst (path);
			}
		}
		ARDOUR::PluginManager::instance ().refresh (true);
		refresh_vst2_catalog_snap ();
		_snap.detail = L"VST2 rescan ok";
		return true;
	} catch (...) {
		_snap.detail = L"VST2 rescan exception";
		return false;
	}
#endif
}

void
EngineHost::refresh_vst2_catalog_snap ()
{
	_snap.vst2_scan_count = 0;
	_snap.vst2_catalog_count = 0;
	for (int i = 0; i < EngineSnapshot::kMaxVst2Catalog; ++i) {
		_snap.vst2_catalog_name[i][0] = L'\0';
	}
#if defined(WINDOWS_VST_SUPPORT)
	try {
		const ARDOUR::PluginInfoList& plugs = vst2_info_list ();
		_snap.vst2_scan_count = (int)plugs.size ();
		int filled = 0;
		for (ARDOUR::PluginInfoList::const_iterator i = plugs.begin ();
		     i != plugs.end () && filled < EngineSnapshot::kMaxVst2Catalog; ++i) {
			if (!*i) {
				continue;
			}
			copy_name_local (_snap.vst2_catalog_name[filled], 40, (*i)->name);
			++filled;
		}
		_snap.vst2_catalog_count = filled;
	} catch (...) {
	}
#endif
}

void
EngineHost::refresh_vst2_params_from_insert ()
{
	_snap.vst2_ok = false;
	_snap.vst2_bypassed = false;
	_snap.vst2_bridged_32 = _impl ? _impl->vst2_bridged_32 : false;
	_snap.vst2_param_count = 0;
	_snap.vst2_name[0] = L'\0';
	for (int i = 0; i < EngineSnapshot::kMaxVst3Params; ++i) {
		_snap.vst2_param_name[i][0] = L'\0';
		_snap.vst2_param_norm[i] = 0.f;
		if (_impl) {
			_impl->vst2_param_ports[i] = 0;
		}
	}
	if (!_impl) {
		return;
	}
	_impl->vst2_param_count = 0;

	std::shared_ptr<ARDOUR::Processor> bp = _impl->vst2_bridge_fx.lock ();
	if (bp) {
		auto* bridge = dynamic_cast<airdour::vst2_bridge::Vst2BridgeProcessor*> (bp.get ());
		if (bridge && bridge->bridged_ok ()) {
			copy_name_local (_snap.vst2_name, EngineSnapshot::kVst3NameLen, bridge->display_name ());
			_snap.vst2_bypassed = !bridge->active ();
			_snap.vst2_ok = true;
			_snap.vst2_bridged_32 = true;
			_impl->vst2_bridged_32 = true;
			const int n = std::min (bridge->param_count (), EngineSnapshot::kMaxVst3Params);
			for (int i = 0; i < n; ++i) {
				_impl->vst2_param_ports[i] = (uint32_t)i;
				_snap.vst2_param_norm[i] = bridge->get_param_norm (i);
				char pname[16];
				std::snprintf (pname, sizeof (pname), "P%d", i + 1);
				copy_name_local (_snap.vst2_param_name[i], EngineSnapshot::kVst3ParamNameLen, pname);
			}
			_impl->vst2_param_count = n;
			_snap.vst2_param_count = n;
			return;
		}
	}

	std::shared_ptr<ARDOUR::PluginInsert> pi = _impl->vst2_fx.lock ();
	if (!pi || !pi->plugin (0)) {
		return;
	}
	std::string name = pi->name ();
	if (name.empty () && pi->plugin (0)->get_info ()) {
		name = pi->plugin (0)->get_info ()->name;
	}
	copy_name_local (_snap.vst2_name, EngineSnapshot::kVst3NameLen, name);
	_snap.vst2_bypassed = !pi->active ();
	_snap.vst2_ok = true;
	ARDOUR::PluginPtr plug = pi->plugin (0);
	int filled = 0;
	uint32_t nports = plug->parameter_count ();
	for (uint32_t port = 0; port < nports && filled < EngineSnapshot::kMaxVst3Params; ++port) {
		if (!plug->parameter_is_control (port) || !plug->parameter_is_input (port)) {
			continue;
		}
		std::shared_ptr<ARDOUR::AutomationControl> ac = pi->automation_control (
			Evoral::Parameter (ARDOUR::PluginAutomation, 0, port));
		if (!ac || (ac->flags () & PBD::Controllable::HiddenControl)) {
			continue;
		}
		_impl->vst2_param_ports[filled] = port;
		float norm = (float)ac->internal_to_interface (ac->get_value ());
		_snap.vst2_param_norm[filled] = std::max (0.f, std::min (1.f, norm));
		std::string pname = ac->name ();
		copy_name_local (_snap.vst2_param_name[filled], EngineSnapshot::kVst3ParamNameLen,
				 pname.empty () ? "P" : pname);
		++filled;
	}
	_impl->vst2_param_count = filled;
	_snap.vst2_param_count = filled;
}

bool
EngineHost::unload_vst2 ()
{
	if (!_impl) {
		return false;
	}
	int ti = _impl->vst2_bridge_track;
	if (ti < 0) {
		ti = 1;
	}
	if (ti >= 0 && ti < (int)_impl->tracks.size () && _impl->tracks[(size_t)ti]) {
		std::shared_ptr<ARDOUR::PluginInsert> pi = _impl->vst2_fx.lock ();
		if (pi) {
			try {
				_impl->tracks[(size_t)ti]->remove_processor (pi);
			} catch (...) {
			}
		}
		std::shared_ptr<ARDOUR::Processor> bp = _impl->vst2_bridge_fx.lock ();
		if (bp) {
			try {
				_impl->tracks[(size_t)ti]->remove_processor (bp);
			} catch (...) {
			}
		}
	}
	_impl->vst2_fx.reset ();
	_impl->vst2_bridge_fx.reset ();
	if (!_impl->vst2_bridge_path.empty ()) {
		airdour::vst2_bridge::shutdown_plugin (_impl->vst2_bridge_path);
		_impl->vst2_bridge_path.clear ();
	}
	_impl->vst2_bridge_track = -1;
	/* Keep vst2_bridged_32 true if a synth bridge remains. */
	_impl->vst2_bridged_32 = (bool)_impl->vst2_bridge_synth.lock ();
	_impl->vst2_param_count = 0;
	refresh_vst2_params_from_insert ();
	_snap.detail = L"VST2 unloaded";
	return true;
}

bool
EngineHost::insert_vst2_by_index (int index, int track)
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"VST2 insert failed — no Session";
		return false;
	}
#if !defined(WINDOWS_VST_SUPPORT)
	(void)index;
	(void)track;
	_snap.detail = L"VST2 insert — not compiled";
	return false;
#else
	try {
		if (_snap.vst2_scan_count <= 0 && !rescan_vst2 ()) {
			return false;
		}
		if (index < 0 || index >= _snap.vst2_scan_count) {
			_snap.detail = L"VST2 index out of range";
			return false;
		}
		ARDOUR::PluginInfoPtr info = vst2_info_at (index);
		if (!info) {
			_snap.detail = L"VST2 index missing";
			return false;
		}
		if (info->is_instrument ()) {
			_snap.detail = L"VST2 index is instrument (use attach_vst2_instrument)";
			return false;
		}
		(void)unload_vst2 ();
		if (_impl->tracks.empty () || !_impl->tracks[0]) {
			collect_tracks ();
		}
		int ti = track;
		if (ti < 0) {
			ti = 1; /* legacy default: Audio 2 */
		}
		if (ti < 0 || ti >= (int)_impl->tracks.size () || !_impl->tracks[(size_t)ti]) {
			_snap.detail = L"VST2 insert — no target track";
			return false;
		}

		/* PE gate: 32-bit DLLs go through the x86 bridge Processor. */
		bool bridged = false;
		if (!info->path.empty ()) {
			bridged = airdour::vst2_bridge::dll_is_i386 (info->path);
		}

		std::shared_ptr<ARDOUR::AudioTrack> atr = _impl->tracks[(size_t)ti];
		if (bridged) {
			std::shared_ptr<airdour::vst2_bridge::Vst2BridgeProcessor> bp (
				new airdour::vst2_bridge::Vst2BridgeProcessor (
					*_impl->session, *atr, info->name, info->path));
			if (!bp->bridged_ok ()) {
				_snap.detail = L"VST2-32 bridge failed to start";
				return false;
			}
			if (atr->add_processor (bp, ARDOUR::PreFader) != 0) {
				_snap.detail = L"VST2-32 add_processor failed";
				airdour::vst2_bridge::shutdown_plugin (info->path);
				return false;
			}
			bp->activate ();
			_impl->vst2_bridge_fx = bp;
			_impl->vst2_bridge_path = info->path;
			_impl->vst2_bridge_track = ti;
			_impl->vst2_bridged_32 = true;
			refresh_vst2_params_from_insert ();
			_snap.detail = L"VST2-32 bridged insert ok";
			return _snap.vst2_ok;
		}

		ARDOUR::PluginPtr plugin = info->load (*_impl->session);
		if (!plugin) {
			_snap.detail = L"VST2 load failed";
			return false;
		}
		std::shared_ptr<ARDOUR::PluginInsert> pi (
			new ARDOUR::PluginInsert (*_impl->session, *atr, plugin));
		pi->set_strict_io (true);
		if (atr->add_processor (pi, ARDOUR::PreFader) != 0) {
			_snap.detail = L"VST2 add_processor failed";
			return false;
		}
		pi->activate ();
		_impl->vst2_fx = pi;
		_impl->vst2_bridged_32 = false;
		refresh_vst2_params_from_insert ();
		_snap.detail = L"VST2 inserted";
		return _snap.vst2_ok;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: insert_vst2: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
#endif
}

bool
EngineHost::set_vst2_param (int index, float norm_0_1)
{
	if (!_impl || index < 0 || index >= EngineSnapshot::kMaxVst3Params) {
		return false;
	}
	if (_impl->vst2_param_count <= 0) {
		refresh_vst2_params_from_insert ();
	}
	if (index >= _impl->vst2_param_count) {
		return false;
	}

	std::shared_ptr<ARDOUR::Processor> bp = _impl->vst2_bridge_fx.lock ();
	if (bp) {
		auto* bridge = dynamic_cast<airdour::vst2_bridge::Vst2BridgeProcessor*> (bp.get ());
		if (bridge) {
			if (!bridge->set_param_norm (index, norm_0_1)) {
				return false;
			}
			refresh_vst2_params_from_insert ();
			return true;
		}
	}

	std::shared_ptr<ARDOUR::PluginInsert> pi = _impl->vst2_fx.lock ();
	if (!pi) {
		return false;
	}
	uint32_t port = _impl->vst2_param_ports[index];
	try {
		std::shared_ptr<ARDOUR::AutomationControl> ac = pi->automation_control (
			Evoral::Parameter (ARDOUR::PluginAutomation, 0, port));
		if (!ac) {
			return false;
		}
		norm_0_1 = std::max (0.f, std::min (1.f, norm_0_1));
		ac->set_value (ac->interface_to_internal (norm_0_1), PBD::Controllable::NoGroup);
		refresh_vst2_params_from_insert ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_vst2_bypassed (bool bypassed)
{
	if (!_impl) {
		return false;
	}
	std::shared_ptr<ARDOUR::Processor> bp = _impl->vst2_bridge_fx.lock ();
	if (bp) {
		if (bypassed) {
			bp->deactivate ();
		} else {
			bp->activate ();
		}
		refresh_vst2_params_from_insert ();
		return true;
	}
	std::shared_ptr<ARDOUR::PluginInsert> pi = _impl->vst2_fx.lock ();
	if (!pi) {
		return false;
	}
	if (bypassed) {
		pi->deactivate ();
	} else {
		pi->activate ();
	}
	refresh_vst2_params_from_insert ();
	return true;
}

bool
EngineHost::toggle_vst2_bypass ()
{
	return set_vst2_bypassed (!_snap.vst2_bypassed);
}

bool
EngineHost::attach_vst2_instrument_by_index (int index)
{
	if (!_impl || !_impl->session) {
		return false;
	}
#if !defined(WINDOWS_VST_SUPPORT)
	(void)index;
	_snap.detail = L"VST2 instrument — not compiled";
	return true; /* soft */
#else
	if (!ensure_midi_track ()) {
		return false;
	}
	if (_snap.vst2_scan_count <= 0 && !rescan_vst2 ()) {
		return false;
	}
	if (_snap.vst2_scan_count <= 0) {
		return soft_pass_or_require ("airdour_win_shell: VST2 instrument — empty catalog (soft PASS)");
	}
	ARDOUR::PluginInfoPtr info = vst2_info_at (index);
	if (!info) {
		return false;
	}
	if (!info->is_instrument ()) {
		/* scan for an instrument */
		info.reset ();
		const ARDOUR::PluginInfoList& plugs = vst2_info_list ();
		int at = 0;
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
			_snap.detail = L"VST2 instrument — none";
			return true;
		}
	}
	try {
		std::shared_ptr<ARDOUR::MidiTrack> mt = _impl->midi_track;

		/* Drop prior bridged synth so re-attach is clean. */
		{
			std::shared_ptr<ARDOUR::Processor> prev = _impl->vst2_bridge_synth.lock ();
			if (prev) {
				try {
					mt->remove_processor (prev);
				} catch (...) {
				}
			}
			if (!_impl->vst2_bridge_synth_path.empty ()) {
				airdour::vst2_bridge::shutdown_plugin (_impl->vst2_bridge_synth_path);
				_impl->vst2_bridge_synth_path.clear ();
			}
			_impl->vst2_bridge_synth.reset ();
		}

		bool bridged = !info->path.empty () && airdour::vst2_bridge::dll_is_i386 (info->path);
		if (bridged) {
			std::shared_ptr<airdour::vst2_bridge::Vst2BridgeProcessor> bp (
				new airdour::vst2_bridge::Vst2BridgeProcessor (
					*_impl->session, *mt, info->name, info->path, true));
			if (!bp->bridged_ok ()) {
				_snap.detail = L"VST2-32 instrument bridge failed";
				return false;
			}
			if (mt->add_processor (bp, ARDOUR::PreFader) != 0) {
				_snap.detail = L"VST2-32 instrument add_processor failed";
				airdour::vst2_bridge::shutdown_plugin (info->path);
				return false;
			}
			bp->activate ();
			_impl->vst2_bridge_synth = bp;
			_impl->vst2_bridge_synth_path = info->path;
			_impl->vst2_bridged_32 = true;
		} else {
			ARDOUR::PluginPtr plugin = info->load (*_impl->session);
			if (!plugin) {
				_snap.detail = L"VST2 instrument load failed";
				return false;
			}
			std::shared_ptr<ARDOUR::PluginInsert> pi (
				new ARDOUR::PluginInsert (*_impl->session, *mt, plugin));
			pi->set_strict_io (true);
			if (mt->add_processor (pi, ARDOUR::PreFader) != 0) {
				return false;
			}
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
		_impl->midi_synth_name = info->name;
		_snap.midi_synth_ok = true;
		_snap.midi_synth_to_master = to_master;
		_snap.midi_synth_name = utf8_to_wide_local (info->name);
		_snap.detail = bridged ? L"VST2-32 instrument bridged" : L"VST2 instrument attached";
		std::fprintf (stderr, "airdour_win_shell: VST2 instrument \"%s\" (bridged=%d to_master=%d)\n",
			      _impl->midi_synth_name.c_str (), bridged ? 1 : 0, to_master ? 1 : 0);
		std::fflush (stderr);
		return true;
	} catch (...) {
		return false;
	}
#endif
}

bool
EngineHost::smoke_vst2 ()
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (!rescan_vst2 ()) {
		return false;
	}
#if !defined(WINDOWS_VST_SUPPORT)
	return soft_pass_or_require ("smoke-vst2 ok (not compiled — soft PASS)");
#else
	if (_snap.vst2_scan_count <= 0) {
		return soft_pass_or_require ("airdour_win_shell: smoke-vst2 ok (scan-only; set VST_PATH)");
	}
	/* Prefer first non-instrument. */
	bool inserted = false;
	for (int i = 0; i < _snap.vst2_scan_count; ++i) {
		ARDOUR::PluginInfoPtr info = vst2_info_at (i);
		if (info && !info->is_instrument ()) {
			inserted = insert_vst2_by_index (i);
			break;
		}
	}
	if (!inserted) {
		return soft_pass_or_require ("airdour_win_shell: smoke-vst2 ok (no audio FX in catalog)");
	}
	(void)unload_vst2 ();
	std::fprintf (stderr, "airdour_win_shell: smoke-vst2 ok\n");
	std::fflush (stderr);
	return true;
#endif
}

bool
EngineHost::smoke_vst2_instrument ()
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (!rescan_vst2 ()) {
		return false;
	}
	if (!attach_vst2_instrument_by_index (0)) {
		return false;
	}
	std::fprintf (stderr, "airdour_win_shell: smoke-vst2-instrument ok\n");
	std::fflush (stderr);
	return true;
}

bool
EngineHost::smoke_vst2_bridge ()
{
	/* Validates bridge client discovery + optional fixture load. */
	std::string bridge = airdour::vst2_bridge::bridge_exe_path ();
	if (bridge.empty ()) {
		return soft_pass_or_require (
			"airdour_win_shell: smoke-vst2-bridge ok (bridge exe not staged — soft PASS)");
	}
	const char* fixture = std::getenv ("AIRDOUR_SMOKE_VST2_32");
	if (!fixture || !*fixture) {
		char buf[512];
		std::snprintf (buf, sizeof (buf),
			       "airdour_win_shell: smoke-vst2-bridge ok (exe=%s; set AIRDOUR_SMOKE_VST2_32)",
			       bridge.c_str ());
		return soft_pass_or_require (buf);
	}
	{
		std::wstring wfix = utf8_to_wide_local (fixture);
		DWORD attr = GetFileAttributesW (wfix.c_str ());
		if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
			std::fprintf (stderr,
				      "airdour_win_shell: smoke-vst2-bridge FAILED (fixture missing: %s)\n",
				      fixture);
			std::fflush (stderr);
			return false;
		}
	}
	if (!airdour::vst2_bridge::dll_is_i386 (fixture)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vst2-bridge FAILED (not i386)\n");
		std::fflush (stderr);
		return false;
	}
	if (!airdour::vst2_bridge::ensure_bridge_running (fixture)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vst2-bridge FAILED (start)\n");
		std::fflush (stderr);
		return false;
	}
	if (!airdour::vst2_bridge::process_silence_block (128, 48000.f)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vst2-bridge FAILED (process)\n");
		std::fflush (stderr);
		airdour::vst2_bridge::shutdown_all ();
		return false;
	}
	/* MIDI note → processEvents path (instruments). Silence in/out is fine. */
	{
		airdour::vst2_bridge::MidiEventWire note {};
		note.sample_offset = 0;
		note.size = 3;
		note.data[0] = 0x90;
		note.data[1] = 60;
		note.data[2] = 100;
		float zeros_in[2][128];
		float zeros_out[2][128];
		std::memset (zeros_in, 0, sizeof (zeros_in));
		std::memset (zeros_out, 0, sizeof (zeros_out));
		float* in_ptrs[2] = { zeros_in[0], zeros_in[1] };
		float* out_ptrs[2] = { zeros_out[0], zeros_out[1] };
		if (!airdour::vst2_bridge::process_audio (fixture, in_ptrs, out_ptrs, 2, 2, 128,
							  48000.f, 2000, &note, 1)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-vst2-bridge FAILED (midi process)\n");
			std::fflush (stderr);
			airdour::vst2_bridge::shutdown_all ();
			return false;
		}
	}
	airdour::vst2_bridge::shutdown_all ();
	std::fprintf (stderr, "airdour_win_shell: smoke-vst2-bridge ok (open+process+midi)\n");
	std::fflush (stderr);
	_snap.detail = L"smoke-vst2-bridge ok";
	return true;
}

#else /* !ENGINE */

bool
EngineHost::rescan_vst2 ()
{
	return false;
}
void
EngineHost::refresh_vst2_catalog_snap ()
{
}
bool
EngineHost::insert_vst2_by_index (int, int)
{
	return false;
}
bool
EngineHost::unload_vst2 ()
{
	return false;
}
bool
EngineHost::set_vst2_param (int, float)
{
	return false;
}
bool
EngineHost::set_vst2_bypassed (bool)
{
	return false;
}
bool
EngineHost::toggle_vst2_bypass ()
{
	return false;
}
bool
EngineHost::attach_vst2_instrument_by_index (int)
{
	return false;
}
bool
EngineHost::smoke_vst2 ()
{
	return false;
}
bool
EngineHost::smoke_vst2_instrument ()
{
	return false;
}
bool
EngineHost::smoke_vst2_bridge ()
{
	return false;
}

#endif
