/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Insert-rack / ACE FX chain helpers (Phase 2).
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

#include "ardour/audio_track.h"
#include "ardour/plugin.h"
#include "ardour/plugin_insert.h"
#include "ardour/plugin_manager.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/automation_control.h"
#include "evoral/Parameter.h"
#include "pbd/controllable.h"

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
fill_slot_from_insert (EngineSnapshot::InsertSlotSnap& slot, std::shared_ptr<ARDOUR::PluginInsert> pi)
{
	slot = EngineSnapshot::InsertSlotSnap ();
	if (!pi) {
		return;
	}
	slot.ok = true;
	slot.bypassed = !pi->active ();
	std::string name = pi->name ();
	std::wstring w = utf8_to_wide_local (name.empty () ? "Insert" : name);
	wcsncpy_s (slot.name, EngineSnapshot::kInsertNameLen, w.c_str (), _TRUNCATE);

	ARDOUR::PluginPtr plug = pi->plugin (0);
	slot.is_instrument = plug && plug->get_info () && plug->get_info ()->is_instrument ();

	int filled = 0;
	if (plug) {
		uint32_t nports = plug->parameter_count ();
		for (uint32_t port = 0; port < nports && filled < EngineSnapshot::kMaxVst3Params; ++port) {
			if (!plug->parameter_is_control (port) || !plug->parameter_is_input (port)) {
				continue;
			}
			try {
				std::shared_ptr<ARDOUR::AutomationControl> ac = pi->automation_control (
					Evoral::Parameter (ARDOUR::PluginAutomation, 0, port));
				if (!ac) {
					continue;
				}
				if (ac->flags () & PBD::Controllable::HiddenControl) {
					continue;
				}
				std::string pname = ac->name ();
				if (pname.empty ()) {
					pname = plug->describe_parameter (
						Evoral::Parameter (ARDOUR::PluginAutomation, 0, port));
				}
				std::wstring pw = utf8_to_wide_local (pname.empty () ? "P" : pname);
				wcsncpy_s (slot.param_name[filled], EngineSnapshot::kVst3ParamNameLen,
					   pw.c_str (), _TRUNCATE);
				float norm = (float)ac->internal_to_interface (ac->get_value ());
				slot.param_norm[filled] = std::max (0.f, std::min (1.f, norm));
				++filled;
			} catch (...) {
			}
		}
	}
	slot.param_count = filled;
}

} // namespace

void
EngineHost::refresh_insert_rack_snap ()
{
	for (int t = 0; t < EngineSnapshot::kMeters; ++t) {
		_snap.insert_slot_count[t] = 0;
		for (int s = 0; s < EngineSnapshot::kMaxInsertSlots; ++s) {
			_snap.insert_slots[t][s] = EngineSnapshot::InsertSlotSnap ();
		}
	}
	_snap.master_insert_count = 0;
	for (int s = 0; s < EngineSnapshot::kMaxInsertSlots; ++s) {
		_snap.master_inserts[s] = EngineSnapshot::InsertSlotSnap ();
	}
	if (!_impl) {
		return;
	}
	for (int t = 0; t < EngineSnapshot::kMeters; ++t) {
		int n = std::max (0, std::min (EngineSnapshot::kMaxInsertSlots, _impl->rack_slot_count[t]));
		_snap.insert_slot_count[t] = n;
		for (int s = 0; s < n; ++s) {
			fill_slot_from_insert (_snap.insert_slots[t][s], _impl->rack_slots[t][s].lock ());
		}
	}
	int mn = std::max (0, std::min (EngineSnapshot::kMaxInsertSlots, _impl->master_rack_count));
	_snap.master_insert_count = mn;
	for (int s = 0; s < mn; ++s) {
		fill_slot_from_insert (_snap.master_inserts[s], _impl->master_rack[s].lock ());
	}
}

