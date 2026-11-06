/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Private EngineHost::Impl + helpers shared by engine_host*.cc modules.
 * Only included from win_ardour translation units with AIRDOUR_WIN_SHELL_WITH_ENGINE.
 */
#pragma once

#include "engine_host.h"

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE

#include "ardour/audio_backend.h"
#include "ardour/audio_track.h"
#include "ardour/audioengine.h"
#include "ardour/midi_track.h"
#include "ardour/midi_region.h"
#include "ardour/plugin.h"
#include "ardour/plugin_insert.h"
#include "ardour/plugin_manager.h"
#include "ardour/region.h"
#include "ardour/route.h"
#include "ardour/route_group.h"
#include "ardour/session.h"
#include "ardour/vca.h"
#include "ardour/types.h"
#include "pbd/event_loop.h"
#include "pbd/rwlock.h"
#include "evoral/Note.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

using namespace ARDOUR;
using namespace PBD;

/** Minimal EventLoop so Session signals/butler don't block waiting for GTK. */
class ShellEventLoop : public EventLoop
{
public:
	ShellEventLoop ()
		: EventLoop ("win-shell")
	{
	}

	bool call_slot (InvalidationRecord*, const std::function<void ()>& f) override
	{
		/* UI thread owns this loop — run slots immediately. */
		f ();
		return true;
	}

	PBD::RWLock& slot_invalidation_rwlock () override
	{
		return _lock;
	}

private:
	PBD::RWLock _lock;
};

class EngineHost::Impl
{
public:
	AudioEngine* engine;
	Session* session;
	ShellEventLoop* event_loop;
	bool ardour_inited;
	bool transport_playing;
	samplecnt_t play_origin_samples;
	std::string session_dir;
	std::vector<std::shared_ptr<AudioTrack>> tracks;
	std::shared_ptr<Route> aux_bus;          /* Aux destination for mixer sends */
	std::shared_ptr<RouteGroup> shell_group; /* optional mute/solo/gain link */
	std::shared_ptr<VCA> shell_vca;          /* optional VCA master stub */
	std::shared_ptr<MidiTrack> midi_track;   /* active MIDI track (alias of midi_tracks[active]) */
	std::shared_ptr<MidiRegion> midi_region; /* active MIDI region */
	std::vector<std::shared_ptr<MidiTrack>> midi_tracks;
	std::vector<std::shared_ptr<MidiRegion>> midi_regions;
	int active_midi_track; /* index into midi_tracks */
	std::weak_ptr<PluginInsert> rack_slots[EngineSnapshot::kMeters][EngineSnapshot::kMaxInsertSlots];
	int rack_slot_count[EngineSnapshot::kMeters];
	std::weak_ptr<PluginInsert> master_rack[EngineSnapshot::kMaxInsertSlots];
	int master_rack_count;
	std::weak_ptr<PluginInsert> vst2_fx;     /* shell VST2-64 PluginInsert on Audio 2 */
	std::weak_ptr<Processor> vst2_bridge_fx; /* VST2-32 bridge Processor */
	std::string vst2_bridge_path;
	int vst2_bridge_track; /* audio track index hosting the bridge (−1 none) */
	uint32_t vst2_param_ports[EngineSnapshot::kMaxVst3Params];
	int vst2_param_count;
	bool vst2_bridged_32;
	int selected_track;
	std::weak_ptr<Region> selected_region;
	float midi_activity;
	Evoral::event_id_t selected_midi_note_id; /* primary / last clicked */
	std::set<Evoral::event_id_t> selected_midi_note_ids;
	bool midi_out_connected;
	std::string midi_out_port;
	bool midi_synth_ok;
	bool midi_synth_to_master;
	std::string midi_synth_name;
	bool track_fx_ok;
	bool track_fx_bypassed;
	float track_fx_param; /* Blend 0..1 */
	std::string track_fx_name;
	std::weak_ptr<PluginInsert> track_fx;
	std::weak_ptr<PluginInsert> vst3_fx; /* shell VST3 insert (Audio 2) */
	uint32_t vst3_param_ports[EngineSnapshot::kMaxVst3Params];
	int vst3_param_count;
	int vst3_cycle_index; /* last catalog index inserted via cycle (−1 none) */
	bool track_comp_ok;
	bool track_comp_bypassed;
	float track_comp_threshold_db;
	float track_comp_ratio;
	bool track_comp_sidechain;
	int track_comp_sidechain_src;
	std::string track_comp_name;
	std::weak_ptr<PluginInsert> track_comp;
	bool track_eq_ok;
	bool track_eq_bypassed;
	float track_eq_gain_l_db;
	float track_eq_gain_m_db;
	float track_eq_gain_h_db;
	std::string track_eq_name;
	std::weak_ptr<PluginInsert> track_eq;
	bool track_delay_ok;
	bool track_delay_bypassed;
	float track_delay_time_ms;
	float track_delay_feedback_pct;
	std::string track_delay_name;
	std::weak_ptr<PluginInsert> track_delay;
	bool track_limiter_ok;
	bool track_limiter_bypassed;
	float track_limiter_ceiling_db;
	float track_limiter_release_ms;
	std::string track_limiter_name;
	std::weak_ptr<PluginInsert> track_limiter;
	bool master_fx_ok;
	bool master_fx_bypassed;
	float master_fx_ceiling_db;
	std::string master_fx_name;
	std::weak_ptr<PluginInsert> master_fx;
	bool track_gate_ok;
	bool track_gate_bypassed;
	float track_gate_threshold_db;
	float track_gate_hold_ms;
	std::string track_gate_name;
	std::weak_ptr<PluginInsert> track_gate;
	bool plugins_scanned;
	FadeShape shell_fade_shape;