bool
EngineHost::insert_rack_slot (int track, int kind, int catalog_index)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (track < 0 || track >= EngineSnapshot::kMeters) {
		_snap.detail = L"rack insert — bad track";
		return false;
	}
	if (_impl->rack_slot_count[track] >= EngineSnapshot::kMaxInsertSlots) {
		_snap.detail = L"rack insert — full";
		return false;
	}

	/* kind 0: reuse existing ACE helpers by track role; 1: VST3; 2: VST2 */
	bool ok = false;
	if (kind == 0) {
		switch (track % 6) {
		case 0:
			ok = insert_track_fx (track);
			break;
		case 1:
			ok = insert_track_comp (track);
			break;
		case 2:
			ok = insert_track_eq (track);
			break;
		case 3:
			ok = insert_track_delay (track);
			break;
		case 4:
			ok = insert_track_limiter (track);
			break;
		default:
			ok = insert_track_gate (track);
			break;
		}
		if (ok) {
			/* Mirror legacy single insert into rack slot 0 when empty. */
			int slot = _impl->rack_slot_count[track];
			std::shared_ptr<ARDOUR::PluginInsert> pi;
			if (track == 0) {
				pi = _impl->track_fx.lock ();
			} else if (track == 1) {
				pi = _impl->track_comp.lock ();
				if (!pi) {
					pi = _impl->vst3_fx.lock ();
				}
			} else if (track == 2) {
				pi = _impl->track_eq.lock ();
			} else if (track == 3) {
				pi = _impl->track_delay.lock ();
			} else if (track == 4) {
				pi = _impl->track_limiter.lock ();
			} else if (track == 5) {
				pi = _impl->track_gate.lock ();
			}
			if (pi && slot < EngineSnapshot::kMaxInsertSlots) {
				_impl->rack_slots[track][slot] = pi;
				_impl->rack_slot_count[track] = slot + 1;
			}
		}
	} else if (kind == 1) {
		ok = insert_vst3_by_index (catalog_index, track);
		if (ok) {
			int slot = _impl->rack_slot_count[track];
			if (_impl->vst3_fx.lock () && slot < EngineSnapshot::kMaxInsertSlots) {
				_impl->rack_slots[track][slot] = _impl->vst3_fx;
				_impl->rack_slot_count[track] = slot + 1;
			}
		}
	} else if (kind == 2) {
		ok = insert_vst2_by_index (catalog_index, track);
		if (ok) {
			int slot = _impl->rack_slot_count[track];
			if (_impl->vst2_fx.lock () && slot < EngineSnapshot::kMaxInsertSlots) {
				_impl->rack_slots[track][slot] = _impl->vst2_fx;
				_impl->rack_slot_count[track] = slot + 1;
			} else if (_impl->vst2_bridge_fx.lock () && slot < EngineSnapshot::kMaxInsertSlots) {
				/* Bridged 32-bit insert lives as Processor, not PluginInsert. */
				_impl->rack_slot_count[track] = slot + 1;
			}
		}
	}
	refresh_insert_rack_snap ();
	if (ok) {
		_snap.detail = L"rack slot inserted";
	}
	return ok;
}

bool
EngineHost::remove_rack_slot (int track, int slot)
{
	if (!_impl || track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	int n = _impl->rack_slot_count[track];
	if (slot < 0 || slot >= n) {
		return false;
	}
	std::shared_ptr<ARDOUR::PluginInsert> pi = _impl->rack_slots[track][slot].lock ();
	if (pi && _impl->tracks.size () > (size_t)track && _impl->tracks[track]) {
		try {
			_impl->tracks[track]->remove_processor (pi);
		} catch (...) {
		}
	}
	for (int i = slot; i < n - 1; ++i) {
		_impl->rack_slots[track][i] = _impl->rack_slots[track][i + 1];
	}
	_impl->rack_slots[track][n - 1].reset ();
	_impl->rack_slot_count[track] = n - 1;
	refresh_insert_rack_snap ();
	_snap.detail = L"rack slot removed";
	return true;
}

bool
EngineHost::set_rack_slot_bypassed (int track, int slot, bool bypassed)
{
	if (!_impl || track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	if (slot < 0 || slot >= _impl->rack_slot_count[track]) {
		return false;
	}
	std::shared_ptr<ARDOUR::PluginInsert> pi = _impl->rack_slots[track][slot].lock ();
	if (!pi) {
		return false;
	}
	if (bypassed) {
		pi->deactivate ();
	} else {
		pi->activate ();
	}
	refresh_insert_rack_snap ();
	return true;
}

bool
EngineHost::set_rack_slot_param (int track, int slot, int param, float norm_0_1)
{
	if (!_impl || track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	if (slot < 0 || slot >= _impl->rack_slot_count[track]) {
		return false;
	}
	if (param < 0 || param >= EngineSnapshot::kMaxVst3Params) {
		return false;
	}
	std::shared_ptr<ARDOUR::PluginInsert> pi = _impl->rack_slots[track][slot].lock ();
	if (!pi) {
		return false;
	}
	/* Map UI param index onto automatable controls in order. */
	ARDOUR::PluginPtr plug = pi->plugin (0);
	if (!plug) {
		return false;
	}
	int filled = 0;
	uint32_t nports = plug->parameter_count ();
	for (uint32_t port = 0; port < nports; ++port) {
		if (!plug->parameter_is_control (port) || !plug->parameter_is_input (port)) {
			continue;
		}
		std::shared_ptr<ARDOUR::AutomationControl> ac = pi->automation_control (
			Evoral::Parameter (ARDOUR::PluginAutomation, 0, port));
		if (!ac || (ac->flags () & PBD::Controllable::HiddenControl)) {
			continue;
		}
		if (filled == param) {
			try {
				norm_0_1 = std::max (0.f, std::min (1.f, norm_0_1));
				ac->set_value (ac->interface_to_internal (norm_0_1), PBD::Controllable::NoGroup);
				refresh_insert_rack_snap ();
				return true;
			} catch (...) {
				return false;
			}
		}
		++filled;
	}
	return false;
}

bool
EngineHost::insert_master_rack_slot (int kind, int catalog_index)
{
	(void)catalog_index;
	if (!_impl || !_impl->session) {
		return false;
	}
	if (_impl->master_rack_count >= EngineSnapshot::kMaxInsertSlots) {
		return false;
	}
	bool ok = false;
	if (kind == 0) {
		ok = insert_master_fx ();
		if (ok) {
			int slot = _impl->master_rack_count;
			_impl->master_rack[slot] = _impl->master_fx;
			_impl->master_rack_count = slot + 1;
		}
	}
	refresh_insert_rack_snap ();
	return ok;
}

bool
EngineHost::remove_master_rack_slot (int slot)
{
	if (!_impl) {
		return false;
	}
	int n = _impl->master_rack_count;
	if (slot < 0 || slot >= n) {
		return false;
	}
	std::shared_ptr<ARDOUR::PluginInsert> pi = _impl->master_rack[slot].lock ();
	std::shared_ptr<ARDOUR::Route> master = _impl->session ? _impl->session->master_out () : std::shared_ptr<ARDOUR::Route> ();
	if (pi && master) {
		try {
			master->remove_processor (pi);
		} catch (...) {
		}
	}
	for (int i = slot; i < n - 1; ++i) {
		_impl->master_rack[i] = _impl->master_rack[i + 1];
	}
	_impl->master_rack[n - 1].reset ();
	_impl->master_rack_count = n - 1;
	refresh_insert_rack_snap ();
	return true;
}

bool
EngineHost::smoke_fx_chain ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-fx-chain — no Session";
		return false;
	}
	if (!insert_rack_slot (0, 0, 0)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-fx-chain FAILED (insert)\n");
		std::fflush (stderr);
		return false;
	}
	refresh_insert_rack_snap ();
	if (_snap.insert_slot_count[0] < 1 || !_snap.insert_slots[0][0].ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-fx-chain FAILED (snap)\n");
		std::fflush (stderr);
		return false;
	}
	if (!set_rack_slot_bypassed (0, 0, true) || !_snap.insert_slots[0][0].bypassed) {
		refresh_insert_rack_snap ();
	}
	(void)set_rack_slot_bypassed (0, 0, false);
	if (!insert_master_rack_slot (0, 0)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-fx-chain FAILED (master)\n");
		std::fflush (stderr);
		return false;
	}
	std::fprintf (stderr, "airdour_win_shell: smoke-fx-chain ok (slots=%d master=%d)\n",
		      _snap.insert_slot_count[0], _snap.master_insert_count);
	std::fflush (stderr);
	_snap.detail = L"smoke-fx-chain ok";
	return true;
}

#else /* !AIRDOUR_WIN_SHELL_WITH_ENGINE */

void
EngineHost::refresh_insert_rack_snap ()
{
}
bool
EngineHost::insert_rack_slot (int, int, int)
{
	return false;
}
bool
EngineHost::remove_rack_slot (int, int)
{
	return false;
}
bool
EngineHost::set_rack_slot_bypassed (int, int, bool)
{
	return false;
}
bool
EngineHost::set_rack_slot_param (int, int, int, float)
{
	return false;
}
bool
EngineHost::insert_master_rack_slot (int, int)
{
	return false;
}
bool
EngineHost::remove_master_rack_slot (int)
{
	return false;
}
bool
EngineHost::smoke_fx_chain ()
{
	return false;
}

#endif