	Impl ()
		: engine (nullptr)
		, session (nullptr)
		, event_loop (nullptr)
		, ardour_inited (false)
		, transport_playing (false)
		, play_origin_samples (0)
		, shell_fade_shape (FadeLinear)
		, active_midi_track (0)
		, master_rack_count (0)
		, vst2_param_count (0)
		, vst2_bridged_32 (false)
		, vst2_bridge_track (-1)
		, selected_track (-1)
		, midi_activity (0.f)
		, selected_midi_note_id (-1)
		, midi_out_connected (false)
		, midi_synth_ok (false)
		, midi_synth_to_master (false)
		, track_fx_ok (false)
		, track_fx_bypassed (false)
		, track_fx_param (0.3f)
		, vst3_param_count (0)
		, vst3_cycle_index (-1)
		, track_comp_ok (false)
		, track_comp_bypassed (false)
		, track_comp_threshold_db (0.f)
		, track_comp_ratio (4.f)
		, track_comp_sidechain (false)
		, track_comp_sidechain_src (-1)
		, track_eq_ok (false)
		, track_eq_bypassed (false)
		, track_eq_gain_l_db (0.f)
		, track_eq_gain_m_db (0.f)
		, track_eq_gain_h_db (0.f)
		, track_delay_ok (false)
		, track_delay_bypassed (false)
		, track_delay_time_ms (160.f)
		, track_delay_feedback_pct (20.f)
		, track_limiter_ok (false)
		, track_limiter_bypassed (false)
		, track_limiter_ceiling_db (-0.3f)
		, track_limiter_release_ms (100.f)
		, master_fx_ok (false)
		, master_fx_bypassed (false)
		, master_fx_ceiling_db (-0.3f)
		, track_gate_ok (false)
		, track_gate_bypassed (false)
		, track_gate_threshold_db (-40.f)
		, track_gate_hold_ms (50.f)
		, plugins_scanned (false)
	{
		for (int i = 0; i < EngineSnapshot::kMaxVst3Params; ++i) {
			vst3_param_ports[i] = 0;
			vst2_param_ports[i] = 0;
		}
		for (int t = 0; t < EngineSnapshot::kMeters; ++t) {
			rack_slot_count[t] = 0;
		}
	}

	void persist_active_midi_region ()
	{
		if (active_midi_track < 0) {
			return;
		}
		while ((int)midi_regions.size () <= active_midi_track) {
			midi_regions.push_back (std::shared_ptr<MidiRegion> ());
		}
		midi_regions[(size_t)active_midi_track] = midi_region;
	}

	void sync_active_midi_from_index ()
	{
		if (active_midi_track < 0) {
			active_midi_track = 0;
		}
		if (midi_tracks.empty ()) {
			midi_track.reset ();
			midi_region.reset ();
			active_midi_track = 0;
			return;
		}
		if (active_midi_track >= (int)midi_tracks.size ()) {
			active_midi_track = (int)midi_tracks.size () - 1;
		}
		midi_track = midi_tracks[(size_t)active_midi_track];
		if (active_midi_track < (int)midi_regions.size ()) {
			midi_region = midi_regions[(size_t)active_midi_track];
		} else {
			midi_region.reset ();
		}
	}

	void clear_midi_note_sel ()
	{
		selected_midi_note_ids.clear ();
		selected_midi_note_id = -1;
	}

	void set_midi_note_sel (Evoral::event_id_t id)
	{
		selected_midi_note_ids.clear ();
		if (id >= 0) {
			selected_midi_note_ids.insert (id);
		}
		selected_midi_note_id = id;
	}

	void toggle_midi_note_sel (Evoral::event_id_t id)
	{
		if (id < 0) {
			return;
		}
		/* Seed from primary when first additive click after a legacy single. */
		if (selected_midi_note_ids.empty () && selected_midi_note_id >= 0) {
			selected_midi_note_ids.insert (selected_midi_note_id);
		}
		std::set<Evoral::event_id_t>::iterator it = selected_midi_note_ids.find (id);
		if (it != selected_midi_note_ids.end ()) {
			selected_midi_note_ids.erase (it);
			if (selected_midi_note_id == id) {
				selected_midi_note_id = selected_midi_note_ids.empty ()
								? -1
								: *selected_midi_note_ids.rbegin ();
			}
		} else {
			selected_midi_note_ids.insert (id);
			selected_midi_note_id = id;
		}
	}

	bool midi_note_sel_has (Evoral::event_id_t id) const
	{
		if (id < 0) {
			return false;
		}
		if (!selected_midi_note_ids.empty ()) {
			return selected_midi_note_ids.count (id) > 0;
		}
		return selected_midi_note_id >= 0 && selected_midi_note_id == id;
	}
};

#endif /* AIRDOUR_WIN_SHELL_WITH_ENGINE */
