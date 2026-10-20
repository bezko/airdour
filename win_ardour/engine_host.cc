/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "engine_host.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE

#include "ardour/ardour.h"
#include "ardour/filesystem_paths.h"
#include "ardour/audio_backend.h"
#include "ardour/audio_track.h"
#include "ardour/audioengine.h"
#include "ardour/audioregion.h"
#include "ardour/audiosource.h"
#include "ardour/export_channel.h"
#include "ardour/export_channel_configuration.h"
#include "ardour/export_filename.h"
#include "ardour/export_format_specification.h"
#include "ardour/export_handler.h"
#include "ardour/export_status.h"
#include "ardour/export_timespan.h"
#include "ardour/io.h"
#include "ardour/meter.h"
#include "ardour/track.h"
#include "ardour/playlist.h"
#include "ardour/audioplaylist.h"
#include "ardour/session_directory.h"
#include "ardour/presentation_info.h"
#include "ardour/route_group.h"
#include "ardour/monitor_processor.h"
#include "ardour/rc_configuration.h"
#include "ardour/region.h"
#include "ardour/region_factory.h"
#include "ardour/route.h"
#include "ardour/internal_send.h"
#include "ardour/send.h"
#include "ardour/audio_backend.h"
#include "ardour/chan_count.h"
#include "ardour/location.h"
#include "ardour/midi_model.h"
#include "ardour/midi_region.h"
#include "ardour/midi_source.h"
#include "ardour/midi_track.h"
#include "ardour/automation_control.h"
#include "ardour/automation_list.h"
#include "ardour/mute_control.h"
#include "ardour/solo_control.h"
#include "ardour/gain_control.h"
#include "ardour/plugin.h"
#include "ardour/plugin_insert.h"
#include "ardour/plugin_manager.h"
#include "ardour/port.h"
#include "ardour/processor.h"
#include "ardour/session.h"
#include "ardour/session_event.h"
#include "ardour/audiofilesource.h"
#include "ardour/sndfilesource.h"
#include "ardour/source_factory.h"
#include "ardour/dB.h"
#include "ardour/interthread_info.h"
#include "ardour/pitch.h"
#include "ardour/reverse.h"
#include "ardour/strip_silence.h"
#include "ardour/stretch.h"
#include "ardour/timefx_request.h"
#include "ardour/types.h"
#include "pbd/progress.h"
#include "evoral/Note.h"
#include "evoral/Parameter.h"
#include "pbd/compose.h"
#include "pbd/controllable.h"
#include "pbd/error.h"
#include "pbd/event_loop.h"
#include "pbd/failed_constructor.h"
#include "pbd/memento_command.h"
#include "pbd/mutex.h"
#include "pbd/property_list.h"
#include "pbd/rwlock.h"
#include "pbd/stateful_diff_command.h"

#include <glib.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include "pbd/xml++.h"
#include "temporal/beats.h"
#include "temporal/bbt_time.h"
#include "temporal/tempo.h"
#include "temporal/time.h"
#include "temporal/types.h"

#ifdef interface
#undef interface
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <shlobj.h>

#pragma comment(lib, "psapi.lib")

using namespace ARDOUR;
using namespace PBD;

namespace {

std::wstring
utf8_to_wide (const std::string& s)
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

std::string
wide_to_utf8 (const std::wstring& s)
{
	if (s.empty ()) {
		return std::string ();
	}
	int n = WideCharToMultiByte (CP_UTF8, 0, s.c_str (), (int)s.size (), nullptr, 0, nullptr, nullptr);
	if (n <= 0) {
		return std::string ();
	}
	std::string out ((size_t)n, '\0');
	WideCharToMultiByte (CP_UTF8, 0, s.c_str (), (int)s.size (), &out[0], n, nullptr, nullptr);
	return out;
}

void
copy_name_to_snap (wchar_t* dest, size_t dest_chars, const std::string& utf8)
{
	std::wstring w = utf8_to_wide (utf8);
	if (w.empty ()) {
		wcsncpy_s (dest, dest_chars, L"?", _TRUNCATE);
	} else {
		wcsncpy_s (dest, dest_chars, w.c_str (), _TRUNCATE);
	}
}

std::string
resolve_backend_name (const std::string& requested)
{
	if (requested.empty () || requested == "Dummy" || requested == "dummy") {
		return "None (Dummy)";
	}
	return requested;
}

float
db_to_linear (float db)
{
	if (!std::isfinite (db) || db < -90.f) {
		return 0.f;
	}
	float lin = std::pow (10.f, db / 20.f);
	return std::max (0.f, std::min (1.f, lin));
}

float
peak_linear_from_meter (std::shared_ptr<PeakMeter> m)
{
	if (!m) {
		return 0.f;
	}
	/* MeterPeak returns dB with falloff. */
	float db = m->meter_level (0, MeterPeak);
	return db_to_linear (db);
}

bool
clip_from_meter (std::shared_ptr<PeakMeter> m)
{
	if (!m) {
		return false;
	}
	/* Sticky max peak (until reset_max) — classic clip latch. */
	float max_db = m->meter_level (0, MeterMaxPeak);
	if (std::isfinite (max_db) && max_db >= 0.f) {
		return true;
	}
	float peak_db = m->meter_level (0, MeterPeak);
	return std::isfinite (peak_db) && peak_db >= 0.f;
}

std::string
win_temp_dir ()
{
	wchar_t wbuf[MAX_PATH];
	DWORD n = GetTempPathW (MAX_PATH, wbuf);
	if (n == 0 || n >= MAX_PATH) {
		return "C:\\Temp";
	}
	char buf[MAX_PATH * 2];
	WideCharToMultiByte (CP_UTF8, 0, wbuf, -1, buf, sizeof (buf), nullptr, nullptr);
	std::string s (buf);
	while (!s.empty () && (s.back () == '\\' || s.back () == '/')) {
		s.pop_back ();
	}
	return s;
}

std::string
path_join (const std::string& a, const std::string& b)
{
	if (a.empty ()) {
		return b;
	}
	char sep = '\\';
	if (a.back () == '\\' || a.back () == '/') {
		return a + b;
	}
	return a + sep + b;
}

std::wstring
utf8_path_to_wide (const std::string& s)
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

/** Minimal mono 16-bit PCM WAV (no libsndfile) for import smoke. */
bool
write_pcm16_wav (const std::string& path_utf8, int sample_rate, int n_samples, float freq_hz, float amp)
{
	std::wstring wpath = utf8_path_to_wide (path_utf8);
	if (wpath.empty ()) {
		return false;
	}
	FILE* f = _wfopen (wpath.c_str (), L"wb");
	if (!f) {
		return false;
	}

	const int channels = 1;
	const int bits = 16;
	const int byte_rate = sample_rate * channels * (bits / 8);
	const int block_align = channels * (bits / 8);
	const uint32_t data_bytes = (uint32_t)n_samples * (uint32_t)block_align;
	const uint32_t riff_size = 36u + data_bytes;

	auto wr = [&] (const void* p, size_t n) {
		return fwrite (p, 1, n, f) == n;
	};
	auto wr32 = [&] (uint32_t v) {
		unsigned char b[4] = {
			(unsigned char)(v & 0xff),
			(unsigned char)((v >> 8) & 0xff),
			(unsigned char)((v >> 16) & 0xff),
			(unsigned char)((v >> 24) & 0xff)
		};
		return wr (b, 4);
	};
	auto wr16 = [&] (uint16_t v) {
		unsigned char b[2] = {
			(unsigned char)(v & 0xff),
			(unsigned char)((v >> 8) & 0xff)
		};
		return wr (b, 2);
	};

	bool ok = wr ("RIFF", 4) && wr32 (riff_size) && wr ("WAVE", 4)
	       && wr ("fmt ", 4) && wr32 (16) && wr16 (1) && wr16 ((uint16_t)channels)
	       && wr32 ((uint32_t)sample_rate) && wr32 ((uint32_t)byte_rate)
	       && wr16 ((uint16_t)block_align) && wr16 ((uint16_t)bits)
	       && wr ("data", 4) && wr32 (data_bytes);

	if (ok) {
		for (int i = 0; i < n_samples; ++i) {
			float ph = 6.2831853f * freq_hz * ((float)i / (float)sample_rate);
			float beat = std::fmod ((float)i / (float)sample_rate * 2.f, 1.f);
			float env = 0.55f + 0.45f * std::exp (-beat * 8.f);
			float s = amp * env * std::sin (ph);
			if (s > 1.f) {
				s = 1.f;
			} else if (s < -1.f) {
				s = -1.f;
			}
			int16_t sample = (int16_t)std::lrintf (s * 32767.f);
			unsigned char b[2] = {
				(unsigned char)(sample & 0xff),
				(unsigned char)((sample >> 8) & 0xff)
			};
			if (!wr (b, 2)) {
				ok = false;
				break;
			}
		}
	}
	fclose (f);
	return ok;
}

/** PCM16 mono WAV with silence / tone / silence / tone (for strip-silence smoke). */
bool
write_pcm16_wav_gated (const std::string& path_utf8, int sample_rate,
                       float freq_hz, float amp,
                       float silence_sec, float tone_sec)
{
	if (silence_sec < 0.05f) {
		silence_sec = 0.05f;
	}
	if (tone_sec < 0.1f) {
		tone_sec = 0.1f;
	}
	const int n_sil = (int)std::llround ((double)sample_rate * (double)silence_sec);
	const int n_tone = (int)std::llround ((double)sample_rate * (double)tone_sec);
	const int n_samples = n_sil + n_tone + n_sil + n_tone;
	std::wstring wpath = utf8_path_to_wide (path_utf8);
	if (wpath.empty ()) {
		return false;
	}
	FILE* f = _wfopen (wpath.c_str (), L"wb");
	if (!f) {
		return false;
	}

	const int channels = 1;
	const int bits = 16;
	const int byte_rate = sample_rate * channels * (bits / 8);
	const int block_align = channels * (bits / 8);
	const uint32_t data_bytes = (uint32_t)n_samples * (uint32_t)block_align;
	const uint32_t riff_size = 36u + data_bytes;

	auto wr = [&] (const void* p, size_t n) {
		return fwrite (p, 1, n, f) == n;
	};
	auto wr32 = [&] (uint32_t v) {
		unsigned char b[4] = {
			(unsigned char)(v & 0xff),
			(unsigned char)((v >> 8) & 0xff),
			(unsigned char)((v >> 16) & 0xff),
			(unsigned char)((v >> 24) & 0xff)
		};
		return wr (b, 4);
	};
	auto wr16 = [&] (uint16_t v) {
		unsigned char b[2] = {
			(unsigned char)(v & 0xff),
			(unsigned char)((v >> 8) & 0xff)
		};
		return wr (b, 2);
	};

	bool ok = wr ("RIFF", 4) && wr32 (riff_size) && wr ("WAVE", 4)
	       && wr ("fmt ", 4) && wr32 (16) && wr16 (1) && wr16 ((uint16_t)channels)
	       && wr32 ((uint32_t)sample_rate) && wr32 ((uint32_t)byte_rate)
	       && wr16 ((uint16_t)block_align) && wr16 ((uint16_t)bits)
	       && wr ("data", 4) && wr32 (data_bytes);

	auto sample_at = [&] (int i) -> int16_t {
		bool in_tone = false;
		int tone_i = 0;
		if (i >= n_sil && i < n_sil + n_tone) {
			in_tone = true;
			tone_i = i - n_sil;
		} else if (i >= n_sil + n_tone + n_sil) {
			in_tone = true;
			tone_i = i - (n_sil + n_tone + n_sil);
		}
		if (!in_tone) {
			return 0;
		}
		float ph = 6.2831853f * freq_hz * ((float)tone_i / (float)sample_rate);
		float s = amp * std::sin (ph);
		if (s > 1.f) {
			s = 1.f;
		} else if (s < -1.f) {
			s = -1.f;
		}
		return (int16_t)std::lrintf (s * 32767.f);
	};

	if (ok) {
		for (int i = 0; i < n_samples; ++i) {
			int16_t sample = sample_at (i);
			unsigned char b[2] = {
				(unsigned char)(sample & 0xff),
				(unsigned char)((sample >> 8) & 0xff)
			};
			if (!wr (b, 2)) {
				ok = false;
				break;
			}
		}
	}
	fclose (f);
	return ok;
}

} // namespace

/** Minimal EventLoop so Session signals/butler don't block waiting for GTK. */
class ShellEventLoop : public EventLoop
{
public:
	ShellEventLoop ()
		: EventLoop ("win-shell")
	{
	}

	bool call_slot (InvalidationRecord*, const std::function<void()>& f) override
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
	AudioEngine*    engine;
	Session*        session;
	ShellEventLoop* event_loop;
	bool            ardour_inited;
	bool            transport_playing;
	samplecnt_t     play_origin_samples;
	std::string     session_dir;
	std::vector<std::shared_ptr<AudioTrack> > tracks;
	std::shared_ptr<Route> aux_bus; /* Aux destination for mixer sends */
	std::shared_ptr<RouteGroup> shell_group; /* optional mute/solo/gain link */
	std::shared_ptr<MidiTrack> midi_track;
	std::shared_ptr<MidiRegion> midi_region;
	int             selected_track;
	std::weak_ptr<Region> selected_region;
	float           midi_activity;
	Evoral::event_id_t selected_midi_note_id;
	bool            midi_out_connected;
	std::string     midi_out_port;
	bool            midi_synth_ok;
	bool            midi_synth_to_master;
	std::string     midi_synth_name;
	bool            track_fx_ok;
	bool            track_fx_bypassed;
	float           track_fx_param; /* Blend 0..1 */
	std::string     track_fx_name;
	std::weak_ptr<PluginInsert> track_fx;
	bool            track_comp_ok;
	bool            track_comp_bypassed;
	float           track_comp_threshold_db;
	float           track_comp_ratio;
	bool            track_comp_sidechain;
	int             track_comp_sidechain_src;
	std::string     track_comp_name;
	std::weak_ptr<PluginInsert> track_comp;
	bool            track_eq_ok;
	bool            track_eq_bypassed;
	float           track_eq_gain_l_db;
	float           track_eq_gain_m_db;
	float           track_eq_gain_h_db;
	std::string     track_eq_name;
	std::weak_ptr<PluginInsert> track_eq;
	bool            track_delay_ok;
	bool            track_delay_bypassed;
	float           track_delay_time_ms;
	float           track_delay_feedback_pct;
	std::string     track_delay_name;
	std::weak_ptr<PluginInsert> track_delay;
	bool            track_limiter_ok;
	bool            track_limiter_bypassed;
	float           track_limiter_ceiling_db;
	float           track_limiter_release_ms;
	std::string     track_limiter_name;
	std::weak_ptr<PluginInsert> track_limiter;
	bool            track_gate_ok;
	bool            track_gate_bypassed;
	float           track_gate_threshold_db;
	float           track_gate_hold_ms;
	std::string     track_gate_name;
	std::weak_ptr<PluginInsert> track_gate;
	bool            plugins_scanned;

	Impl ()
		: engine (nullptr)
		, session (nullptr)
		, event_loop (nullptr)
		, ardour_inited (false)
		, transport_playing (false)
		, play_origin_samples (0)
		, selected_track (-1)
		, midi_activity (0.f)
		, selected_midi_note_id (-1)
		, midi_out_connected (false)
		, midi_synth_ok (false)
		, midi_synth_to_master (false)
		, track_fx_ok (false)
		, track_fx_bypassed (false)
		, track_fx_param (0.3f)
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
		, track_gate_ok (false)
		, track_gate_bypassed (false)
		, track_gate_threshold_db (-40.f)
		, track_gate_hold_ms (50.f)
		, plugins_scanned (false)
	{
	}
};

EngineHost::EngineHost ()
	: _started (false)
	, _backend_name () /* empty → resolve from WinShellPrefs / Dummy in start() */
	, _period_frames (0)
	, _pref_backend ("Dummy")
	, _pref_period_frames (0)
	, _pref_snap_default (false)
	, _pref_click_default (true)
	, _autosave_enabled (true)
	, _last_autosave_ms (0)
	, _recovery_saved_ft (0)
	, _recovery_last_open_ft (0)
	, _recovery_dirty (false)
	, _gain (0.85f)
	, _arp_enabled (false)
	, _arp_mode (0)
	, _arp_step (0)
	, _arp_last_grid (-1)
	, _arp_sounding (-1)
	, _arp_pitch_count (0)
	, _arp_held_count (0)
	, _impl (new Impl)
{
	for (int i = 0; i < kMaxArpPitches; ++i) {
		_arp_pitches[i] = 0;
		_arp_held[i] = 0;
	}
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_track_gain[i] = (i == 0) ? 0.85f : (0.85f - 0.12f * (float)i);
		_snap.meter_peak[i] = 0.f;
		_snap.meter_clip[i] = false;
		_snap.track_has_regions[i] = false;
		_snap.track_armed[i] = false;
		_snap.track_frozen[i] = false;
		_snap.track_muted[i] = false;
		_snap.track_soloed[i] = false;
		_snap.track_pan[i] = 0.5f;
		_snap.track_send[i] = 0.f;
		_snap.track_send_ok[i] = false;
		_snap.track_gain[i] = _track_gain[i];
		_snap.track_name[i][0] = L'\0';
		_snap.track_in_group[i] = false;
	}
	_snap.midi_track_name[0] = L'\0';
	_snap.group_member_count = 0;
	_snap.group_active = false;
	_snap.group_shares_mute = false;
	_snap.group_shares_solo = false;
	_snap.group_shares_gain = false;
	_snap.group_name[0] = L'\0';
	_snap.monitor_ok = false;
	_snap.monitor_dim = false;
	_snap.monitor_mono = false;
	_snap.monitor_cut = false;
	_snap.transport_speed = 0.f;
	_snap.bbt_bars = 1;
	_snap.bbt_beats = 1;
	_snap.bbt_ticks = 0;
	_snap.tc_hours = 0;
	_snap.tc_minutes = 0;
	_snap.tc_seconds = 0;
	_snap.tc_frames = 0;
	_snap.pref_backend = L"Dummy";
	_snap.pref_period_frames = 0;
	_snap.pref_snap_default = false;
	_snap.pref_click_default = true; /* shell default until RC / prefs load */
	_snap.vst3_path_hint[0] = L'\0';
	_snap.recent_count = 0;
	for (int i = 0; i < EngineSnapshot::kMaxRecent; ++i) {
		_snap.recent_label[i][0] = L'\0';
		_snap.recent_path[i][0] = L'\0';
	}
	_snap.aux_bus_ok = false;
	_snap.midi_muted = false;
	_snap.midi_soloed = false;
	_snap.snap_enabled = false;
	_snap.midi_step_enabled = false;
	_snap.midi_chord_quality = 0;
	_snap.midi_arp_enabled = false;
	_snap.midi_arp_mode = 0;
	_snap.midi_arp_pitch = -1;
	_snap.midi_scale_lock = false;
	_snap.midi_scale_kind = 0;
	_snap.midi_scale_root = 0;
	_snap.snap_density = 1; /* beat */
	_snap.grid_line_count = 0;
	for (int i = 0; i < EngineSnapshot::kMaxGridLines; ++i) {
		_snap.grid_line_sec[i] = 0.f;
		_snap.grid_line_bar[i] = false;
	}
	_snap.available = true;
	_snap.running = false;
	_snap.session_ok = false;
	_snap.transport_rolling = false;
	_snap.meters_real = false;
	_snap.waves_from_session = false;
	_snap.session_record_armed = false;
	_snap.session_recording = false;
	_snap.session_saved = false;
	_snap.export_ok = false;
	_snap.undo_depth = 0;
	_snap.redo_depth = 0;
	_snap.undo_list_count = 0;
	_snap.next_undo_name[0] = L'\0';
	_snap.next_redo_name[0] = L'\0';
	for (int i = 0; i < EngineSnapshot::kMaxHistory; ++i) {
		_snap.undo_list_name[i][0] = L'\0';
	}
	_snap.has_selection = false;
	_snap.selection_track = -1;
	_snap.selection_start_sec = 0.f;
	_snap.selection_end_sec = 0.f;
	_snap.selection_fade_in_sec = 0.f;
	_snap.selection_fade_out_sec = 0.f;
	_snap.loop_active = false;
	_snap.loop_playing = false;
	_snap.loop_start_sec = 0.f;
	_snap.loop_end_sec = 0.f;
	_snap.punch_range_ok = false;
	_snap.punch_enabled = false;
	_snap.punch_start_sec = 0.f;
	_snap.punch_end_sec = 0.f;
	_snap.tempo_bpm = 120.f;
	_snap.count_in_enabled = false;
	_snap.click_enabled = false;
	_snap.audio_track_count = 0;
	_snap.midi_track_count = 0;
	_snap.marker_count = 0;
	for (int i = 0; i < EngineSnapshot::kMaxMarkers; ++i) {
		_snap.markers[i].sec = 0.f;
		_snap.markers[i].name[0] = L'\0';
	}
	_snap.midi_ok = false;
	_snap.midi_track_ok = false;
	_snap.midi_region_ok = false;
	_snap.midi_synth_ok = false;
	_snap.midi_synth_to_master = false;
	_snap.midi_out_connected = false;
	_snap.track_fx_ok = false;
	_snap.track_fx_bypassed = false;
	_snap.track_fx_param = 0.3f;
	_snap.track_comp_ok = false;
	_snap.track_comp_bypassed = false;
	_snap.track_comp_threshold_db = 0.f;
	_snap.track_comp_ratio = 4.f;
	_snap.track_comp_threshold_norm = 1.f;
	_snap.track_comp_ratio_norm = (4.f - 1.f) / 19.f;
	_snap.track_comp_sidechain = false;
	_snap.track_comp_sidechain_src = -1;
	_snap.track_eq_ok = false;
	_snap.track_eq_bypassed = false;
	_snap.track_eq_gain_l_db = 0.f;
	_snap.track_eq_gain_m_db = 0.f;
	_snap.track_eq_gain_h_db = 0.f;
	_snap.track_eq_gain_l_norm = 0.5f;
	_snap.track_eq_gain_m_norm = 0.5f;
	_snap.track_eq_gain_h_norm = 0.5f;
	_snap.track_delay_ok = false;
	_snap.track_delay_bypassed = false;
	_snap.track_delay_time_ms = 160.f;
	_snap.track_delay_feedback_pct = 20.f;
	_snap.track_delay_time_norm = std::log (160.f) / std::log (8000.f);
	_snap.track_delay_feedback_norm = 0.2f;
	_snap.track_limiter_ok = false;
	_snap.track_limiter_bypassed = false;
	_snap.track_limiter_ceiling_db = -0.3f;
	_snap.track_limiter_release_ms = 100.f;
	_snap.track_limiter_ceiling_norm = (-0.3f - (-20.f)) / 20.f;
	_snap.track_limiter_release_norm = (100.f - 1.f) / 1999.f;
	_snap.track_gate_ok = false;
	_snap.track_gate_bypassed = false;
	_snap.track_gate_threshold_db = -40.f;
	_snap.track_gate_hold_ms = 50.f;
	_snap.track_gate_threshold_norm = (-40.f - (-60.f)) / 60.f;
	_snap.track_gate_hold_norm = (50.f - 1.f) / 499.f;
	_snap.spectrum_enabled = false;
	_snap.spectrum_ok = false;
	_snap.spectrum_master = false;
	_snap.spectrum_track = 0;
	for (int si = 0; si < EngineSnapshot::kSpectrumBands; ++si) {
		_snap.spectrum_band[si] = 0.f;
	}
	_snap.spectrum_label[0] = L'\0';
	_snap.correlation_enabled = false;
	_snap.correlation_ok = false;
	_snap.correlation_master = false;
	_snap.correlation_track = 0;
	_snap.correlation = 0.f;
	for (int gi = 0; gi < EngineSnapshot::kGonioPts; ++gi) {
		_snap.gonio_x[gi] = 0.f;
		_snap.gonio_y[gi] = 0.f;
	}
	_snap.correlation_label[0] = L'\0';
	_snap.loudness_enabled = false;
	_snap.loudness_ok = false;
	_snap.loudness_master = false;
	_snap.loudness_track = 0;
	_snap.loudness_momentary = -70.f;
	_snap.loudness_integrated = -70.f;
	_snap.loudness_label[0] = L'\0';
	_snap.midi_activity = 0.f;
	_snap.dsp_load_pct = 0.f;
	_snap.callback_last_us = 0;
	_snap.callback_max_us = 0;
	_snap.callback_avg_us = 0;
	_snap.callback_count = 0;
	_snap.xrun_count = 0;
	_snap.midi_device_count = 0;
	for (int i = 0; i < EngineSnapshot::kMaxMidiDevices; ++i) {
		_snap.midi_devices[i].name[0] = L'\0';
		_snap.midi_devices[i].input = false;
	}
	_snap.midi_note_count = 0;
	for (int i = 0; i < EngineSnapshot::kMaxMidiNotes; ++i) {
		_snap.midi_notes[i].id = -1;
		_snap.midi_notes[i].start_sec = 0.f;
		_snap.midi_notes[i].length_sec = 0.f;
		_snap.midi_notes[i].pitch = 0;
		_snap.midi_notes[i].velocity = 0;
		_snap.midi_notes[i].selected = false;
	}
	_snap.midi_selected_note_id = -1;
	_snap.midi_option.clear ();
	_snap.midi_synth_name.clear ();
	_snap.midi_out_name.clear ();
	_snap.track_fx_name.clear ();
	_snap.track_fx_param_name = L"Blend";
	_snap.track_comp_name.clear ();
	_snap.track_eq_name.clear ();
	_snap.track_delay_name.clear ();
	_snap.track_limiter_name.clear ();
	_snap.track_gate_name.clear ();
	_snap.sample_rate = 0;
	_snap.period_frames = 0;
	_snap.gain = _gain;
	_snap.transport_sec = 0.f;
	_snap.backend = L"(idle)";
	_snap.session_path.clear ();
	_snap.export_path.clear ();
	_snap.detail = L"engine linked — not started";
	_last_export_path.clear ();
}

EngineHost::~EngineHost ()
{
	stop ();
	delete _impl;
	_impl = nullptr;
}

bool
EngineHost::is_linked ()
{
	return true;
}

void
EngineHost::prepare (const std::string& backend_name)
{
	if (!backend_name.empty ()) {
		_backend_name = backend_name;
	}
	_snap.backend = utf8_to_wide (resolve_backend_name (
		_backend_name.empty () ? (_pref_backend.empty () ? "Dummy" : _pref_backend)
		                       : _backend_name));
	_snap.detail = L"prepared — press Play to start";
}

void
EngineHost::set_period_frames (uint32_t frames)
{
	_period_frames = frames;
}

void
EngineHost::refresh_prefs_snap ()
{
	_snap.pref_backend = utf8_to_wide (_pref_backend.empty () ? "Dummy" : _pref_backend);
	_snap.pref_period_frames = _pref_period_frames;
	_snap.pref_snap_default = _pref_snap_default;
	_snap.pref_click_default = _pref_click_default;

	std::string hint;
	const char* env = std::getenv ("VST3_PATH");
	if (env && env[0]) {
		hint = env;
	} else if (_impl && _impl->ardour_inited && Config) {
		hint = Config->get_plugin_path_vst3 ();
	}
	if (hint.empty ()) {
		_snap.vst3_path_hint[0] = L'\0';
	} else {
		copy_name_to_snap (_snap.vst3_path_hint, EngineSnapshot::kPrefsHintLen, hint);
	}
}

void
EngineHost::load_shell_prefs ()
{
	if (!_impl || !_impl->ardour_inited || !Config) {
		refresh_prefs_snap ();
		return;
	}
	try {
		_pref_click_default = Config->get_clicking ();
		XMLNode* node = Config->instant_xml ("WinShellPrefs");
		if (node) {
			std::string be;
			if (node->get_property ("backend", be) && !be.empty ()) {
				if (be == "WASAPI" || be == "wasapi") {
					_pref_backend = "WASAPI";
				} else {
					_pref_backend = "Dummy";
				}
			}
			uint32_t period = 0;
			if (node->get_property ("period", period)) {
				_pref_period_frames = period;
			}
			int snap = 0;
			if (node->get_property ("snap-default", snap)) {
				_pref_snap_default = (snap != 0);
			}
			int click = 0;
			if (node->get_property ("click-default", click)) {
				_pref_click_default = (click != 0);
			}
		}
		/* Keep RC Config clicking aligned with the shell preference. */
		Config->set_clicking (_pref_click_default);
		refresh_prefs_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: prefs loaded (backend=%s period=%u snap=%d click=%d)\n",
		              _pref_backend.c_str (), (unsigned)_pref_period_frames,
		              _pref_snap_default ? 1 : 0, _pref_click_default ? 1 : 0);
		std::fflush (stderr);
	} catch (...) {
		refresh_prefs_snap ();
	}
}

bool
EngineHost::save_shell_prefs ()
{
	if (!_impl || !_impl->ardour_inited || !Config) {
		_snap.detail = L"prefs save failed — Config not ready";
		return false;
	}
	try {
		Config->set_clicking (_pref_click_default);
		if (Config->save_state () != 0) {
			_snap.detail = L"prefs: Config::save_state failed";
			std::fprintf (stderr, "airdour_win_shell: Config::save_state failed\n");
			std::fflush (stderr);
			return false;
		}
		XMLNode node ("WinShellPrefs");
		node.set_property ("backend", _pref_backend.empty () ? "Dummy" : _pref_backend);
		node.set_property ("period", _pref_period_frames);
		node.set_property ("snap-default", _pref_snap_default ? 1 : 0);
		node.set_property ("click-default", _pref_click_default ? 1 : 0);
		Config->add_instant_xml (node);
		refresh_prefs_snap ();
		_snap.detail = L"preferences saved (RC + instant.xml)";
		std::fprintf (stderr,
		              "airdour_win_shell: prefs saved (backend=%s period=%u snap=%d click=%d)\n",
		              _pref_backend.c_str (), (unsigned)_pref_period_frames,
		              _pref_snap_default ? 1 : 0, _pref_click_default ? 1 : 0);
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		_snap.detail = L"prefs save exception";
		std::fprintf (stderr, "airdour_win_shell: prefs save exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		_snap.detail = L"prefs save failed";
		return false;
	}
}

void
EngineHost::set_pref_backend (const std::string& name)
{
	if (name == "WASAPI" || name == "wasapi") {
		_pref_backend = "WASAPI";
	} else {
		_pref_backend = "Dummy";
	}
	refresh_prefs_snap ();
}

void
EngineHost::set_pref_period_frames (uint32_t frames)
{
	_pref_period_frames = frames;
	refresh_prefs_snap ();
}

void
EngineHost::set_pref_snap_default (bool on)
{
	_pref_snap_default = on;
	if (_impl && _impl->session) {
		set_snap_enabled (on);
	} else {
		_snap.snap_enabled = on;
	}
	refresh_prefs_snap ();
}

bool
EngineHost::set_pref_click_default (bool on)
{
	_pref_click_default = on;
	refresh_prefs_snap ();
	if (_impl && _impl->session) {
		return set_clicking (on);
	}
	if (_impl && _impl->ardour_inited && Config) {
		Config->set_clicking (on);
	}
	return true;
}

bool
EngineHost::smoke_prefs ()
{
	if (!_impl || !_impl->ardour_inited || !Config) {
		_snap.detail = L"smoke-prefs failed — Config not ready";
		return false;
	}
	try {
		const std::string be0 = _pref_backend;
		const uint32_t period0 = _pref_period_frames;
		const bool snap0 = _pref_snap_default;
		const bool click0 = _pref_click_default;

		set_pref_backend ("WASAPI");
		set_pref_period_frames (512);
		set_pref_snap_default (true);
		if (!set_pref_click_default (false)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-prefs FAILED (click off)\n");
			std::fflush (stderr);
			return false;
		}
		if (!save_shell_prefs ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-prefs FAILED (save)\n");
			std::fflush (stderr);
			return false;
		}

		/* Clobber in-memory prefs only (do not touch Config) then reload from disk. */
		_pref_backend = "Dummy";
		_pref_period_frames = 0;
		_pref_snap_default = false;
		_pref_click_default = true;
		load_shell_prefs ();

		if (_pref_backend != "WASAPI" || _pref_period_frames != 512 || !_pref_snap_default
		    || _pref_click_default || Config->get_clicking ()) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-prefs FAILED (reload be=%s period=%u snap=%d click=%d cfg=%d)\n",
			              _pref_backend.c_str (), (unsigned)_pref_period_frames,
			              _pref_snap_default ? 1 : 0, _pref_click_default ? 1 : 0,
			              Config->get_clicking () ? 1 : 0);
			std::fflush (stderr);
			return false;
		}

		set_pref_backend (be0);
		set_pref_period_frames (period0);
		set_pref_snap_default (snap0);
		set_pref_click_default (click0);
		if (!save_shell_prefs ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-prefs FAILED (restore save)\n");
			std::fflush (stderr);
			return false;
		}

		std::fprintf (stderr, "airdour_win_shell: smoke-prefs ok (WASAPI/512/snap/click round-trip)\n");
		std::fflush (stderr);
		_snap.detail = L"smoke-prefs ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-prefs exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

namespace {

std::string
path_basename_utf8 (const std::string& path)
{
	if (path.empty ()) {
		return std::string ();
	}
	size_t slash = path.find_last_of ("/\\");
	if (slash == std::string::npos) {
		return path;
	}
	if (slash + 1 >= path.size ()) {
		/* trailing slash — take previous component */
		size_t end = slash;
		while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\')) {
			--end;
		}
		size_t prev = path.find_last_of ("/\\", end ? end - 1 : 0);
		if (prev == std::string::npos) {
			return path.substr (0, end);
		}
		return path.substr (prev + 1, end - prev - 1);
	}
	return path.substr (slash + 1);
}

} // namespace

void
EngineHost::refresh_recent_snap ()
{
	_snap.recent_count = (int)std::min (_recent.size (), (size_t)EngineSnapshot::kMaxRecent);
	for (int i = 0; i < EngineSnapshot::kMaxRecent; ++i) {
		if (i < _snap.recent_count) {
			std::string base = path_basename_utf8 (_recent[i].path);
			if (base.empty ()) {
				base = _recent[i].path;
			}
			copy_name_to_snap (_snap.recent_label[i], EngineSnapshot::kRecentLabelLen, base);
			copy_name_to_snap (_snap.recent_path[i], EngineSnapshot::kRecentPathLen, _recent[i].path);
		} else {
			_snap.recent_label[i][0] = L'\0';
			_snap.recent_path[i][0] = L'\0';
		}
	}
}

void
EngineHost::load_recent_sessions ()
{
	if (!_impl || !_impl->ardour_inited || !Config) {
		refresh_recent_snap ();
		return;
	}
	try {
		_recent.clear ();
		XMLNode* root = Config->instant_xml ("WinShellRecent");
		if (root) {
			const XMLNodeList& kids = root->children ();
			for (XMLNodeList::const_iterator it = kids.begin ();
			     it != kids.end () && _recent.size () < (size_t)EngineSnapshot::kMaxRecent;
			     ++it) {
				XMLNode* n = *it;
				if (!n || n->name () != "Session") {
					continue;
				}
				std::string path;
				std::string snap = "win_shell";
				if (!n->get_property ("path", path) || path.empty ()) {
					continue;
				}
				n->get_property ("snapshot", snap);
				if (snap.empty ()) {
					snap = "win_shell";
				}
				RecentEntry e;
				e.path = path;
				e.snapshot = snap;
				_recent.push_back (e);
			}
		}
		refresh_recent_snap ();
		std::fprintf (stderr, "airdour_win_shell: recent loaded (count=%d)\n", _snap.recent_count);
		std::fflush (stderr);
	} catch (...) {
		refresh_recent_snap ();
	}
}

bool
EngineHost::save_recent_sessions ()
{
	if (!_impl || !_impl->ardour_inited || !Config) {
		return false;
	}
	try {
		XMLNode root ("WinShellRecent");
		for (size_t i = 0; i < _recent.size () && i < (size_t)EngineSnapshot::kMaxRecent; ++i) {
			XMLNode* child = root.add_child ("Session");
			if (!child) {
				continue;
			}
			child->set_property ("path", _recent[i].path);
			child->set_property ("snapshot",
			                     _recent[i].snapshot.empty () ? "win_shell" : _recent[i].snapshot);
		}
		Config->add_instant_xml (root);
		refresh_recent_snap ();
		std::fprintf (stderr, "airdour_win_shell: recent saved (count=%d)\n", _snap.recent_count);
		std::fflush (stderr);
		return true;
	} catch (...) {
		return false;
	}
}

void
EngineHost::remember_recent_session (const std::string& session_dir,
                                     const std::string& snapshot,
                                     bool persist)
{
	if (session_dir.empty ()) {
		return;
	}
	std::string snap = snapshot.empty () ? "win_shell" : snapshot;
	/* Dedupe (case-insensitive path compare on Windows). */
	for (auto it = _recent.begin (); it != _recent.end (); ) {
		if (_stricmp (it->path.c_str (), session_dir.c_str ()) == 0) {
			it = _recent.erase (it);
		} else {
			++it;
		}
	}
	RecentEntry e;
	e.path = session_dir;
	e.snapshot = snap;
	_recent.insert (_recent.begin (), e);
	while (_recent.size () > (size_t)EngineSnapshot::kMaxRecent) {
		_recent.pop_back ();
	}
	refresh_recent_snap ();
	if (persist) {
		save_recent_sessions ();
	}
}

void
EngineHost::clear_recent_sessions (bool persist)
{
	_recent.clear ();
	refresh_recent_snap ();
	if (persist) {
		save_recent_sessions ();
	}
}

int
EngineHost::recent_count () const
{
	return (int)_recent.size ();
}

bool
EngineHost::recent_entry (int index, std::string* path, std::string* snapshot) const
{
	if (index < 0 || index >= (int)_recent.size ()) {
		return false;
	}
	if (path) {
		*path = _recent[index].path;
	}
	if (snapshot) {
		*snapshot = _recent[index].snapshot;
	}
	return true;
}

bool
EngineHost::open_recent (int index)
{
	std::string path;
	std::string snap;
	if (!recent_entry (index, &path, &snap)) {
		_snap.detail = L"recent open failed — bad index";
		return false;
	}
	return open_session (path, snap); /* remember_recent inside open_session */
}

bool
EngineHost::smoke_recent ()
{
	if (!_impl || !_impl->ardour_inited || !Config) {
		_snap.detail = L"smoke-recent failed — Config not ready";
		return false;
	}
	try {
		std::vector<RecentEntry> saved = _recent;

		const std::string probe_a = "C:\\AirdourSmoke\\SessionA";
		const std::string probe_b = "C:\\AirdourSmoke\\SessionB";
		clear_recent_sessions (false);
		remember_recent_session (probe_a, "win_shell", false);
		remember_recent_session (probe_b, "snap_b", false);
		/* B should be front; A second; dedupe re-push A → A front, B second. */
		remember_recent_session (probe_a, "win_shell", false);
		if (_recent.size () != 2 || _recent[0].path != probe_a || _recent[1].path != probe_b
		    || _recent[1].snapshot != "snap_b") {
			std::fprintf (stderr, "airdour_win_shell: smoke-recent FAILED (order/dedupe)\n");
			std::fflush (stderr);
			return false;
		}
		if (!save_recent_sessions ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-recent FAILED (save)\n");
			std::fflush (stderr);
			return false;
		}
		_recent.clear ();
		refresh_recent_snap ();
		load_recent_sessions ();
		if (_recent.size () != 2 || _recent[0].path != probe_a || _recent[1].snapshot != "snap_b") {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-recent FAILED (reload count=%zu)\n",
			              _recent.size ());
			std::fflush (stderr);
			return false;
		}

		/* Restore prior list so interactive prefs stay intact. */
		_recent = saved;
		save_recent_sessions ();

		/* Live reopen: remember current session dir if present. */
		if (_impl->session && !_impl->session_dir.empty ()) {
			remember_recent_session (_impl->session_dir,
			                         _impl->session->snap_name ().empty ()
			                             ? "win_shell"
			                             : _impl->session->snap_name (),
			                         true);
			std::string path;
			std::string snap;
			if (!recent_entry (0, &path, &snap) || path != _impl->session_dir) {
				std::fprintf (stderr, "airdour_win_shell: smoke-recent FAILED (remember current)\n");
				std::fflush (stderr);
				return false;
			}
			if (!open_recent (0) || !_snap.session_ok) {
				std::fprintf (stderr, "airdour_win_shell: smoke-recent FAILED (open_recent)\n");
				std::fflush (stderr);
				return false;
			}
		}

		std::fprintf (stderr, "airdour_win_shell: smoke-recent ok (persist + open)\n");
		std::fflush (stderr);
		_snap.detail = L"smoke-recent ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-recent exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

void
EngineHost::set_autosave_enabled (bool on)
{
	_autosave_enabled = on;
	if (!on) {
		_last_autosave_ms = 0;
	}
	std::fprintf (stderr, "airdour_win_shell: autosave %s\n", on ? "on" : "off");
	std::fflush (stderr);
}

std::string
EngineHost::recovery_copy_path () const
{
	if (!Config) {
		return std::string ();
	}
	return Glib::build_filename (user_config_directory (), "win_shell_recovery", "recovery.ardour");
}

uint64_t
EngineHost::snapshot_filetime (const std::string& session_dir, const std::string& snap) const
{
	if (session_dir.empty ()) {
		return 0;
	}
	std::string name = snap.empty () ? "win_shell" : snap;
	std::string file = path_join (session_dir, name + ".ardour");
	std::wstring w = utf8_path_to_wide (file);
	WIN32_FILE_ATTRIBUTE_DATA fad = {};
	if (!GetFileAttributesExW (w.c_str (), GetFileExInfoStandard, &fad)) {
		return 0;
	}
	ULARGE_INTEGER ul;
	ul.LowPart = fad.ftLastWriteTime.dwLowDateTime;
	ul.HighPart = fad.ftLastWriteTime.dwHighDateTime;
	return ul.QuadPart;
}

void
EngineHost::load_recovery_marker ()
{
	_recovery_path.clear ();
	_recovery_snapshot = "win_shell";
	_recovery_saved_ft = 0;
	_recovery_last_open_ft = 0;
	_recovery_dirty = false;
	if (!_impl || !_impl->ardour_inited || !Config) {
		return;
	}
	try {
		XMLNode* node = Config->instant_xml ("WinShellRecovery");
		if (!node) {
			return;
		}
		node->get_property ("path", _recovery_path);
		std::string snap;
		if (node->get_property ("snapshot", snap) && !snap.empty ()) {
			_recovery_snapshot = snap;
		}
		uint64_t saved = 0;
		uint64_t opened = 0;
		int dirty = 0;
		node->get_property ("saved-ft", saved);
		node->get_property ("last-open-ft", opened);
		node->get_property ("dirty", dirty);
		_recovery_saved_ft = saved;
		_recovery_last_open_ft = opened;
		_recovery_dirty = (dirty != 0);
		std::fprintf (stderr,
		              "airdour_win_shell: recovery marker (dirty=%d saved=%llu open=%llu)\n",
		              _recovery_dirty ? 1 : 0,
		              (unsigned long long)_recovery_saved_ft,
		              (unsigned long long)_recovery_last_open_ft);
		std::fflush (stderr);
	} catch (...) {
	}
}

bool
EngineHost::save_recovery_marker ()
{
	if (!_impl || !_impl->ardour_inited || !Config) {
		return false;
	}
	try {
		XMLNode node ("WinShellRecovery");
		node.set_property ("path", _recovery_path);
		node.set_property ("snapshot", _recovery_snapshot.empty () ? "win_shell" : _recovery_snapshot);
		node.set_property ("saved-ft", _recovery_saved_ft);
		node.set_property ("last-open-ft", _recovery_last_open_ft);
		node.set_property ("dirty", _recovery_dirty ? 1 : 0);
		Config->add_instant_xml (node);
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::copy_snapshot_to_recovery ()
{
	if (!_impl || _impl->session_dir.empty ()) {
		return false;
	}
	std::string snap = _recovery_snapshot.empty () ? "win_shell" : _recovery_snapshot;
	if (_impl->session) {
		std::string sn = _impl->session->snap_name ();
		if (!sn.empty ()) {
			snap = sn;
		}
	}
	std::string src = path_join (_impl->session_dir, snap + ".ardour");
	std::string dst = recovery_copy_path ();
	if (dst.empty ()) {
		return false;
	}
	try {
		std::string dir = Glib::path_get_dirname (dst);
		if (!Glib::file_test (dir, Glib::FILE_TEST_IS_DIR)) {
			std::wstring wdir = utf8_path_to_wide (dir);
			CreateDirectoryW (wdir.c_str (), nullptr);
		}
		std::wstring wsrc = utf8_path_to_wide (src);
		std::wstring wdst = utf8_path_to_wide (dst);
		if (!CopyFileW (wsrc.c_str (), wdst.c_str (), FALSE)) {
			std::fprintf (stderr, "airdour_win_shell: recovery copy failed (%lu)\n", GetLastError ());
			std::fflush (stderr);
			return false;
		}
		return true;
	} catch (...) {
		return false;
	}
}

void
EngineHost::autosave_tick (uint32_t now_ms)
{
	if (!_autosave_enabled || !_impl || !_impl->session) {
		return;
	}
	const uint32_t interval_ms = 45000u; /* 45s */
	if (_last_autosave_ms != 0 && (now_ms - _last_autosave_ms) < interval_ms) {
		return;
	}
	autosave_now ();
	_last_autosave_ms = now_ms ? now_ms : 1;
}

bool
EngineHost::autosave_now ()
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (!save_session ()) {
		return false;
	}
	std::string snap = "win_shell";
	if (_impl->session) {
		snap = _impl->session->snap_name ();
		if (snap.empty ()) {
			snap = "win_shell";
		}
	}
	_recovery_path = _impl->session_dir;
	_recovery_snapshot = snap;
	_recovery_saved_ft = snapshot_filetime (_recovery_path, snap);
	if (_recovery_saved_ft == 0) {
		FILETIME ft;
		GetSystemTimeAsFileTime (&ft);
		ULARGE_INTEGER ul;
		ul.LowPart = ft.dwLowDateTime;
		ul.HighPart = ft.dwHighDateTime;
		_recovery_saved_ft = ul.QuadPart;
	}
	_recovery_dirty = true;
	copy_snapshot_to_recovery ();
	if (!save_recovery_marker ()) {
		return false;
	}
	_snap.detail = L"autosave — recovery marker written";
	std::fprintf (stderr, "airdour_win_shell: autosave ok (path=%s ft=%llu)\n",
	              _recovery_path.c_str (), (unsigned long long)_recovery_saved_ft);
	std::fflush (stderr);
	return true;
}

bool
EngineHost::recovery_offer_available () const
{
	if (!_recovery_dirty || _recovery_path.empty ()) {
		return false;
	}
	if (_recovery_saved_ft == 0 || _recovery_saved_ft <= _recovery_last_open_ft) {
		return false;
	}
	DWORD attrs = GetFileAttributesW (utf8_path_to_wide (_recovery_path).c_str ());
	if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
		return false;
	}
	return true;
}

bool
EngineHost::recovery_offer_path (std::string* path, std::string* snapshot) const
{
	if (!recovery_offer_available ()) {
		return false;
	}
	if (path) {
		*path = _recovery_path;
	}
	if (snapshot) {
		*snapshot = _recovery_snapshot.empty () ? "win_shell" : _recovery_snapshot;
	}
	return true;
}

bool
EngineHost::restore_recovery_session ()
{
	std::string path;
	std::string snap;
	if (!recovery_offer_path (&path, &snap)) {
		_snap.detail = L"recovery restore failed — nothing to offer";
		return false;
	}
	if (!open_session (path, snap)) {
		return false;
	}
	/* open_session already mark_recovery_clean_open(). */
	_snap.detail = L"session restored from autosave";
	return true;
}

bool
EngineHost::discard_recovery_offer ()
{
	_recovery_dirty = false;
	FILETIME ft;
	GetSystemTimeAsFileTime (&ft);
	ULARGE_INTEGER ul;
	ul.LowPart = ft.dwLowDateTime;
	ul.HighPart = ft.dwHighDateTime;
	_recovery_last_open_ft = ul.QuadPart;
	bool ok = save_recovery_marker ();
	_snap.detail = L"recovery offer discarded";
	std::fprintf (stderr, "airdour_win_shell: recovery discarded\n");
	std::fflush (stderr);
	return ok;
}

bool
EngineHost::mark_recovery_clean_open ()
{
	FILETIME ft;
	GetSystemTimeAsFileTime (&ft);
	ULARGE_INTEGER ul;
	ul.LowPart = ft.dwLowDateTime;
	ul.HighPart = ft.dwHighDateTime;
	_recovery_last_open_ft = ul.QuadPart;
	_recovery_dirty = false;
	if (_impl && !_impl->session_dir.empty ()) {
		_recovery_path = _impl->session_dir;
		if (_impl->session) {
			_recovery_snapshot = _impl->session->snap_name ();
			if (_recovery_snapshot.empty ()) {
				_recovery_snapshot = "win_shell";
			}
		}
	}
	return save_recovery_marker ();
}

bool
EngineHost::smoke_recovery ()
{
	if (!_impl || !_impl->session || !_impl->ardour_inited || !Config) {
		_snap.detail = L"smoke-recovery failed — no Session";
		return false;
	}
	try {
		const bool prev = _autosave_enabled;
		_autosave_enabled = true;
		if (!autosave_now ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-recovery FAILED (autosave)\n");
			std::fflush (stderr);
			_autosave_enabled = prev;
			return false;
		}
		load_recovery_marker ();
		if (!_recovery_dirty || _recovery_path.empty () || _recovery_saved_ft == 0) {
			std::fprintf (stderr, "airdour_win_shell: smoke-recovery FAILED (marker)\n");
			std::fflush (stderr);
			_autosave_enabled = prev;
			return false;
		}
		std::string copy = recovery_copy_path ();
		if (copy.empty () || !Glib::file_test (copy, Glib::FILE_TEST_IS_REGULAR)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-recovery FAILED (copy missing)\n");
			std::fflush (stderr);
			_autosave_enabled = prev;
			return false;
		}
		/* Force offer: last-open older than saved. */
		_recovery_last_open_ft = _recovery_saved_ft > 1 ? (_recovery_saved_ft - 1) : 0;
		_recovery_dirty = true;
		save_recovery_marker ();
		if (!recovery_offer_available ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-recovery FAILED (offer)\n");
			std::fflush (stderr);
			_autosave_enabled = prev;
			return false;
		}
		if (!discard_recovery_offer () || recovery_offer_available ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-recovery FAILED (discard)\n");
			std::fflush (stderr);
			_autosave_enabled = prev;
			return false;
		}
		_autosave_enabled = prev;
		std::fprintf (stderr, "airdour_win_shell: smoke-recovery ok (autosave+offer+discard)\n");
		std::fflush (stderr);
		_snap.detail = L"smoke-recovery ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-recovery exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::start (const std::string& backend_name)
{
	prepare (backend_name);
	return start ();
}

bool
EngineHost::ensure_session ()
{
	if (_impl->session) {
		return true;
	}
	if (!_impl->engine || !_started) {
		return false;
	}

	/*
	 * PeakMeter lives on Session Routes. New Session needs:
	 * - ShellEventLoop on the UI thread (no GTK)
	 * - Path that does NOT exist yet (_is_new = !dir_exists)
	 * - Panner DLLs discoverable (ARDOUR_DLL_PATH/panners or ARDOUR_PANNER_PATH)
	 * - MSVC: never try_lock() process_lock while already held (Delivery/Route)
	 * Opt out: AIRDOUR_WIN_SHELL_SESSION=0
	 */
	const char* try_sess = std::getenv ("AIRDOUR_WIN_SHELL_SESSION");
	if (try_sess && try_sess[0] == '0') {
		_snap.detail = L"Session PeakMeter disabled (AIRDOUR_WIN_SHELL_SESSION=0) — stub meters";
		_snap.meters_real = false;
		_snap.session_ok = false;
		return false;
	}

	auto step = [] (const char* msg) {
		std::fprintf (stderr, "airdour_win_shell: session: %s\n", msg);
		std::fflush (stderr);
	};

	try {
		if (!_impl->event_loop) {
			step ("EventLoop for UI thread");
			_impl->event_loop = new ShellEventLoop ();
			EventLoop::set_event_loop_for_thread (_impl->event_loop);
		}

		step ("create temp session");
		char uniq[96];
		std::snprintf (uniq, sizeof (uniq), "airdour_win_shell_%u_%lu",
		               (unsigned)GetCurrentProcessId (), (unsigned long)GetTickCount ());
		/* Path must NOT exist yet — Session::pre_engine_init sets _is_new from that. */
		_impl->session_dir = path_join (win_temp_dir (), uniq);

		BusProfile profile;
		profile.master_out_channels = 2;

		/* Click default from prefs / RC (shell ctor defaults to on). */
		Config->set_clicking (_pref_click_default);

		step ("Session ctor (new/unnamed)…");
		/* unnamed=true matches unit-test / headless new-session path. */
		_impl->session = new Session (*_impl->engine, _impl->session_dir, "win_shell",
		                              &profile, std::string (), true);
		step ("Session ctor done");
		/* Session ctor already called engine->set_session(this). */

		/* Demo wav regions: opt-in. Default is empty tracks + PeakMeter (click
		 * when rolling). Writing tones under a live Dummy process has hung /
		 * AV'd on MSVC; set AIRDOUR_WIN_SHELL_DEMO_TONES=1 to try. */
		const char* tone_flag = std::getenv ("AIRDOUR_WIN_SHELL_DEMO_TONES");
		const bool want_tones = (tone_flag && tone_flag[0] == '1');

		step ("new_audio_track x6");
		AudioTrackList created = _impl->session->new_audio_track (
			1, 2,
			std::shared_ptr<RouteGroup> (),
			6u /* default Audio 1–6 (Reverb/Comp/EQ/Delay/Limiter/Gate); add more via UI */,
			"Audio",
			PresentationInfo::max_order);

		_impl->tracks.clear ();
		for (AudioTrackList::iterator it = created.begin (); it != created.end (); ++it) {
			_impl->tracks.push_back (*it);
		}
		update_session_path_snap ();

		if (want_tones) {
			samplecnt_t sr = (samplecnt_t)_impl->engine->sample_rate ();
			if (sr <= 0) {
				sr = 48000;
			}
			const samplecnt_t n = sr * 4; /* 4s demo tone */

			step ("write demo tones into playlists");
			for (size_t t = 0; t < _impl->tracks.size (); ++t) {
				std::shared_ptr<AudioTrack> track = _impl->tracks[t];
				if (!track) {
					continue;
				}

				std::string wav = path_join (
					_impl->session_dir,
					string_compose ("tone_%1.wav", (int)t + 1));

				std::shared_ptr<Source> src = SourceFactory::createWritable (
					DataType::AUDIO, *_impl->session, wav, sr);
				std::shared_ptr<AudioSource> as = std::dynamic_pointer_cast<AudioSource> (src);
				std::shared_ptr<AudioFileSource> afs = std::dynamic_pointer_cast<AudioFileSource> (src);
				if (!as) {
					continue;
				}

				std::vector<Sample> buf ((size_t)n);
				float freq = 110.f * (float)(t + 1);
				float amp = 0.22f + 0.05f * (float)t;
				for (samplecnt_t i = 0; i < n; ++i) {
					float ph = 6.2831853f * freq * ((float)i / (float)sr);
					/* Soft envelope pulses every beat (~0.5s). */
					float beat = std::fmod ((float)i / (float)sr * 2.f, 1.f);
					float env = 0.55f + 0.45f * std::exp (-beat * 8.f);
					buf[(size_t)i] = amp * env * std::sin (ph);
				}
				{
					Source::WriterLock lm (as->mutex ());
					as->mark_streaming_write_started (lm);
					as->write (buf.data (), n);
					as->mark_streaming_write_completed (lm, timecnt_t ((samplepos_t)n));
				}
				if (afs) {
					afs->flush ();
				}

				PropertyList plist;
				plist.add (Properties::start, timepos_t (0));
				plist.add (Properties::length, timecnt_t ((samplepos_t)n));
				std::shared_ptr<Region> region = RegionFactory::create (src, plist);
				if (region && track->playlist ()) {
					track->playlist ()->add_region (region, timepos_t (0));
				}

				if (track->gain_control ()) {
					track->gain_control ()->set_value (_track_gain[t], Controllable::NoGroup);
				}
			}
			_snap.detail = L"Session + PeakMeter — demo tones on Audio 1–6";
		} else {
			for (size_t t = 0; t < _impl->tracks.size (); ++t) {
				if (_impl->tracks[t] && _impl->tracks[t]->gain_control ()) {
					_impl->tracks[t]->gain_control ()->set_value (_track_gain[t], Controllable::NoGroup);
				}
			}
			_snap.detail = L"Session + PeakMeter — empty Audio 1–6 (no demo tones)";
		}

		/* Best-effort ACE Reverb on Audio 1 (headless insert, no GTK UI). */
		insert_track_fx (0);
		/* Best-effort ACE Compressor on Audio 2 (threshold/ratio D2D). */
		if (_impl->tracks.size () > 1) {
			insert_track_comp (1);
		}
		/* Best-effort ACE EQ on Audio 3 (Gain L / Gain 2 / Gain H D2D). */
		if (_impl->tracks.size () > 2) {
			insert_track_eq (2);
		}
		/* Best-effort ACE Delay on Audio 4 (Time / Feedback D2D). */
		if (_impl->tracks.size () > 3) {
			insert_track_delay (3);
		}
		/* Best-effort ACE Limiter on Audio 5 (Ceiling / Release D2D). */
		if (_impl->tracks.size () > 4) {
			insert_track_limiter (4);
		}
		/* Best-effort ACE Gate on Audio 6 (Threshold / Hold D2D). */
		if (_impl->tracks.size () > 5) {
			insert_track_gate (5);
		}

		step ("aux bus + sends");
		ensure_aux_sends ();

		_snap.audio_track_count = (int)_impl->tracks.size ();
		_snap.midi_track_count = _impl->midi_track ? 1 : 0;
		_snap.session_ok = true;
		_snap.meters_real = true;
		_snap.waves_from_session = true;
		refresh_region_flags ();
		refresh_track_fx_snap ();
		refresh_track_comp_snap ();
		refresh_track_eq_snap ();
		refresh_track_delay_snap ();
		refresh_track_limiter_snap ();
		refresh_track_gate_snap ();
		step ("ready");
		return true;
	} catch (SessionException& e) {
		std::fprintf (stderr, "airdour_win_shell: session SessionException: %s\n", e.what ());
		std::fflush (stderr);
		destroy_session ();
		_snap.detail = L"Session create failed — stub meters";
		return false;
	} catch (failed_constructor& e) {
		std::fprintf (stderr, "airdour_win_shell: session failed_constructor: %s\n", e.what ());
		std::fflush (stderr);
		destroy_session ();
		_snap.detail = L"Session create failed — stub meters";
		return false;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: session exception: %s\n", e.what ());
		std::fflush (stderr);
		destroy_session ();
		_snap.detail = L"Session exception — stub meters";
		return false;
	} catch (...) {
		std::fprintf (stderr, "airdour_win_shell: session unknown failure\n");
		std::fflush (stderr);
		destroy_session ();
		_snap.detail = L"Session unknown failure — stub meters";
		return false;
	}
}

void
EngineHost::destroy_session ()
{
	if (!_impl) {
		return;
	}
	_impl->tracks.clear ();
	_impl->aux_bus.reset ();
	_impl->midi_track.reset ();
	_impl->midi_region.reset ();
	_impl->midi_activity = 0.f;
	_impl->selected_midi_note_id = -1;
	_impl->midi_out_connected = false;
	_impl->midi_out_port.clear ();
	_impl->midi_synth_ok = false;
	_impl->midi_synth_to_master = false;
	_impl->midi_synth_name.clear ();
	_impl->track_fx_ok = false;
	_impl->track_fx_bypassed = false;
	_impl->track_fx_param = 0.3f;
	_impl->track_fx_name.clear ();
	_impl->track_fx.reset ();
	_impl->track_comp_ok = false;
	_impl->track_comp_bypassed = false;
	_impl->track_comp_threshold_db = 0.f;
	_impl->track_comp_ratio = 4.f;
	_impl->track_comp_sidechain = false;
	_impl->track_comp_sidechain_src = -1;
	_impl->track_comp_name.clear ();
	_impl->track_comp.reset ();
	_impl->track_eq_ok = false;
	_impl->track_eq_bypassed = false;
	_impl->track_eq_gain_l_db = 0.f;
	_impl->track_eq_gain_m_db = 0.f;
	_impl->track_eq_gain_h_db = 0.f;
	_impl->track_eq_name.clear ();
	_impl->track_eq.reset ();
	_impl->track_delay_ok = false;
	_impl->track_delay_bypassed = false;
	_impl->track_delay_time_ms = 160.f;
	_impl->track_delay_feedback_pct = 20.f;
	_impl->track_delay_name.clear ();
	_impl->track_delay.reset ();
	_impl->track_limiter_ok = false;
	_impl->track_limiter_bypassed = false;
	_impl->track_limiter_ceiling_db = -0.3f;
	_impl->track_limiter_release_ms = 100.f;
	_impl->track_limiter_name.clear ();
	_impl->track_limiter.reset ();
	_impl->track_gate_ok = false;
	_impl->track_gate_bypassed = false;
	_impl->track_gate_threshold_db = -40.f;
	_impl->track_gate_hold_ms = 50.f;
	_impl->track_gate_name.clear ();
	_impl->track_gate.reset ();
	_impl->selected_track = -1;
	_impl->selected_region.reset ();
	if (_impl->session) {
		try {
			_impl->session->request_stop (false, false, TRS_UI);
		} catch (...) {
		}
		/* Must delete so Session::destroy() unregisters LTC/click ports and
		 * clears SessionHandleRefs — otherwise reopen fails (LTC-Out exists).
		 * Process-exit --timeout still ExitProcess(0) and skips this path. */
		Session* doomed = _impl->session;
		_impl->session = nullptr;
		try {
			delete doomed;
		} catch (...) {
			std::fprintf (stderr, "airdour_win_shell: Session delete threw\n");
			std::fflush (stderr);
		}
	}
	_snap.session_ok = false;
	_snap.meters_real = false;
	_snap.waves_from_session = false;
	_snap.transport_rolling = false;
	_snap.session_record_armed = false;
	_snap.session_recording = false;
	_snap.session_saved = false;
	_snap.undo_depth = 0;
	_snap.redo_depth = 0;
	_snap.undo_list_count = 0;
	_snap.next_undo_name[0] = L'\0';
	_snap.next_redo_name[0] = L'\0';
	for (int i = 0; i < EngineSnapshot::kMaxHistory; ++i) {
		_snap.undo_list_name[i][0] = L'\0';
	}
	_snap.has_selection = false;
	_snap.selection_track = -1;
	_snap.selection_start_sec = 0.f;
	_snap.selection_end_sec = 0.f;
	_snap.loop_active = false;
	_snap.loop_playing = false;
	_snap.loop_start_sec = 0.f;
	_snap.loop_end_sec = 0.f;
	_snap.punch_range_ok = false;
	_snap.punch_enabled = false;
	_snap.punch_start_sec = 0.f;
	_snap.punch_end_sec = 0.f;
	_snap.tempo_bpm = 120.f;
	_snap.count_in_enabled = false;
	_snap.click_enabled = false;
	_snap.marker_count = 0;
	_snap.session_path.clear ();
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_snap.track_has_regions[i] = false;
		_snap.track_armed[i] = false;
		_snap.track_frozen[i] = false;
	}
}

void
EngineHost::collect_tracks ()
{
	_impl->tracks.clear ();
	_impl->aux_bus.reset ();
	_snap.audio_track_count = 0;
	_snap.midi_track_count = 0;
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_snap.track_name[i][0] = L'\0';
		_snap.track_gain[i] = _track_gain[i];
	}
	_snap.midi_track_name[0] = L'\0';
	if (!_impl || !_impl->session) {
		return;
	}
	try {
		std::shared_ptr<RouteList> routes = _impl->session->get_tracks ();
		if (!routes) {
			return;
		}
		std::vector<std::shared_ptr<AudioTrack> > audio;
		for (RouteList::const_iterator it = routes->begin (); it != routes->end (); ++it) {
			std::shared_ptr<AudioTrack> at = std::dynamic_pointer_cast<AudioTrack> (*it);
			if (at) {
				audio.push_back (at);
			}
		}
		std::sort (audio.begin (), audio.end (),
		           [] (const std::shared_ptr<AudioTrack>& a,
		               const std::shared_ptr<AudioTrack>& b) {
			           if (!a) {
				           return false;
			           }
			           if (!b) {
				           return true;
			           }
			           return a->presentation_info ().order ()
			                  < b->presentation_info ().order ();
		           });
		for (size_t i = 0; i < audio.size () && i < (size_t)EngineSnapshot::kMeters; ++i) {
			_impl->tracks.push_back (audio[i]);
			copy_name_to_snap (_snap.track_name[i], EngineSnapshot::kTrackNameLen,
			                   audio[i]->name ());
			if (audio[i]->gain_control ()) {
				_snap.track_gain[i] = (float)audio[i]->gain_control ()->get_value ();
				_track_gain[i] = _snap.track_gain[i];
			}
		}
		_snap.audio_track_count = (int)_impl->tracks.size ();
		/* Prefer existing midi_track if still in Session; else first MidiTrack. */
		bool midi_alive = false;
		if (_impl->midi_track) {
			for (RouteList::const_iterator it = routes->begin (); it != routes->end (); ++it) {
				if (*it == _impl->midi_track) {
					midi_alive = true;
					break;
				}
			}
		}
		if (!midi_alive) {
			_impl->midi_track.reset ();
			for (RouteList::const_iterator it = routes->begin (); it != routes->end (); ++it) {
				std::shared_ptr<MidiTrack> mt = std::dynamic_pointer_cast<MidiTrack> (*it);
				if (mt) {
					_impl->midi_track = mt;
					break;
				}
			}
		}
		_snap.midi_track_count = _impl->midi_track ? 1 : 0;
		if (_impl->midi_track) {
			copy_name_to_snap (_snap.midi_track_name, EngineSnapshot::kTrackNameLen,
			                   _impl->midi_track->name ());
		}
		std::shared_ptr<RouteList const> all = _impl->session->get_routes ();
		if (all) {
			for (RouteList::const_iterator it = all->begin (); it != all->end (); ++it) {
				if (!*it || (*it)->is_master () || (*it)->is_monitor ()) {
					continue;
				}
				if ((*it)->presentation_info ().flags () & PresentationInfo::AudioBus) {
					_impl->aux_bus = *it;
					break;
				}
			}
		}
	} catch (...) {
	}
}

void
EngineHost::update_session_path_snap ()
{
	if (_impl && !_impl->session_dir.empty ()) {
		_snap.session_path = utf8_to_wide (_impl->session_dir);
	} else if (_impl && _impl->session) {
		_impl->session_dir = _impl->session->path ();
		_snap.session_path = utf8_to_wide (_impl->session_dir);
	} else {
		_snap.session_path.clear ();
	}
}

std::string
EngineHost::session_dir () const
{
	if (_impl) {
		return _impl->session_dir;
	}
	return std::string ();
}

bool
EngineHost::save_session ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"save failed — no Session";
		_snap.session_saved = false;
		return false;
	}
	try {
		int rc = _impl->session->save_state ("");
		if (rc != 0) {
			std::fprintf (stderr, "airdour_win_shell: save_state rc=%d writable=%d cannot_save=%d\n",
			              rc,
			              _impl->session->writable () ? 1 : 0,
			              _impl->session->cannot_save () ? 1 : 0);
			std::fflush (stderr);
			_snap.detail = L"save_state failed";
			_snap.session_saved = false;
			return false;
		}
		try {
			_impl->session->end_unnamed_status ();
		} catch (...) {
		}
		update_session_path_snap ();
		_snap.session_saved = true;
		_snap.detail = L"session saved";
		std::fprintf (stderr, "airdour_win_shell: session saved under %s\n",
		              _impl->session_dir.c_str ());
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: save exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"save exception";
		_snap.session_saved = false;
		return false;
	} catch (...) {
		_snap.detail = L"save unknown failure";
		_snap.session_saved = false;
		return false;
	}
}

bool
EngineHost::open_session (const std::string& session_dir, const std::string& snapshot_name)
{
	if (!_impl || !_impl->engine || !_started) {
		_snap.detail = L"open failed — engine not running";
		return false;
	}
	if (session_dir.empty ()) {
		_snap.detail = L"open failed — empty path";
		return false;
	}

	DWORD attrs = GetFileAttributesW (utf8_path_to_wide (session_dir).c_str ());
	if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
		_snap.detail = L"open failed — session folder missing";
		std::fprintf (stderr, "airdour_win_shell: open: not a directory: %s\n", session_dir.c_str ());
		std::fflush (stderr);
		return false;
	}

	auto step = [] (const char* msg) {
		std::fprintf (stderr, "airdour_win_shell: open: %s\n", msg);
		std::fflush (stderr);
	};

	try {
		if (!_impl->event_loop) {
			_impl->event_loop = new ShellEventLoop ();
			EventLoop::set_event_loop_for_thread (_impl->event_loop);
		}

		step ("detach current Session");
		destroy_session ();

		_impl->session_dir = session_dir;
		std::string snap = snapshot_name.empty () ? "win_shell" : snapshot_name;

		step (("Session ctor load " + session_dir).c_str ());
		/* Path exists → load_state. unnamed=false so it is a normal reopen. */
		_impl->session = new Session (*_impl->engine, session_dir, snap,
		                              nullptr, std::string (), false);
		step ("Session load done");
		/* Session ctor already called engine->set_session(this). */

		collect_tracks ();
		if (_impl->tracks.empty ()) {
			step ("no audio tracks after load — creating 3");
			AudioTrackList created = _impl->session->new_audio_track (
				1, 2,
				std::shared_ptr<RouteGroup> (),
				3u,
				"Audio",
				PresentationInfo::max_order);
			for (AudioTrackList::iterator it = created.begin (); it != created.end (); ++it) {
				_impl->tracks.push_back (*it);
			}
			collect_tracks ();
		}

		for (size_t t = 0; t < _impl->tracks.size () && t < (size_t)EngineSnapshot::kMeters; ++t) {
			if (_impl->tracks[t] && _impl->tracks[t]->gain_control ()) {
				_impl->tracks[t]->gain_control ()->set_value (_track_gain[t], Controllable::NoGroup);
			}
		}

		ensure_aux_sends ();

		_snap.session_ok = true;
		_snap.meters_real = true;
		_snap.waves_from_session = true;
		_snap.session_saved = true;
		update_session_path_snap ();
		refresh_region_flags ();
		refresh_record_flags ();
		remember_recent_session (session_dir, snap, true);
		mark_recovery_clean_open ();
		_snap.detail = L"session reopened — Editor peaks from disk";
		std::fprintf (stderr, "airdour_win_shell: opened %s (tracks=%u regions0=%d)\n",
		              session_dir.c_str (),
		              (unsigned)_impl->tracks.size (),
		              _snap.track_has_regions[0] ? 1 : 0);
		std::fflush (stderr);
		return true;
	} catch (SessionException& e) {
		std::fprintf (stderr, "airdour_win_shell: open SessionException: %s\n", e.what ());
		std::fflush (stderr);
		destroy_session ();
		_snap.detail = L"open SessionException";
		return false;
	} catch (failed_constructor& e) {
		std::fprintf (stderr, "airdour_win_shell: open failed_constructor: %s\n", e.what ());
		std::fflush (stderr);
		destroy_session ();
		_snap.detail = L"open failed_constructor";
		return false;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: open exception: %s\n", e.what ());
		std::fflush (stderr);
		destroy_session ();
		_snap.detail = L"open exception";
		return false;
	} catch (...) {
		std::fprintf (stderr, "airdour_win_shell: open unknown failure\n");
		std::fflush (stderr);
		destroy_session ();
		_snap.detail = L"open unknown failure";
		return false;
	}
}

bool
EngineHost::reload_session ()
{
	if (!_impl || _impl->session_dir.empty ()) {
		_snap.detail = L"reload failed — no session path";
		return false;
	}
	std::string path = _impl->session_dir;
	std::string snap = "win_shell";
	if (_impl->session) {
		snap = _impl->session->snap_name ();
		if (snap.empty ()) {
			snap = "win_shell";
		}
	}
	if (!save_session ()) {
		return false;
	}
	return open_session (path, snap);
}

bool
EngineHost::compute_export_range (int64_t* start_sample, int64_t* end_sample) const
{
	if (!start_sample || !end_sample || !_impl || !_impl->session) {
		return false;
	}
	*start_sample = 0;
	*end_sample = 0;

	try {
		samplepos_t s = _impl->session->current_start_sample ();
		samplepos_t e = _impl->session->current_end_sample ();
		if (e > s) {
			*start_sample = (int64_t)s;
			*end_sample = (int64_t)e;
			return true;
		}
	} catch (...) {
	}

	samplepos_t max_end = 0;
	try {
		for (size_t t = 0; t < _impl->tracks.size (); ++t) {
			if (!_impl->tracks[t] || !_impl->tracks[t]->playlist ()) {
				continue;
			}
			std::shared_ptr<RegionList> regions = _impl->tracks[t]->playlist ()->region_list ();
			if (!regions) {
				continue;
			}
			for (RegionList::const_iterator it = regions->begin (); it != regions->end (); ++it) {
				if (!*it) {
					continue;
				}
				samplepos_t re = (*it)->end_position ().samples ();
				if (re > max_end) {
					max_end = re;
				}
			}
		}
	} catch (...) {
	}

	if (max_end > 0) {
		*start_sample = 0;
		*end_sample = (int64_t)max_end;
		return true;
	}

	samplecnt_t sr = _impl->session->sample_rate ();
	if (sr <= 0 && _impl->engine) {
		sr = (samplecnt_t)_impl->engine->sample_rate ();
	}
	if (sr <= 0) {
		sr = 48000;
	}
	*start_sample = 0;
	*end_sample = (int64_t)sr * 2; /* 2s silence fallback */
	return true;
}

bool
EngineHost::export_session_wav (const std::string& outfile, int64_t start_sample, int64_t end_sample)
{
	_snap.export_ok = false;
	_snap.export_path.clear ();
	_last_export_path.clear ();

	if (!_impl || !_impl->session) {
		_snap.detail = L"export failed — no Session";
		return false;
	}

	try {
		_impl->session->request_stop (false, false, TRS_UI);
	} catch (...) {
	}

	if (end_sample <= start_sample) {
		if (!compute_export_range (&start_sample, &end_sample) || end_sample <= start_sample) {
			_snap.detail = L"export failed — empty range";
			return false;
		}
	}

	std::shared_ptr<Route> master = _impl->session->master_out ();
	if (!master || !master->output ()) {
		_snap.detail = L"export failed — no master out";
		return false;
	}

	samplecnt_t sr = _impl->session->sample_rate ();
	if (sr <= 0) {
		sr = 48000;
	}

	std::string out = outfile;
	std::string folder;
	std::string basename = "airdour_export";
	if (out.empty ()) {
		folder = _impl->session->session_directory ().export_path ();
		out = path_join (folder, basename + ".wav");
	} else {
		/* Split path into folder + basename without .wav */
		size_t slash = out.find_last_of ("\\/");
		folder = (slash == std::string::npos) ? "." : out.substr (0, slash);
		basename = (slash == std::string::npos) ? out : out.substr (slash + 1);
		if (basename.size () > 4) {
			std::string ext = basename.substr (basename.size () - 4);
			if (ext == ".wav" || ext == ".WAV") {
				basename = basename.substr (0, basename.size () - 4);
			}
		}
		out = path_join (folder, basename + ".wav");
	}

	{
		std::wstring wfolder = utf8_path_to_wide (folder);
		if (!wfolder.empty ()) {
			CreateDirectoryW (wfolder.c_str (), nullptr);
		}
	}

	std::fprintf (stderr, "airdour_win_shell: export range %lld..%lld → %s\n",
	              (long long)start_sample, (long long)end_sample, out.c_str ());
	std::fflush (stderr);

	try {
		std::shared_ptr<ExportHandler> handler = _impl->session->get_export_handler ();
		std::shared_ptr<ExportStatus> status = _impl->session->get_export_status ();
		if (!handler || !status) {
			_snap.detail = L"export failed — no handler";
			return false;
		}

		ExportTimespanPtr tsp = handler->add_timespan ();
		std::shared_ptr<ExportChannelConfiguration> ccp = handler->add_channel_config ();
		std::shared_ptr<ExportFilename> fnp = handler->add_filename ();

		char rate_buf[32];
		std::snprintf (rate_buf, sizeof (rate_buf), "%lld", (long long)sr);
		std::string xml =
			std::string (
			    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
			    "<ExportFormatSpecification name=\"AIRDOUR-WAV\" id=\"b1280899-0459-4aef-9dc9-7e2277fa6d24\">"
			    "  <Encoding id=\"F_WAV\" type=\"T_Sndfile\" extension=\"wav\" name=\"WAV\" has-sample-format=\"true\" channel-limit=\"256\"/>"
			    "  <SampleRate rate=\"")
			+ rate_buf +
			"\"/>"
			"  <SRCQuality quality=\"SRC_SincBest\"/>"
			"  <EncodingOptions>"
			"    <Option name=\"sample-format\" value=\"SF_16\"/>"
			"    <Option name=\"dithering\" value=\"D_None\"/>"
			"    <Option name=\"tag-metadata\" value=\"true\"/>"
			"    <Option name=\"tag-support\" value=\"false\"/>"
			"    <Option name=\"broadcast-info\" value=\"false\"/>"
			"  </EncodingOptions>"
			"  <Processing>"
			"    <Normalize enabled=\"false\" target=\"0\"/>"
			"    <Silence>"
			"      <Start><Trim enabled=\"false\"/><Add enabled=\"false\">"
			"        <Duration format=\"Timecode\" hours=\"0\" minutes=\"0\" seconds=\"0\" frames=\"0\"/>"
			"      </Add></Start>"
			"      <End><Trim enabled=\"false\"/><Add enabled=\"false\">"
			"        <Duration format=\"Timecode\" hours=\"0\" minutes=\"0\" seconds=\"0\" frames=\"0\"/>"
			"      </Add></End>"
			"    </Silence>"
			"  </Processing>"
			"</ExportFormatSpecification>";

		XMLTree tree;
		if (!tree.read_buffer (xml.c_str ()) || !tree.root ()) {
			_snap.detail = L"export failed — format XML";
			return false;
		}
		std::shared_ptr<ExportFormatSpecification> fmp = handler->add_format (*tree.root ());

		tsp->set_range ((samplepos_t)start_sample, (samplepos_t)end_sample);
		tsp->set_range_id ("session");
		tsp->set_name (basename);
		tsp->set_realtime (false);

		IO* master_out = master->output ().get ();
		for (uint32_t n = 0; n < master_out->n_ports ().n_audio (); ++n) {
			PortExportChannel* channel = new PortExportChannel ();
			channel->add_port (master_out->audio (n));
			ExportChannelPtr chan_ptr (channel);
			ccp->register_channel (chan_ptr);
		}
		if (ccp->get_n_chans () == 0) {
			_snap.detail = L"export failed — no master channels";
			return false;
		}

		fnp->set_folder (folder);
		fnp->set_timespan (tsp);
		fnp->include_label = false;

		fmp->set_soundcloud_upload (false);
		handler->add_export_config (tsp, ccp, fmp, fnp, BroadcastInfoPtr ());

		if (0 != handler->do_export ()) {
			_snap.detail = L"export failed — do_export";
			return false;
		}

		while (status->running ()) {
			/* Keep UI responsive during interactive export; smoke is short. */
			MSG msg;
			while (PeekMessageW (&msg, nullptr, 0, 0, PM_REMOVE)) {
				TranslateMessage (&msg);
				DispatchMessageW (&msg);
			}
			ARDOUR::GUIIdle ();
			Sleep (10);
		}
		status->finish (TRS_UI);

		if (status->aborted ()) {
			_snap.detail = L"export aborted";
			return false;
		}

		std::wstring wout = utf8_path_to_wide (out);
		DWORD attrs = GetFileAttributesW (wout.c_str ());
		if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
			/* Handler may have written a slightly different name — check get_path. */
			std::string alt = fnp->get_path (fmp);
			if (!alt.empty ()) {
				out = alt;
				wout = utf8_path_to_wide (out);
				attrs = GetFileAttributesW (wout.c_str ());
			}
		}
		if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
			_snap.detail = L"export finished but file missing";
			std::fprintf (stderr, "airdour_win_shell: export file missing: %s\n", out.c_str ());
			std::fflush (stderr);
			return false;
		}

		HANDLE hf = CreateFileW (wout.c_str (), GENERIC_READ, FILE_SHARE_READ, nullptr,
		                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		LARGE_INTEGER sz = {};
		if (hf != INVALID_HANDLE_VALUE) {
			GetFileSizeEx (hf, &sz);
			CloseHandle (hf);
		}
		if (sz.QuadPart < 44) {
			_snap.detail = L"export file too small";
			return false;
		}

		_last_export_path = out;
		_snap.export_path = utf8_to_wide (out);
		_snap.export_ok = true;
		_snap.detail = L"export ok — WAV written";
		std::fprintf (stderr, "airdour_win_shell: export ok (%lld bytes) %s\n",
		              (long long)sz.QuadPart, out.c_str ());
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: export exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"export exception";
		return false;
	} catch (...) {
		_snap.detail = L"export unknown failure";
		return false;
	}
}

bool
EngineHost::smoke_export_wav (std::string* out_path)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	refresh_region_flags ();
	if (!_snap.track_has_regions[0]) {
		if (!import_test_tone (0, 440.f, 1.5f)) {
			return false;
		}
	}
	std::string folder = _impl->session->session_directory ().export_path ();
	std::string path = path_join (folder, "airdour_smoke_export.wav");
	if (!export_session_wav (path)) {
		return false;
	}
	if (out_path) {
		*out_path = _last_export_path;
	}
	return true;
}

std::string
EngineHost::last_export_path () const
{
	return _last_export_path;
}

bool
EngineHost::start ()
{
	if (_started) {
		return true;
	}

	auto step = [] (const char* msg) {
		std::fprintf (stderr, "airdour_win_shell: engine: %s\n", msg);
		std::fflush (stderr);
	};

	if (_impl->engine) {
		step ("engine->start (restart)");
		if (_impl->engine->start () != 0) {
			_snap.detail = L"engine restart failed";
			return false;
		}
		_started = true;
		ensure_session ();
		refresh_snapshot ();
		step ("running");
		return true;
	}

	if (!_impl->ardour_inited) {
		step ("ARDOUR::init");
		if (!ARDOUR::init (false, "", false)) {
			_snap.detail = L"ARDOUR::init failed";
			return false;
		}
		_impl->ardour_inited = true;
	}

	load_shell_prefs ();
	load_recent_sessions ();
	load_recovery_marker ();
	if (_backend_name.empty ()) {
		_backend_name = _pref_backend.empty () ? "Dummy" : _pref_backend;
	}
	if (_period_frames == 0 && _pref_period_frames > 0) {
		_period_frames = _pref_period_frames;
	}

	step ("SessionEvent pool");
	SessionEvent::create_per_thread_pool ("win-shell", 512);

	step ("AudioEngine::create");
	_impl->engine = AudioEngine::create ();
	if (!_impl->engine) {
		_snap.detail = L"AudioEngine::create failed";
		return false;
	}

	const std::string name = resolve_backend_name (_backend_name);
	step (("set_backend " + name).c_str ());
	if (!_impl->engine->set_backend (name, "airdour", "")) {
		_snap.detail = L"set_backend failed: " + utf8_to_wide (name);
		_snap.backend = utf8_to_wide (name);
		return false;
	}

	configure_backend_midi ();

	if (_period_frames > 0) {
		step (("set_buffer_size " + std::to_string (_period_frames)).c_str ());
		if (_impl->engine->set_buffer_size (_period_frames) != 0) {
			_snap.detail = L"set_buffer_size failed";
			return false;
		}
	}

	step ("engine->start");
	if (_impl->engine->start () != 0) {
		_snap.detail = L"engine start failed (" + utf8_to_wide (name) + L")";
		_snap.backend = utf8_to_wide (_impl->engine->current_backend_name ());
		return false;
	}

	/* Headless-style plugin scan so bundled LV2 instruments are findable. */
	ARDOUR::init_post_engine (0);
	ensure_plugins_scanned ();

	_started = true;
	ensure_session ();
	if (_pref_snap_default) {
		set_snap_enabled (true);
	}
	refresh_prefs_snap ();
	refresh_snapshot ();
	if (_snap.session_ok) {
		_snap.detail = L"Session + PeakMeter — UI thread; audio on RT";
	} else {
		_snap.detail = L"engine only — stub meters (no Session)";
	}
	step ("running");
	return true;
}

void
EngineHost::transport_play ()
{
	if (!_started && !start ()) {
		return;
	}
	/* Best-effort GS / physical MIDI out for playback (after edits, not before). */
	if (_impl && _impl->midi_track) {
		connect_midi_output ();
	}
	_impl->transport_playing = true;
	if (_impl->engine) {
		_impl->play_origin_samples = _impl->engine->processed_samples ();
		if (_impl->play_origin_samples < 0) {
			_impl->play_origin_samples = 0;
		}
	}
	if (_impl->session) {
		_impl->session->request_roll (TRS_UI);
		_snap.transport_rolling = true;
		_snap.detail = L"transport rolling — PeakMeter live";
	} else {
		_snap.transport_rolling = true;
		_snap.detail = L"transport rolling — engine processed_samples clock";
	}
	refresh_snapshot ();
}

void
EngineHost::transport_stop ()
{
	_impl->transport_playing = false;
	if (_impl->session) {
		/* Stop transport first so DiskWriter can finalize the take, then clear
		 * session record-enable. Disabling record before stop drops the capture.
		 * Skip immediate locate — FSM may still be DeclickToStop (bad transition). */
		try {
			_impl->session->request_stop (false, false, TRS_UI);
		} catch (...) {
		}
		try {
			_impl->session->disable_record (false, true);
		} catch (...) {
		}
		refresh_region_flags ();
		refresh_record_flags ();
		_snap.detail = L"transport stopped — Session kept warm";
	} else {
		_snap.detail = L"transport stopped — engine kept warm";
	}
	_snap.transport_rolling = false;
	_snap.transport_sec = 0.f;
	_impl->play_origin_samples = 0;
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_snap.meter_peak[i] = 0.f;
	}
	refresh_snapshot ();
}

void
EngineHost::stop_backend_only ()
{
	if (!_impl || !_impl->engine) {
		_started = false;
		_snap.running = false;
		return;
	}
	try {
		std::shared_ptr<AudioBackend> be = _impl->engine->current_backend ();
		if (be) {
			be->stop ();
		}
	} catch (...) {
	}
	_started = false;
	_snap.running = false;
	_snap.transport_rolling = false;
}

void
EngineHost::stop ()
{
	if (!_impl) {
		_started = false;
		_snap.running = false;
		return;
	}

	if (_impl->session) {
		_impl->session->request_stop (false, false, TRS_UI);
	}
	destroy_session ();
	stop_backend_only ();

	_snap.transport_sec = 0.f;
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_snap.meter_peak[i] = 0.f;
	}
	_snap.detail = L"engine stopped — press Play to restart";
}

void
EngineHost::set_gain (float linear)
{
	set_track_gain (0, linear);
}

void
EngineHost::set_track_gain (int track, float linear)
{
	linear = std::max (0.f, std::min (1.f, linear));
	if (track < 0 || track >= EngineSnapshot::kMeters) {
		return;
	}
	_track_gain[track] = linear;
	if (track == 0) {
		_gain = linear;
		_snap.gain = linear;
	}
	if (_impl && track < (int)_impl->tracks.size () && _impl->tracks[track]
	    && _impl->tracks[track]->gain_control ()) {
		_impl->tracks[track]->gain_control ()->set_value (linear, Controllable::UseGroup);
		/* Mirror peers after UseGroup propagation. */
		for (size_t i = 0; i < _impl->tracks.size () && i < (size_t)EngineSnapshot::kMeters; ++i) {
			if (_impl->tracks[i] && _impl->tracks[i]->gain_control ()) {
				_track_gain[i] = (float)_impl->tracks[i]->gain_control ()->get_value ();
				_snap.track_gain[i] = _track_gain[i];
			}
		}
		_gain = _track_gain[0];
		_snap.gain = _gain;
	}
}

bool
EngineHost::set_midi_gain (float linear)
{
	linear = std::max (0.f, std::min (1.f, linear));
	if (!_impl) {
		return false;
	}
	ensure_midi_track ();
	if (!_impl->midi_track || !_impl->midi_track->gain_control ()) {
		return false;
	}
	try {
		_impl->midi_track->gain_control ()->set_value (linear, Controllable::NoGroup);
		return true;
	} catch (...) {
		return false;
	}
}

float
EngineHost::gain () const
{
	return _gain;
}

void
EngineHost::read_meters ()
{
	if (!_impl || !_impl->session || !_snap.session_ok) {
		_snap.meters_real = false;
		return;
	}
	try {
		_snap.meters_real = true;
		for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
			float peak = 0.f;
			bool  clip = false;
			if (i < (int)_impl->tracks.size () && _impl->tracks[i]) {
				std::shared_ptr<PeakMeter> pm = _impl->tracks[i]->peak_meter ();
				if (pm) {
					peak = peak_linear_from_meter (pm);
					clip = clip_from_meter (pm);
				}
			}
			_snap.meter_peak[i] = peak;
			_snap.meter_clip[i] = clip;
		}
	} catch (...) {
		_snap.meters_real = false;
	}
}

bool
EngineHost::clear_track_meter_clip (int track)
{
	if (!_impl || !_impl->session || track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	if (track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	try {
		std::shared_ptr<PeakMeter> pm = _impl->tracks[track]->peak_meter ();
		if (!pm) {
			return false;
		}
		pm->reset_max ();
		_snap.meter_clip[track] = false;
		_snap.detail = L"meter clip cleared";
		return true;
	} catch (...) {
		return false;
	}
}

void
EngineHost::refresh_region_flags ()
{
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_snap.track_has_regions[i] = false;
	}
	if (!_impl || !_impl->session) {
		return;
	}
	try {
		for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
			if (i >= (int)_impl->tracks.size () || !_impl->tracks[i]) {
				continue;
			}
			std::shared_ptr<Playlist> pl = _impl->tracks[i]->playlist ();
			if (!pl) {
				continue;
			}
			std::shared_ptr<RegionList> regions = pl->region_list ();
			_snap.track_has_regions[i] = (regions && !regions->empty ());
		}
	} catch (...) {
	}
	refresh_freeze_flags ();
}

void
EngineHost::refresh_freeze_flags ()
{
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_snap.track_frozen[i] = false;
	}
	if (!_impl || !_impl->session) {
		return;
	}
	try {
		for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
			if (i >= (int)_impl->tracks.size () || !_impl->tracks[i]) {
				continue;
			}
			_snap.track_frozen[i] =
				(_impl->tracks[i]->freeze_state () == Track::Frozen);
		}
	} catch (...) {
	}
}

bool
EngineHost::fill_track_waveform (int track,
                                 float view_start_sec,
                                 float view_span_sec,
                                 float* peaks,
                                 float* rms,
                                 size_t n_columns)
{
	if (peaks && n_columns > 0) {
		std::memset (peaks, 0, sizeof (float) * n_columns);
	}
	if (rms && n_columns > 0) {
		std::memset (rms, 0, sizeof (float) * n_columns);
	}
	if (track < 0 || track >= EngineSnapshot::kMeters || !peaks || n_columns == 0) {
		return false;
	}
	if (!_impl || !_impl->session || !_snap.session_ok) {
		return false;
	}
	if (track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}

	try {
		std::shared_ptr<Playlist> pl = _impl->tracks[track]->playlist ();
		if (!pl) {
			_snap.track_has_regions[track] = false;
			return false;
		}

		std::shared_ptr<RegionList> all = pl->region_list ();
		const bool has_any = (all && !all->empty ());
		_snap.track_has_regions[track] = has_any;
		if (!has_any) {
			return false;
		}

		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
		if (sr <= 0 || view_span_sec <= 0.f) {
			return has_any;
		}

		samplepos_t view_start = (samplepos_t)std::llround ((double)view_start_sec * (double)sr);
		samplecnt_t view_span = (samplecnt_t)std::llround ((double)view_span_sec * (double)sr);
		if (view_span < (samplecnt_t)n_columns) {
			view_span = (samplecnt_t)n_columns;
		}
		samplepos_t view_end = view_start + view_span;

		std::shared_ptr<RegionList> touched = pl->regions_touched (
			timepos_t (view_start), timepos_t (view_end));
		if (!touched || touched->empty ()) {
			return has_any;
		}

		/* Reuse scratch — avoid per-tick heap traffic on the UI thread. */
		static thread_local std::vector<PeakData> buf;
		if (buf.size () < n_columns) {
			buf.resize (n_columns);
		}
		const double samples_per_col = (double)view_span / (double)n_columns;

		for (RegionList::const_iterator it = touched->begin (); it != touched->end (); ++it) {
			std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (*it);
			if (!ar) {
				continue;
			}

			samplepos_t rpos = ar->position_sample ();
			samplecnt_t rlen = ar->length_samples ();
			samplepos_t rsrc = ar->start_sample ();
			samplepos_t rend = rpos + rlen;

			samplepos_t ov0 = std::max (view_start, rpos);
			samplepos_t ov1 = std::min (view_end, rend);
			if (ov0 >= ov1) {
				continue;
			}

			samplecnt_t ov_len = ov1 - ov0;
			size_t c0 = (size_t)((ov0 - view_start) / samples_per_col);
			size_t c1 = (size_t)((ov1 - view_start + (samplecnt_t)samples_per_col - 1) / samples_per_col);
			if (c0 >= n_columns) {
				continue;
			}
			c1 = std::min (c1, n_columns);
			if (c1 <= c0) {
				c1 = c0 + 1;
			}
			size_t ncols = c1 - c0;

			samplepos_t src_off = rsrc + (ov0 - rpos);
			double spp = (double)ov_len / (double)ncols;
			if (spp < 1.0) {
				spp = 1.0;
			}

			if (buf.size () < ncols) {
				buf.resize (ncols);
			}
			std::memset (buf.data (), 0, sizeof (PeakData) * ncols);
			samplecnt_t got = ar->read_peaks (buf.data (), (samplecnt_t)ncols, src_off, ov_len, 0, spp);
			if (got == 0) {
				continue;
			}

			for (size_t i = 0; i < ncols; ++i) {
				float lo = buf[i].min;
				float hi = buf[i].max;
				float peak = std::max (std::fabs (lo), std::fabs (hi));
				peak = std::max (0.f, std::min (1.f, peak));
				size_t col = c0 + i;
				if (col >= n_columns) {
					break;
				}
				if (peak > peaks[col]) {
					peaks[col] = peak;
				}
				float approx_rms = peak * 0.55f;
				if (rms && approx_rms > rms[col]) {
					rms[col] = approx_rms;
				}
			}
		}
		return has_any;
	} catch (...) {
		if (peaks && n_columns > 0) {
			std::memset (peaks, 0, sizeof (float) * n_columns);
		}
		if (rms && n_columns > 0) {
			std::memset (rms, 0, sizeof (float) * n_columns);
		}
		return false;
	}
}

void
EngineHost::refresh_record_flags ()
{
	_snap.session_record_armed = false;
	_snap.session_recording = false;
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_snap.track_armed[i] = false;
	}
	if (!_impl || !_impl->session) {
		return;
	}
	try {
		_snap.session_record_armed = _impl->session->get_record_enabled ();
		_snap.session_recording = _impl->session->actively_recording ();
		for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
			if (i < (int)_impl->tracks.size () && _impl->tracks[i]
			    && _impl->tracks[i]->rec_enable_control ()) {
				_snap.track_armed[i] =
					_impl->tracks[i]->rec_enable_control ()->get_value () >= 0.5;
			}
		}
	} catch (...) {
	}
}

bool
EngineHost::set_track_armed (int track, bool armed)
{
	if (!_impl || !_impl->session || track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	if (track >= (int)_impl->tracks.size () || !_impl->tracks[track]
	    || !_impl->tracks[track]->rec_enable_control ()) {
		return false;
	}
	try {
		_impl->tracks[track]->rec_enable_control ()->set_value (
			armed ? 1.0 : 0.0, Controllable::NoGroup);
		refresh_record_flags ();
		_snap.detail = armed ? L"track armed for record" : L"track disarmed";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_track_armed (int track)
{
	if (track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	return set_track_armed (track, !_snap.track_armed[track]);
}

void
EngineHost::refresh_mute_solo_flags ()
{
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_snap.track_muted[i] = false;
		_snap.track_soloed[i] = false;
	}
	_snap.midi_muted = false;
	_snap.midi_soloed = false;
	if (!_impl) {
		return;
	}
	try {
		for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
			if (i < (int)_impl->tracks.size () && _impl->tracks[i]) {
				std::shared_ptr<Route> r = _impl->tracks[i];
				if (r->mute_control ()) {
					_snap.track_muted[i] = r->muted ();
				}
				if (r->solo_control ()) {
					_snap.track_soloed[i] = r->soloed ();
				}
			}
		}
		if (_impl->midi_track) {
			std::shared_ptr<Route> r = _impl->midi_track;
			if (r->mute_control ()) {
				_snap.midi_muted = r->muted ();
			}
			if (r->solo_control ()) {
				_snap.midi_soloed = r->soloed ();
			}
		}
	} catch (...) {
	}
}

/** Mute/Solo are Controllable::RealTime — applied on the process thread. */
static bool
wait_until (std::function<bool ()> pred, int timeout_ms = 400)
{
	const int step = 20;
	for (int waited = 0; waited < timeout_ms; waited += step) {
		if (pred ()) {
			return true;
		}
		Sleep ((DWORD)step);
	}
	return pred ();
}

bool
EngineHost::set_track_muted (int track, bool muted)
{
	if (!_impl || !_impl->session || track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	if (track >= (int)_impl->tracks.size () || !_impl->tracks[track]
	    || !_impl->tracks[track]->mute_control ()) {
		return false;
	}
	try {
		std::shared_ptr<MuteControl> mc = _impl->tracks[track]->mute_control ();
		_impl->session->set_control (mc, muted ? 1.0 : 0.0, Controllable::UseGroup);
		wait_until ([&] () {
			refresh_mute_solo_flags ();
			return _snap.track_muted[track] == muted;
		});
		_snap.detail = muted ? L"track muted" : L"track unmuted";
		return _snap.track_muted[track] == muted;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_track_muted (int track)
{
	if (track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	refresh_mute_solo_flags ();
	return set_track_muted (track, !_snap.track_muted[track]);
}

bool
EngineHost::set_track_soloed (int track, bool soloed)
{
	if (!_impl || !_impl->session || track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	if (track >= (int)_impl->tracks.size () || !_impl->tracks[track]
	    || !_impl->tracks[track]->solo_control ()) {
		return false;
	}
	try {
		std::shared_ptr<SoloControl> sc = _impl->tracks[track]->solo_control ();
		_impl->session->set_control (sc, soloed ? 1.0 : 0.0, Controllable::UseGroup);
		wait_until ([&] () {
			refresh_mute_solo_flags ();
			return _snap.track_soloed[track] == soloed;
		});
		_snap.detail = soloed ? L"track soloed" : L"track unsoloed";
		return _snap.track_soloed[track] == soloed;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_track_soloed (int track)
{
	if (track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	refresh_mute_solo_flags ();
	return set_track_soloed (track, !_snap.track_soloed[track]);
}

bool
EngineHost::set_midi_muted (bool muted)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	ensure_midi_track ();
	if (!_impl->midi_track || !_impl->midi_track->mute_control ()) {
		return false;
	}
	try {
		std::shared_ptr<MuteControl> mc = _impl->midi_track->mute_control ();
		_impl->session->set_control (mc, muted ? 1.0 : 0.0, Controllable::NoGroup);
		wait_until ([&] () {
			refresh_mute_solo_flags ();
			return _snap.midi_muted == muted;
		});
		_snap.detail = muted ? L"MIDI muted" : L"MIDI unmuted";
		return _snap.midi_muted == muted;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_midi_muted ()
{
	refresh_mute_solo_flags ();
	return set_midi_muted (!_snap.midi_muted);
}

bool
EngineHost::set_midi_soloed (bool soloed)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	ensure_midi_track ();
	if (!_impl->midi_track || !_impl->midi_track->solo_control ()) {
		return false;
	}
	try {
		std::shared_ptr<SoloControl> sc = _impl->midi_track->solo_control ();
		_impl->session->set_control (sc, soloed ? 1.0 : 0.0, Controllable::NoGroup);
		wait_until ([&] () {
			refresh_mute_solo_flags ();
			return _snap.midi_soloed == soloed;
		});
		_snap.detail = soloed ? L"MIDI soloed" : L"MIDI unsoloed";
		return _snap.midi_soloed == soloed;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_midi_soloed ()
{
	refresh_mute_solo_flags ();
	return set_midi_soloed (!_snap.midi_soloed);
}

void
EngineHost::refresh_pan_flags ()
{
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_snap.track_pan[i] = 0.5f;
	}
	if (!_impl) {
		return;
	}
	try {
		for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
			if (i < (int)_impl->tracks.size () && _impl->tracks[i]) {
				std::shared_ptr<AutomationControl> pan =
					_impl->tracks[i]->pan_azimuth_control ();
				if (pan) {
					_snap.track_pan[i] = (float)pan->get_value ();
				}
			}
		}
	} catch (...) {
	}
}

bool
EngineHost::set_track_pan (int track, float pan_0_1)
{
	pan_0_1 = std::max (0.f, std::min (1.f, pan_0_1));
	if (!_impl || !_impl->session || track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	if (track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	try {
		std::shared_ptr<AutomationControl> pan =
			_impl->tracks[track]->pan_azimuth_control ();
		if (!pan) {
			std::fprintf (stderr, "airdour_win_shell: pan missing on Audio %d (panner?)\n",
			              track + 1);
			std::fflush (stderr);
			return false;
		}
		_impl->session->set_control (pan, (double)pan_0_1, Controllable::NoGroup);
		/* Pan is usually immediate; poll briefly for RT paths. */
		wait_until ([&] () {
			refresh_pan_flags ();
			return std::fabs (_snap.track_pan[track] - pan_0_1) < 0.02f;
		}, 300);
		_snap.detail = L"track pan set";
		return std::fabs (_snap.track_pan[track] - pan_0_1) < 0.02f;
	} catch (...) {
		return false;
	}
}

float
EngineHost::track_pan (int track) const
{
	if (track < 0 || track >= EngineSnapshot::kMeters) {
		return 0.5f;
	}
	return _snap.track_pan[track];
}

bool
EngineHost::smoke_pan ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-pan failed — no Session";
		return false;
	}
	try {
		collect_tracks ();
		if (_impl->tracks.empty () || !_impl->tracks[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-pan FAILED (no Audio 1)\n");
			std::fflush (stderr);
			return false;
		}
		if (!_impl->tracks[0]->pan_azimuth_control ()) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-pan FAILED "
			              "(no pan_azimuth — check panner DLLs)\n");
			std::fflush (stderr);
			return false;
		}

		if (!set_track_pan (0, 0.0f) || _snap.track_pan[0] > 0.05f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-pan FAILED (pan L got=%.3f)\n",
			              _snap.track_pan[0]);
			std::fflush (stderr);
			return false;
		}
		if (!set_track_pan (0, 1.0f) || _snap.track_pan[0] < 0.95f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-pan FAILED (pan R got=%.3f)\n",
			              _snap.track_pan[0]);
			std::fflush (stderr);
			return false;
		}
		if (!set_track_pan (0, 0.5f)
		    || _snap.track_pan[0] < 0.45f || _snap.track_pan[0] > 0.55f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-pan FAILED (pan C got=%.3f)\n",
			              _snap.track_pan[0]);
			std::fflush (stderr);
			return false;
		}

		std::fprintf (stderr, "airdour_win_shell: smoke-pan ok (Audio1 L→R→C)\n");
		std::fflush (stderr);
		_snap.detail = L"smoke-pan ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pan exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

void
EngineHost::refresh_send_flags ()
{
	_snap.aux_bus_ok = false;
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_snap.track_send[i] = 0.f;
		_snap.track_send_ok[i] = false;
	}
	if (!_impl || !_impl->session) {
		return;
	}
	try {
		_snap.aux_bus_ok = (_impl->aux_bus != nullptr);
		if (!_impl->aux_bus) {
			return;
		}
		for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
			if (i >= (int)_impl->tracks.size () || !_impl->tracks[i]) {
				continue;
			}
			std::shared_ptr<Send> send =
				_impl->tracks[i]->internal_send_for (_impl->aux_bus);
			if (!send || !send->gain_control ()) {
				continue;
			}
			_snap.track_send_ok[i] = true;
			float g = (float)send->gain_control ()->get_value ();
			_snap.track_send[i] = std::max (0.f, std::min (1.f, g));
		}
	} catch (...) {
	}
}

bool
EngineHost::ensure_aux_sends ()
{
	if (!_impl || !_impl->session) {
		return false;
	}
	try {
		collect_tracks ();
		if (!_impl->aux_bus) {
			RouteList buses = _impl->session->new_audio_route (
				2, 2,
				std::shared_ptr<RouteGroup> (),
				1,
				"Aux",
				PresentationInfo::AudioBus,
				PresentationInfo::max_order);
			if (buses.empty ()) {
				std::fprintf (stderr, "airdour_win_shell: Aux bus create failed\n");
				std::fflush (stderr);
				refresh_send_flags ();
				return false;
			}
			_impl->aux_bus = buses.front ();
			std::fprintf (stderr, "airdour_win_shell: Aux bus \"%s\" ready\n",
			              _impl->aux_bus->name ().c_str ());
			std::fflush (stderr);
		}

		for (size_t i = 0; i < _impl->tracks.size () && i < (size_t)EngineSnapshot::kMeters; ++i) {
			std::shared_ptr<AudioTrack> track = _impl->tracks[i];
			if (!track) {
				continue;
			}
			std::shared_ptr<Send> existing = track->internal_send_for (_impl->aux_bus);
			if (!existing) {
				std::shared_ptr<Processor> before =
					track->before_processor_for_placement (PostFader);
				_impl->session->add_internal_send (_impl->aux_bus, before, track);
				existing = track->internal_send_for (_impl->aux_bus);
				if (existing && existing->gain_control ()) {
					/* Start silent so Send is opt-in from the mixer. */
					_impl->session->set_control (
						existing->gain_control (), 0.0, Controllable::NoGroup);
				}
			}
		}
		refresh_send_flags ();
		return _snap.aux_bus_ok && _snap.track_send_ok[0];
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: ensure_aux_sends: %s\n", e.what ());
		std::fflush (stderr);
		refresh_send_flags ();
		return false;
	} catch (...) {
		refresh_send_flags ();
		return false;
	}
}

bool
EngineHost::set_track_send (int track, float send_0_1)
{
	send_0_1 = std::max (0.f, std::min (1.f, send_0_1));
	if (!_impl || !_impl->session || track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	if (!ensure_aux_sends ()) {
		return false;
	}
	if (track >= (int)_impl->tracks.size () || !_impl->tracks[track] || !_impl->aux_bus) {
		return false;
	}
	try {
		std::shared_ptr<Send> send =
			_impl->tracks[track]->internal_send_for (_impl->aux_bus);
		if (!send || !send->gain_control ()) {
			std::fprintf (stderr, "airdour_win_shell: send missing on Audio %d\n",
			              track + 1);
			std::fflush (stderr);
			return false;
		}
		_impl->session->set_control (send->gain_control (), (double)send_0_1,
		                             Controllable::NoGroup);
		wait_until ([&] () {
			refresh_send_flags ();
			return std::fabs (_snap.track_send[track] - send_0_1) < 0.02f;
		}, 300);
		_snap.detail = L"track send set";
		return std::fabs (_snap.track_send[track] - send_0_1) < 0.02f;
	} catch (...) {
		return false;
	}
}

float
EngineHost::track_send (int track) const
{
	if (track < 0 || track >= EngineSnapshot::kMeters) {
		return 0.f;
	}
	return _snap.track_send[track];
}

bool
EngineHost::smoke_send ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-send failed — no Session";
		return false;
	}
	try {
		if (!ensure_aux_sends () || !_snap.aux_bus_ok || !_snap.track_send_ok[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-send FAILED (no Aux/send)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_track_send (0, 0.0f) || _snap.track_send[0] > 0.05f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-send FAILED (0 got=%.3f)\n",
			              _snap.track_send[0]);
			std::fflush (stderr);
			return false;
		}
		if (!set_track_send (0, 0.75f)
		    || _snap.track_send[0] < 0.70f || _snap.track_send[0] > 0.80f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-send FAILED (0.75 got=%.3f)\n",
			              _snap.track_send[0]);
			std::fflush (stderr);
			return false;
		}
		if (!set_track_send (0, 0.25f)
		    || _snap.track_send[0] < 0.20f || _snap.track_send[0] > 0.30f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-send FAILED (0.25 got=%.3f)\n",
			              _snap.track_send[0]);
			std::fflush (stderr);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-send ok (Audio1 → Aux 0→0.75→0.25)\n");
		std::fflush (stderr);
		_snap.detail = L"smoke-send ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-send exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::ensure_track_gain_automation (int track)
{
	if (!_impl || !_impl->session || track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	if (track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		collect_tracks ();
	}
	if (track >= (int)_impl->tracks.size () || !_impl->tracks[track]
	    || !_impl->tracks[track]->gain_control ()) {
		return false;
	}
	try {
		std::shared_ptr<AutomationControl> gc = _impl->tracks[track]->gain_control ();
		std::shared_ptr<AutomationList> al = gc->alist ();
		if (!al) {
			return false;
		}
		gc->set_automation_state (Play);
		_snap.detail = L"gain automation Play";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::clear_track_gain_automation (int track)
{
	if (!_impl || !_impl->session || track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	if (track >= (int)_impl->tracks.size () || !_impl->tracks[track]
	    || !_impl->tracks[track]->gain_control ()) {
		return false;
	}
	try {
		std::shared_ptr<AutomationList> al = _impl->tracks[track]->gain_control ()->alist ();
		if (!al) {
			return false;
		}
		al->clear ();
		_snap.detail = L"gain automation cleared";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::write_track_gain_automation (int track, float time_sec, float value_0_1)
{
	value_0_1 = std::max (0.f, std::min (1.f, value_0_1));
	time_sec = snap_time (time_sec);
	if (time_sec < 0.f) {
		time_sec = 0.f;
	}
	if (!ensure_track_gain_automation (track)) {
		return false;
	}
	try {
		std::shared_ptr<AutomationControl> gc = _impl->tracks[track]->gain_control ();
		std::shared_ptr<AutomationList> al = gc->alist ();
		if (!al) {
			return false;
		}
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0 && _impl->engine) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
		if (sr <= 0) {
			sr = 48000;
		}
		samplepos_t samples = (samplepos_t)std::llround ((double)time_sec * (double)sr);
		bool added = al->editor_add (timepos_t (samples), (double)value_0_1, true);
		if (!added) {
			/* Point already at this time — modify via add which replaces. */
			al->add (timepos_t (samples), (double)value_0_1, true, true);
		}
		_snap.detail = L"gain automation point";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::fill_gain_automation_curve (int track,
                                        float view_start_sec,
                                        float view_span_sec,
                                        float* y_0_1,
                                        size_t n)
{
	if (!y_0_1 || n == 0) {
		return false;
	}
	for (size_t i = 0; i < n; ++i) {
		y_0_1[i] = 0.5f;
	}
	if (!_impl || !_impl->session || track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	if (track >= (int)_impl->tracks.size () || !_impl->tracks[track]
	    || !_impl->tracks[track]->gain_control ()) {
		return false;
	}
	try {
		std::shared_ptr<AutomationList> al = _impl->tracks[track]->gain_control ()->alist ();
		if (!al || al->empty ()) {
			float g = _track_gain[track];
			for (size_t i = 0; i < n; ++i) {
				y_0_1[i] = g;
			}
			return true;
		}
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0 && _impl->engine) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
		if (sr <= 0) {
			sr = 48000;
		}
		float span = std::max (0.001f, view_span_sec);
		for (size_t i = 0; i < n; ++i) {
			float t = view_start_sec + ((float)i + 0.5f) / (float)n * span;
			if (t < 0.f) {
				t = 0.f;
			}
			samplepos_t samples = (samplepos_t)std::llround ((double)t * (double)sr);
			float v = (float)al->eval (timepos_t (samples));
			y_0_1[i] = std::max (0.f, std::min (1.f, v));
		}
		return true;
	} catch (...) {
		return false;
	}
}

int
EngineHost::fill_gain_automation_points (int track,
                                         float view_start_sec,
                                         float view_span_sec,
                                         float* x_norm,
                                         float* y_0_1,
                                         int max_n)
{
	if (!x_norm || !y_0_1 || max_n <= 0) {
		return 0;
	}
	if (!_impl || !_impl->session || track < 0 || track >= EngineSnapshot::kMeters) {
		return 0;
	}
	if (track >= (int)_impl->tracks.size () || !_impl->tracks[track]
	    || !_impl->tracks[track]->gain_control ()) {
		return 0;
	}
	try {
		std::shared_ptr<AutomationList> al = _impl->tracks[track]->gain_control ()->alist ();
		if (!al || al->empty ()) {
			return 0;
		}
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0 && _impl->engine) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
		if (sr <= 0) {
			sr = 48000;
		}
		float span = std::max (0.001f, view_span_sec);
		float view_end = view_start_sec + span;
		int written = 0;
		for (AutomationList::const_iterator it = al->begin ();
		     it != al->end () && written < max_n; ++it) {
			if (!*it) {
				continue;
			}
			double sec = (*it)->when.samples () / (double)sr;
			if (sec < (double)view_start_sec || sec > (double)view_end) {
				continue;
			}
			x_norm[written] = (float)((sec - (double)view_start_sec) / (double)span);
			y_0_1[written] = std::max (0.f, std::min (1.f, (float)(*it)->value));
			++written;
		}
		return written;
	} catch (...) {
		return 0;
	}
}

bool
EngineHost::smoke_automation ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-automation failed — no Session";
		return false;
	}
	try {
		collect_tracks ();
		if (_impl->tracks.empty () || !_impl->tracks[0] || !_impl->tracks[0]->gain_control ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-automation FAILED (no Audio 1 gain)\n");
			std::fflush (stderr);
			return false;
		}
		if (!clear_track_gain_automation (0)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-automation FAILED (clear)\n");
			std::fflush (stderr);
			return false;
		}
		if (!write_track_gain_automation (0, 0.0f, 0.20f)
		    || !write_track_gain_automation (0, 2.0f, 0.90f)
		    || !write_track_gain_automation (0, 4.0f, 0.40f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-automation FAILED (write points)\n");
			std::fflush (stderr);
			return false;
		}
		std::shared_ptr<AutomationList> al = _impl->tracks[0]->gain_control ()->alist ();
		if (!al || al->size () < 3) {
			std::fprintf (stderr, "airdour_win_shell: smoke-automation FAILED (size=%u)\n",
			              al ? (unsigned)al->size () : 0u);
			std::fflush (stderr);
			return false;
		}
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0) {
			sr = 48000;
		}
		float mid = (float)al->eval (timepos_t ((samplepos_t)(2 * sr)));
		if (mid < 0.85f || mid > 0.95f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-automation FAILED (eval@2s=%.3f)\n",
			              mid);
			std::fflush (stderr);
			return false;
		}
		if (_impl->tracks[0]->gain_control ()->automation_state () != Play) {
			std::fprintf (stderr, "airdour_win_shell: smoke-automation FAILED (state!=Play)\n");
			std::fflush (stderr);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-automation ok (Audio1 gain pts=%u eval@2s=%.2f Play)\n",
		              (unsigned)al->size (), mid);
		std::fflush (stderr);
		_snap.detail = L"smoke-automation ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-automation exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

namespace {

int
clamp_midi_channel (int channel)
{
	if (channel < 0) {
		return 0;
	}
	if (channel > 15) {
		return 15;
	}
	return channel;
}

std::shared_ptr<AutomationList>
midi_cc_alist (std::shared_ptr<MidiRegion> region, int cc, int channel)
{
	if (!region) {
		return std::shared_ptr<AutomationList> ();
	}
	std::shared_ptr<MidiModel> model = region->model ();
	if (!model) {
		return std::shared_ptr<AutomationList> ();
	}
	if (cc < 0) {
		cc = 0;
	}
	if (cc > 127) {
		cc = 127;
	}
	channel = clamp_midi_channel (channel);
	Evoral::Parameter param (MidiCCAutomation, (uint8_t)channel, (uint32_t)cc);
	std::shared_ptr<AutomationControl> ac = model->automation_control (param, true);
	if (!ac) {
		return std::shared_ptr<AutomationList> ();
	}
	return ac->alist ();
}

std::shared_ptr<AutomationList>
midi_bend_alist (std::shared_ptr<MidiRegion> region, int channel)
{
	if (!region) {
		return std::shared_ptr<AutomationList> ();
	}
	std::shared_ptr<MidiModel> model = region->model ();
	if (!model) {
		return std::shared_ptr<AutomationList> ();
	}
	channel = clamp_midi_channel (channel);
	Evoral::Parameter param (MidiPitchBenderAutomation, (uint8_t)channel, 0);
	std::shared_ptr<AutomationControl> ac = model->automation_control (param, true);
	if (!ac) {
		return std::shared_ptr<AutomationList> ();
	}
	return ac->alist ();
}

std::shared_ptr<AutomationList>
midi_pressure_alist (std::shared_ptr<MidiRegion> region, int channel)
{
	if (!region) {
		return std::shared_ptr<AutomationList> ();
	}
	std::shared_ptr<MidiModel> model = region->model ();
	if (!model) {
		return std::shared_ptr<AutomationList> ();
	}
	channel = clamp_midi_channel (channel);
	Evoral::Parameter param (MidiChannelPressureAutomation, (uint8_t)channel, 0);
	std::shared_ptr<AutomationControl> ac = model->automation_control (param, true);
	if (!ac) {
		return std::shared_ptr<AutomationList> ();
	}
	return ac->alist ();
}

} /* anonymous */

bool
EngineHost::ensure_midi_cc_automation (int cc, int channel)
{
	if (!ensure_midi_region () || !_impl || !_impl->session) {
		return false;
	}
	try {
		std::shared_ptr<AutomationList> al = midi_cc_alist (_impl->midi_region, cc, channel);
		if (!al) {
			return false;
		}
		al->set_automation_state (Play);
		_snap.detail = L"MIDI CC automation Play";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::clear_midi_cc_automation (int cc, int channel)
{
	if (!ensure_midi_region () || !_impl || !_impl->session) {
		return false;
	}
	try {
		std::shared_ptr<AutomationList> al = midi_cc_alist (_impl->midi_region, cc, channel);
		if (!al) {
			return false;
		}
		al->clear ();
		_snap.detail = L"MIDI CC automation cleared";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::write_midi_cc_automation (float time_sec, float value_0_1, int cc, int channel)
{
	value_0_1 = std::max (0.f, std::min (1.f, value_0_1));
	time_sec = snap_time (time_sec);
	if (time_sec < 0.f) {
		time_sec = 0.f;
	}
	if (!ensure_midi_cc_automation (cc, channel)) {
		return false;
	}
	try {
		std::shared_ptr<AutomationList> al = midi_cc_alist (_impl->midi_region, cc, channel);
		if (!al) {
			return false;
		}
		/* Fixed 120 BPM matches add_midi_note / MidiModel BeatTime domain. */
		const double beats_per_sec = 120.0 / 60.0;
		Temporal::Beats beats =
		    Temporal::Beats::from_double ((double)time_sec * beats_per_sec);
		if (beats < Temporal::Beats ()) {
			beats = Temporal::Beats ();
		}
		double cc_val = (double)value_0_1 * 127.0;
		bool added = al->editor_add (timepos_t (beats), cc_val, true);
		if (!added) {
			al->add (timepos_t (beats), cc_val, true, true);
		}
		_snap.detail = L"MIDI CC automation point";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::fill_midi_cc_automation_curve (float view_start_sec,
                                          float view_span_sec,
                                          float* y_0_1,
                                          size_t n,
                                          int cc,
                                          int channel)
{
	if (!y_0_1 || n == 0) {
		return false;
	}
	const float def = 100.f / 127.f;
	for (size_t i = 0; i < n; ++i) {
		y_0_1[i] = def;
	}
	if (!_impl || !_impl->session || !_impl->midi_region) {
		return false;
	}
	try {
		std::shared_ptr<AutomationList> al = midi_cc_alist (_impl->midi_region, cc, channel);
		if (!al || al->empty ()) {
			return true;
		}
		const double beats_per_sec = 120.0 / 60.0;
		float span = std::max (0.001f, view_span_sec);
		for (size_t i = 0; i < n; ++i) {
			float t = view_start_sec + ((float)i + 0.5f) / (float)n * span;
			if (t < 0.f) {
				t = 0.f;
			}
			Temporal::Beats beats =
			    Temporal::Beats::from_double ((double)t * beats_per_sec);
			float v = (float)al->eval (timepos_t (beats));
			y_0_1[i] = std::max (0.f, std::min (1.f, v / 127.f));
		}
		return true;
	} catch (...) {
		return false;
	}
}

int
EngineHost::fill_midi_cc_automation_points (float view_start_sec,
                                           float view_span_sec,
                                           float* x_norm,
                                           float* y_0_1,
                                           int max_n,
                                           int cc,
                                           int channel)
{
	if (!x_norm || !y_0_1 || max_n <= 0) {
		return 0;
	}
	if (!_impl || !_impl->session || !_impl->midi_region) {
		return 0;
	}
	try {
		std::shared_ptr<AutomationList> al = midi_cc_alist (_impl->midi_region, cc, channel);
		if (!al || al->empty ()) {
			return 0;
		}
		const double beats_per_sec = 120.0 / 60.0;
		float span = std::max (0.001f, view_span_sec);
		float view_end = view_start_sec + span;
		int written = 0;
		for (AutomationList::const_iterator it = al->begin ();
		     it != al->end () && written < max_n; ++it) {
			if (!*it) {
				continue;
			}
			double sec = Temporal::DoubleableBeats ((*it)->when.beats ()).to_double ()
			             / beats_per_sec;
			if (sec < (double)view_start_sec || sec > (double)view_end) {
				continue;
			}
			x_norm[written] = (float)((sec - (double)view_start_sec) / (double)span);
			y_0_1[written] = std::max (0.f, std::min (1.f, (float)((*it)->value / 127.0)));
			++written;
		}
		return written;
	} catch (...) {
		return 0;
	}
}

bool
EngineHost::smoke_midi_cc ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-midi-cc failed — no Session";
		return false;
	}
	try {
		if (!ensure_midi_track () || !ensure_midi_region (16.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc FAILED (region)\n");
			std::fflush (stderr);
			return false;
		}
		if (!clear_midi_cc_automation ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc FAILED (clear)\n");
			std::fflush (stderr);
			return false;
		}
		if (!write_midi_cc_automation (0.0f, 0.20f)
		    || !write_midi_cc_automation (2.0f, 0.90f)
		    || !write_midi_cc_automation (4.0f, 0.40f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc FAILED (write points)\n");
			std::fflush (stderr);
			return false;
		}
		std::shared_ptr<AutomationList> al = midi_cc_alist (_impl->midi_region, kMidiCcVolume, 0);
		if (!al || al->size () < 3) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc FAILED (size=%u)\n",
			              al ? (unsigned)al->size () : 0u);
			std::fflush (stderr);
			return false;
		}
		const double beats_per_sec = 120.0 / 60.0;
		Temporal::Beats mid_b = Temporal::Beats::from_double (2.0 * beats_per_sec);
		float mid = (float)al->eval (timepos_t (mid_b));
		/* 0.90 * 127 ≈ 114.3 */
		if (mid < 110.f || mid > 119.f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc FAILED (eval@2s=%.3f)\n",
			              mid);
			std::fflush (stderr);
			return false;
		}
		if (al->automation_state () != Play) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc FAILED (state!=Play)\n");
			std::fflush (stderr);
			return false;
		}
		/* Also verify CC1 (mod) list is independent. */
		if (!clear_midi_cc_automation (1)
		    || !write_midi_cc_automation (1.0f, 0.50f, 1)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc FAILED (CC1 write)\n");
			std::fflush (stderr);
			return false;
		}
		std::shared_ptr<AutomationList> al1 = midi_cc_alist (_impl->midi_region, 1, 0);
		if (!al1 || al1->empty ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc FAILED (CC1 empty)\n");
			std::fflush (stderr);
			return false;
		}
		Temporal::Beats one_b = Temporal::Beats::from_double (1.0 * beats_per_sec);
		float cc1 = (float)al1->eval (timepos_t (one_b));
		if (cc1 < 60.f || cc1 > 70.f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc FAILED (CC1 eval=%.3f)\n",
			              cc1);
			std::fflush (stderr);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-cc ok (CC7 pts=%u eval@2s=%.1f Play; CC1@1s=%.1f)\n",
		              (unsigned)al->size (), mid, cc1);
		std::fflush (stderr);
		_snap.detail = L"smoke-midi-cc ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::ensure_midi_bend_automation (int channel)
{
	if (!ensure_midi_region () || !_impl || !_impl->session) {
		return false;
	}
	try {
		std::shared_ptr<AutomationList> al = midi_bend_alist (_impl->midi_region, channel);
		if (!al) {
			return false;
		}
		al->set_automation_state (Play);
		_snap.detail = L"MIDI bend automation Play";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::clear_midi_bend_automation (int channel)
{
	if (!ensure_midi_region () || !_impl || !_impl->session) {
		return false;
	}
	try {
		std::shared_ptr<AutomationList> al = midi_bend_alist (_impl->midi_region, channel);
		if (!al) {
			return false;
		}
		al->clear ();
		_snap.detail = L"MIDI bend automation cleared";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::write_midi_bend_automation (float time_sec, float value_0_1, int channel)
{
	value_0_1 = std::max (0.f, std::min (1.f, value_0_1));
	time_sec = snap_time (time_sec);
	if (time_sec < 0.f) {
		time_sec = 0.f;
	}
	if (!ensure_midi_bend_automation (channel)) {
		return false;
	}
	try {
		std::shared_ptr<AutomationList> al = midi_bend_alist (_impl->midi_region, channel);
		if (!al) {
			return false;
		}
		const double beats_per_sec = 120.0 / 60.0;
		Temporal::Beats beats =
		    Temporal::Beats::from_double ((double)time_sec * beats_per_sec);
		if (beats < Temporal::Beats ()) {
			beats = Temporal::Beats ();
		}
		double bend_val = (double)value_0_1 * 16383.0;
		bool added = al->editor_add (timepos_t (beats), bend_val, true);
		if (!added) {
			al->add (timepos_t (beats), bend_val, true, true);
		}
		_snap.detail = L"MIDI bend automation point";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::fill_midi_bend_automation_curve (float view_start_sec,
                                            float view_span_sec,
                                            float* y_0_1,
                                            size_t n,
                                            int channel)
{
	if (!y_0_1 || n == 0) {
		return false;
	}
	const float def = 0.5f; /* center */
	for (size_t i = 0; i < n; ++i) {
		y_0_1[i] = def;
	}
	if (!_impl || !_impl->session || !_impl->midi_region) {
		return false;
	}
	try {
		std::shared_ptr<AutomationList> al = midi_bend_alist (_impl->midi_region, channel);
		if (!al || al->empty ()) {
			return true;
		}
		const double beats_per_sec = 120.0 / 60.0;
		float span = std::max (0.001f, view_span_sec);
		for (size_t i = 0; i < n; ++i) {
			float t = view_start_sec + ((float)i + 0.5f) / (float)n * span;
			if (t < 0.f) {
				t = 0.f;
			}
			Temporal::Beats beats =
			    Temporal::Beats::from_double ((double)t * beats_per_sec);
			float v = (float)al->eval (timepos_t (beats));
			y_0_1[i] = std::max (0.f, std::min (1.f, v / 16383.f));
		}
		return true;
	} catch (...) {
		return false;
	}
}

int
EngineHost::fill_midi_bend_automation_points (float view_start_sec,
                                             float view_span_sec,
                                             float* x_norm,
                                             float* y_0_1,
                                             int max_n,
                                             int channel)
{
	if (!x_norm || !y_0_1 || max_n <= 0) {
		return 0;
	}
	if (!_impl || !_impl->session || !_impl->midi_region) {
		return 0;
	}
	try {
		std::shared_ptr<AutomationList> al = midi_bend_alist (_impl->midi_region, channel);
		if (!al || al->empty ()) {
			return 0;
		}
		const double beats_per_sec = 120.0 / 60.0;
		float span = std::max (0.001f, view_span_sec);
		float view_end = view_start_sec + span;
		int written = 0;
		for (AutomationList::const_iterator it = al->begin ();
		     it != al->end () && written < max_n; ++it) {
			if (!*it) {
				continue;
			}
			double sec = Temporal::DoubleableBeats ((*it)->when.beats ()).to_double ()
			             / beats_per_sec;
			if (sec < (double)view_start_sec || sec > (double)view_end) {
				continue;
			}
			x_norm[written] = (float)((sec - (double)view_start_sec) / (double)span);
			y_0_1[written] = std::max (0.f, std::min (1.f, (float)((*it)->value / 16383.0)));
			++written;
		}
		return written;
	} catch (...) {
		return 0;
	}
}

bool
EngineHost::smoke_midi_bend ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-midi-bend failed — no Session";
		return false;
	}
	try {
		if (!ensure_midi_track () || !ensure_midi_region (16.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-bend FAILED (region)\n");
			std::fflush (stderr);
			return false;
		}
		if (!clear_midi_bend_automation ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-bend FAILED (clear)\n");
			std::fflush (stderr);
			return false;
		}
		/* 0.0 → 0, 0.5 → ~8191.5, 1.0 → 16383 */
		if (!write_midi_bend_automation (0.0f, 0.0f)
		    || !write_midi_bend_automation (2.0f, 0.5f)
		    || !write_midi_bend_automation (4.0f, 1.0f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-bend FAILED (write points)\n");
			std::fflush (stderr);
			return false;
		}
		std::shared_ptr<AutomationList> al = midi_bend_alist (_impl->midi_region, 0);
		if (!al || al->size () < 3) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-bend FAILED (size=%u)\n",
			              al ? (unsigned)al->size () : 0u);
			std::fflush (stderr);
			return false;
		}
		const double beats_per_sec = 120.0 / 60.0;
		Temporal::Beats mid_b = Temporal::Beats::from_double (2.0 * beats_per_sec);
		float mid = (float)al->eval (timepos_t (mid_b));
		if (mid < 8000.f || mid > 8400.f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-bend FAILED (eval@2s=%.3f)\n",
			              mid);
			std::fflush (stderr);
			return false;
		}
		if (al->automation_state () != Play) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-bend FAILED (state!=Play)\n");
			std::fflush (stderr);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-bend ok (pts=%u eval@2s=%.1f Play)\n",
		              (unsigned)al->size (), mid);
		std::fflush (stderr);
		_snap.detail = L"smoke-midi-bend ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-bend exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::ensure_midi_pressure_automation (int channel)
{
	if (!ensure_midi_region () || !_impl || !_impl->session) {
		return false;
	}
	try {
		std::shared_ptr<AutomationList> al = midi_pressure_alist (_impl->midi_region, channel);
		if (!al) {
			return false;
		}
		al->set_automation_state (Play);
		_snap.detail = L"MIDI pressure automation Play";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::clear_midi_pressure_automation (int channel)
{
	if (!ensure_midi_region () || !_impl || !_impl->session) {
		return false;
	}
	try {
		std::shared_ptr<AutomationList> al = midi_pressure_alist (_impl->midi_region, channel);
		if (!al) {
			return false;
		}
		al->clear ();
		_snap.detail = L"MIDI pressure automation cleared";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::write_midi_pressure_automation (float time_sec, float value_0_1, int channel)
{
	value_0_1 = std::max (0.f, std::min (1.f, value_0_1));
	time_sec = snap_time (time_sec);
	if (time_sec < 0.f) {
		time_sec = 0.f;
	}
	if (!ensure_midi_pressure_automation (channel)) {
		return false;
	}
	try {
		std::shared_ptr<AutomationList> al = midi_pressure_alist (_impl->midi_region, channel);
		if (!al) {
			return false;
		}
		const double beats_per_sec = 120.0 / 60.0;
		Temporal::Beats beats =
		    Temporal::Beats::from_double ((double)time_sec * beats_per_sec);
		if (beats < Temporal::Beats ()) {
			beats = Temporal::Beats ();
		}
		double p_val = (double)value_0_1 * 127.0;
		bool added = al->editor_add (timepos_t (beats), p_val, true);
		if (!added) {
			al->add (timepos_t (beats), p_val, true, true);
		}
		_snap.detail = L"MIDI pressure automation point";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::fill_midi_pressure_automation_curve (float view_start_sec,
                                                float view_span_sec,
                                                float* y_0_1,
                                                size_t n,
                                                int channel)
{
	if (!y_0_1 || n == 0) {
		return false;
	}
	for (size_t i = 0; i < n; ++i) {
		y_0_1[i] = 0.f;
	}
	if (!_impl || !_impl->session || !_impl->midi_region) {
		return false;
	}
	try {
		std::shared_ptr<AutomationList> al = midi_pressure_alist (_impl->midi_region, channel);
		if (!al || al->empty ()) {
			return true;
		}
		const double beats_per_sec = 120.0 / 60.0;
		float span = std::max (0.001f, view_span_sec);
		for (size_t i = 0; i < n; ++i) {
			float t = view_start_sec + ((float)i + 0.5f) / (float)n * span;
			if (t < 0.f) {
				t = 0.f;
			}
			Temporal::Beats beats =
			    Temporal::Beats::from_double ((double)t * beats_per_sec);
			float v = (float)al->eval (timepos_t (beats));
			y_0_1[i] = std::max (0.f, std::min (1.f, v / 127.f));
		}
		return true;
	} catch (...) {
		return false;
	}
}

int
EngineHost::fill_midi_pressure_automation_points (float view_start_sec,
                                                 float view_span_sec,
                                                 float* x_norm,
                                                 float* y_0_1,
                                                 int max_n,
                                                 int channel)
{
	if (!x_norm || !y_0_1 || max_n <= 0) {
		return 0;
	}
	if (!_impl || !_impl->session || !_impl->midi_region) {
		return 0;
	}
	try {
		std::shared_ptr<AutomationList> al = midi_pressure_alist (_impl->midi_region, channel);
		if (!al || al->empty ()) {
			return 0;
		}
		const double beats_per_sec = 120.0 / 60.0;
		float span = std::max (0.001f, view_span_sec);
		float view_end = view_start_sec + span;
		int written = 0;
		for (AutomationList::const_iterator it = al->begin ();
		     it != al->end () && written < max_n; ++it) {
			if (!*it) {
				continue;
			}
			double sec = Temporal::DoubleableBeats ((*it)->when.beats ()).to_double ()
			             / beats_per_sec;
			if (sec < (double)view_start_sec || sec > (double)view_end) {
				continue;
			}
			x_norm[written] = (float)((sec - (double)view_start_sec) / (double)span);
			y_0_1[written] = std::max (0.f, std::min (1.f, (float)((*it)->value / 127.0)));
			++written;
		}
		return written;
	} catch (...) {
		return 0;
	}
}

bool
EngineHost::smoke_midi_pressure ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-midi-pressure failed — no Session";
		return false;
	}
	try {
		if (!ensure_midi_track () || !ensure_midi_region (16.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-pressure FAILED (region)\n");
			std::fflush (stderr);
			return false;
		}
		if (!clear_midi_pressure_automation ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-pressure FAILED (clear)\n");
			std::fflush (stderr);
			return false;
		}
		if (!write_midi_pressure_automation (0.0f, 0.20f)
		    || !write_midi_pressure_automation (2.0f, 0.90f)
		    || !write_midi_pressure_automation (4.0f, 0.40f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-pressure FAILED (write)\n");
			std::fflush (stderr);
			return false;
		}
		std::shared_ptr<AutomationList> al = midi_pressure_alist (_impl->midi_region, 0);
		if (!al || al->size () < 3) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-pressure FAILED (size=%u)\n",
			              al ? (unsigned)al->size () : 0u);
			std::fflush (stderr);
			return false;
		}
		const double beats_per_sec = 120.0 / 60.0;
		Temporal::Beats mid_b = Temporal::Beats::from_double (2.0 * beats_per_sec);
		float mid = (float)al->eval (timepos_t (mid_b));
		if (mid < 110.f || mid > 119.f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-pressure FAILED (eval@2s=%.3f)\n",
			              mid);
			std::fflush (stderr);
			return false;
		}
		if (al->automation_state () != Play) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-pressure FAILED (state!=Play)\n");
			std::fflush (stderr);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-pressure ok (pts=%u eval@2s=%.1f Play)\n",
		              (unsigned)al->size (), mid);
		std::fflush (stderr);
		_snap.detail = L"smoke-midi-pressure ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-pressure exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_mute_solo ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-mute-solo failed — no Session";
		return false;
	}
	try {
		ensure_midi_track ();
		collect_tracks ();
		if (_impl->tracks.empty () || !_impl->tracks[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-mute-solo FAILED (no Audio 1)\n");
			std::fflush (stderr);
			return false;
		}
		if (!_impl->midi_track) {
			std::fprintf (stderr, "airdour_win_shell: smoke-mute-solo FAILED (no MIDI)\n");
			std::fflush (stderr);
			return false;
		}

		/* Clear first (RT — wait for settle). */
		set_track_muted (0, false);
		set_track_soloed (0, false);
		set_midi_muted (false);
		set_midi_soloed (false);

		if (!set_track_muted (0, true)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-mute-solo FAILED (mute Audio 1)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_track_muted (0, false)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-mute-solo FAILED (unmute Audio 1)\n");
			std::fflush (stderr);
			return false;
		}

		if (!set_track_soloed (0, true)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-mute-solo FAILED (solo Audio 1)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_track_soloed (0, false)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-mute-solo FAILED (unsolo Audio 1)\n");
			std::fflush (stderr);
			return false;
		}

		if (!set_midi_muted (true)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-mute-solo FAILED (mute MIDI)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_midi_soloed (true)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-mute-solo FAILED (solo MIDI)\n");
			std::fflush (stderr);
			return false;
		}
		set_midi_muted (false);
		set_midi_soloed (false);

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-mute-solo ok "
		              "(Audio1 mute/solo + MIDI mute/solo)\n");
		std::fflush (stderr);
		_snap.detail = L"smoke-mute-solo ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-mute-solo exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_session_record (bool enable)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	try {
		if (enable) {
			bool any_armed = false;
			for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
				if (_snap.track_armed[i]) {
					any_armed = true;
					break;
				}
			}
			if (!any_armed) {
				set_track_armed (0, true);
			}
			_impl->session->maybe_enable_record (false);
			_snap.detail = L"session record armed — Play to capture";
		} else {
			_impl->session->disable_record (false, true);
			refresh_region_flags ();
			_snap.detail = L"session record off";
		}
		refresh_record_flags ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_session_record ()
{
	return set_session_record (!_snap.session_record_armed);
}

bool
EngineHost::import_audio_file (int track, const std::string& path)
{
	if (!_impl || !_impl->session || path.empty ()) {
		return false;
	}
	if (track < 0 || track >= EngineSnapshot::kMeters
	    || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}

	try {
		std::shared_ptr<Source> src = SourceFactory::createExternal (
			DataType::AUDIO,
			*_impl->session,
			path,
			0,
			Source::Flag (Source::Removable | Source::RemovableIfEmpty),
			true,
			false);
		std::shared_ptr<AudioSource> as = std::dynamic_pointer_cast<AudioSource> (src);
		if (!as) {
			_snap.detail = L"import failed — not an audio source";
			return false;
		}

		samplecnt_t n = as->length ().samples ();
		if (n <= 0) {
			_snap.detail = L"import failed — empty file";
			return false;
		}

		PropertyList plist;
		plist.add (Properties::start, timepos_t (0));
		plist.add (Properties::length, timecnt_t ((samplepos_t)n));
		std::shared_ptr<Region> region = RegionFactory::create (src, plist);
		std::shared_ptr<Playlist> pl = _impl->tracks[track]->playlist ();
		if (!region || !pl) {
			_snap.detail = L"import failed — region create";
			return false;
		}
		/* Wrap in Session history so Ctrl+Z / --smoke-undo can reverse import. */
		_impl->session->begin_reversible_command ("import audio");
		pl->clear_changes ();
		pl->clear_owned_changes ();
		pl->add_region (region, timepos_t (0));
		pl->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();
		_impl->selected_track = track;
		_impl->selected_region = region;
		refresh_region_flags ();
		refresh_history_flags ();
		refresh_selection_snap ();
		_snap.detail = L"imported audio onto track — Editor peaks from Session";
		std::fprintf (stderr, "airdour_win_shell: imported \"%s\" (%lld samples) onto track %d\n",
		              path.c_str (), (long long)n, track);
		std::fflush (stderr);
		return true;
	} catch (failed_constructor& e) {
		std::fprintf (stderr, "airdour_win_shell: import failed_constructor: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"import failed_constructor";
		return false;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: import exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"import exception";
		return false;
	} catch (...) {
		_snap.detail = L"import unknown failure";
		return false;
	}
}

bool
EngineHost::import_test_tone (int track, float freq_hz, float seconds)
{
	if (!_impl || !_impl->session || !_impl->engine) {
		return false;
	}
	samplecnt_t sr = (samplecnt_t)_impl->engine->sample_rate ();
	if (sr <= 0) {
		sr = 48000;
	}
	if (seconds < 0.25f) {
		seconds = 0.25f;
	}
	const int n = (int)std::llround ((double)sr * (double)seconds);
	std::string wav = path_join (
		_impl->session_dir,
		string_compose ("import_tone_%1.wav", track + 1));
	if (!write_pcm16_wav (wav, (int)sr, n, freq_hz, 0.28f)) {
		_snap.detail = L"test WAV write failed";
		return false;
	}
	std::fprintf (stderr, "airdour_win_shell: wrote test WAV %s\n", wav.c_str ());
	std::fflush (stderr);
	return import_audio_file (track, wav);
}

void
EngineHost::refresh_history_flags ()
{
	_snap.undo_depth = 0;
	_snap.redo_depth = 0;
	_snap.undo_list_count = 0;
	_snap.next_undo_name[0] = L'\0';
	_snap.next_redo_name[0] = L'\0';
	for (int i = 0; i < EngineSnapshot::kMaxHistory; ++i) {
		_snap.undo_list_name[i][0] = L'\0';
	}
	if (!_impl || !_impl->session) {
		return;
	}
	try {
		_snap.undo_depth = _impl->session->undo_depth ();
		_snap.redo_depth = _impl->session->redo_depth ();
		std::string nu = _impl->session->next_undo ();
		std::string nr = _impl->session->next_redo ();
		if (!nu.empty ()) {
			copy_name_to_snap (_snap.next_undo_name, EngineSnapshot::kHistoryNameLen, nu);
		}
		if (!nr.empty ()) {
			copy_name_to_snap (_snap.next_redo_name, EngineSnapshot::kHistoryNameLen, nr);
		}

		/* Recent undo names via UndoHistory::get_state (oldest-first among last N). */
		int want = (int)std::min ((uint32_t)EngineSnapshot::kMaxHistory, _snap.undo_depth);
		if (want > 0) {
			XMLNode& hist = _impl->session->history ().get_state (want);
			std::vector<std::string> names;
			names.reserve ((size_t)want);
			const XMLNodeList& kids = hist.children ();
			for (XMLNodeList::const_iterator it = kids.begin (); it != kids.end (); ++it) {
				if (!*it) {
					continue;
				}
				std::string nm;
				if ((*it)->get_property ("name", nm) && !nm.empty ()) {
					names.push_back (nm);
				}
			}
			delete &hist;
			/* Newest first for the UI list. */
			int n = (int)names.size ();
			if (n > EngineSnapshot::kMaxHistory) {
				n = EngineSnapshot::kMaxHistory;
			}
			for (int i = 0; i < n; ++i) {
				copy_name_to_snap (_snap.undo_list_name[i], EngineSnapshot::kHistoryNameLen,
				                   names[(size_t)(n - 1 - i)]);
			}
			_snap.undo_list_count = n;
		}
	} catch (...) {
	}
}

void
EngineHost::refresh_selection_snap ()
{
	_snap.has_selection = false;
	_snap.selection_track = -1;
	_snap.selection_start_sec = 0.f;
	_snap.selection_end_sec = 0.f;
	_snap.selection_fade_in_sec = 0.f;
	_snap.selection_fade_out_sec = 0.f;
	if (!_impl || !_impl->session) {
		return;
	}
	std::shared_ptr<Region> region = _impl->selected_region.lock ();
	if (!region || _impl->selected_track < 0) {
		_impl->selected_region.reset ();
		_impl->selected_track = -1;
		return;
	}
	/* Drop selection if region left its playlist (undo/delete). */
	if (!region->playlist ()) {
		_impl->selected_region.reset ();
		_impl->selected_track = -1;
		return;
	}
	samplecnt_t sr = _impl->session->sample_rate ();
	if (sr <= 0 && _impl->engine) {
		sr = (samplecnt_t)_impl->engine->sample_rate ();
	}
	if (sr <= 0) {
		return;
	}
	samplepos_t start = region->position_sample ();
	samplecnt_t len = region->length_samples ();
	_snap.has_selection = true;
	_snap.selection_track = _impl->selected_track;
	_snap.selection_start_sec = (float)start / (float)sr;
	_snap.selection_end_sec = (float)(start + len) / (float)sr;
	std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (region);
	if (ar) {
		_snap.selection_fade_in_sec = (float)ar->fade_in_length ().samples () / (float)sr;
		_snap.selection_fade_out_sec = (float)ar->fade_out_length ().samples () / (float)sr;
	}
}

bool
EngineHost::undo_to_index (int index)
{
	if (index < 0) {
		return false;
	}
	refresh_history_flags ();
	if ((uint32_t)index >= _snap.undo_depth) {
		_snap.detail = L"undo jump — out of range";
		return false;
	}
	return undo ((uint32_t)index + 1u);
}

bool
EngineHost::undo (uint32_t n)
{
	if (!_impl || !_impl->session || n == 0) {
		return false;
	}
	try {
		if (_impl->session->undo_depth () == 0) {
			_snap.detail = L"nothing to undo";
			refresh_history_flags ();
			return false;
		}
		_impl->session->undo (n);
		refresh_region_flags ();
		refresh_history_flags ();
		refresh_selection_snap ();
		_snap.detail = L"undo";
		std::fprintf (stderr, "airdour_win_shell: undo (%u) — undo_depth=%u redo_depth=%u\n",
		              (unsigned)n, (unsigned)_snap.undo_depth, (unsigned)_snap.redo_depth);
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: undo exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"undo exception";
		return false;
	} catch (...) {
		_snap.detail = L"undo unknown failure";
		return false;
	}
}

bool
EngineHost::redo (uint32_t n)
{
	if (!_impl || !_impl->session || n == 0) {
		return false;
	}
	try {
		if (_impl->session->redo_depth () == 0) {
			_snap.detail = L"nothing to redo";
			refresh_history_flags ();
			return false;
		}
		_impl->session->redo (n);
		refresh_region_flags ();
		refresh_history_flags ();
		refresh_selection_snap ();
		_snap.detail = L"redo";
		std::fprintf (stderr, "airdour_win_shell: redo (%u) — undo_depth=%u redo_depth=%u\n",
		              (unsigned)n, (unsigned)_snap.undo_depth, (unsigned)_snap.redo_depth);
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: redo exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"redo exception";
		return false;
	} catch (...) {
		_snap.detail = L"redo unknown failure";
		return false;
	}
}

bool
EngineHost::select_region_at (int track, float time_sec)
{
	clear_region_selection ();
	if (!_impl || !_impl->session) {
		return false;
	}
	if (track < 0 || track >= EngineSnapshot::kMeters
	    || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	try {
		std::shared_ptr<Playlist> pl = _impl->tracks[track]->playlist ();
		if (!pl) {
			return false;
		}
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0 && _impl->engine) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
		if (sr <= 0) {
			return false;
		}
		if (time_sec < 0.f) {
			time_sec = 0.f;
		}
		samplepos_t sample = (samplepos_t)std::llround ((double)time_sec * (double)sr);
		std::shared_ptr<RegionList> hits = pl->regions_at (timepos_t (sample));
		if (!hits || hits->empty ()) {
			_snap.detail = L"no region at click";
			refresh_selection_snap ();
			return false;
		}
		/* Prefer topmost (last in list / highest layer). */
		std::shared_ptr<Region> best = hits->back ();
		for (RegionList::const_iterator it = hits->begin (); it != hits->end (); ++it) {
			if (*it && best && (*it)->layer () >= best->layer ()) {
				best = *it;
			}
		}
		if (!best) {
			return false;
		}
		_impl->selected_track = track;
		_impl->selected_region = best;
		refresh_selection_snap ();
		_snap.detail = L"region selected — Delete to remove";
		std::fprintf (stderr,
		              "airdour_win_shell: selected region track=%d [%.2f..%.2f]s\n",
		              track, _snap.selection_start_sec, _snap.selection_end_sec);
		std::fflush (stderr);
		return true;
	} catch (...) {
		clear_region_selection ();
		return false;
	}
}

void
EngineHost::clear_region_selection ()
{
	if (_impl) {
		_impl->selected_track = -1;
		_impl->selected_region.reset ();
	}
	_snap.has_selection = false;
	_snap.selection_track = -1;
	_snap.selection_start_sec = 0.f;
	_snap.selection_end_sec = 0.f;
	_snap.selection_fade_in_sec = 0.f;
	_snap.selection_fade_out_sec = 0.f;
}

bool
EngineHost::delete_selected_region ()
{
	if (!_impl || !_impl->session) {
		return false;
	}
	std::shared_ptr<Region> region = _impl->selected_region.lock ();
	int track = _impl->selected_track;
	if (!region || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		_snap.detail = L"no region selected";
		return false;
	}
	try {
		std::shared_ptr<Playlist> pl = region->playlist ();
		if (!pl) {
			pl = _impl->tracks[track]->playlist ();
		}
		if (!pl) {
			_snap.detail = L"delete failed — no playlist";
			return false;
		}
		_impl->session->begin_reversible_command ("remove region");
		pl->clear_changes ();
		pl->clear_owned_changes ();
		pl->remove_region (region);
		pl->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();
		clear_region_selection ();
		refresh_region_flags ();
		refresh_history_flags ();
		_snap.detail = L"region deleted — Ctrl+Z to undo";
		std::fprintf (stderr, "airdour_win_shell: deleted region on track %d\n", track);
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: delete exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"delete exception";
		return false;
	} catch (...) {
		_snap.detail = L"delete unknown failure";
		return false;
	}
}

int
EngineHost::track_region_count (int track) const
{
	if (!_impl || !_impl->session || track < 0 || track >= (int)_impl->tracks.size ()
	    || !_impl->tracks[track]) {
		return 0;
	}
	try {
		std::shared_ptr<Playlist> pl = _impl->tracks[track]->playlist ();
		if (!pl) {
			return 0;
		}
		std::shared_ptr<RegionList> rl = pl->region_list ();
		return rl ? (int)rl->size () : 0;
	} catch (...) {
		return 0;
	}
}

bool
EngineHost::split_at (float time_sec, int track)
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"split failed — no Session";
		return false;
	}
	if (track < 0) {
		track = _impl->selected_track >= 0 ? _impl->selected_track : 0;
	}
	if (track < 0 || track >= EngineSnapshot::kMeters
	    || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		_snap.detail = L"split failed — no track";
		return false;
	}
	try {
		std::shared_ptr<Playlist> pl = _impl->tracks[track]->playlist ();
		if (!pl) {
			_snap.detail = L"split failed — no playlist";
			return false;
		}
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0 && _impl->engine) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
		if (sr <= 0) {
			return false;
		}
		time_sec = snap_time (time_sec);
		if (time_sec < 0.f) {
			time_sec = 0.f;
		}
		int before = track_region_count (track);
		samplepos_t sample = (samplepos_t)std::llround ((double)time_sec * (double)sr);
		_impl->session->begin_reversible_command ("split");
		pl->clear_changes ();
		pl->clear_owned_changes ();
		pl->split (timepos_t (sample));
		pl->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();
		refresh_region_flags ();
		refresh_history_flags ();
		refresh_selection_snap ();
		int after = track_region_count (track);
		_snap.detail = L"split at playhead — peaks refresh";
		std::fprintf (stderr,
		              "airdour_win_shell: split track=%d @%.2fs regions %d→%d\n",
		              track, time_sec, before, after);
		std::fflush (stderr);
		return after >= before;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: split exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"split exception";
		return false;
	} catch (...) {
		_snap.detail = L"split unknown failure";
		return false;
	}
}

bool
EngineHost::split_at_playhead (int track)
{
	float t = _snap.transport_sec;
	if (t < 0.f) {
		t = 0.f;
	}
	return split_at (t, track);
}

bool
EngineHost::set_selected_fade_in_sec (float sec)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	std::shared_ptr<Region> region = _impl->selected_region.lock ();
	std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (region);
	if (!ar) {
		_snap.detail = L"fade-in — select an audio region";
		return false;
	}
	try {
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0) {
			sr = 48000;
		}
		sec = std::max (0.f, sec);
		samplecnt_t samples = (samplecnt_t)std::llround ((double)sec * (double)sr);
		samplecnt_t max_len = ar->length_samples () / 2;
		if (samples > max_len) {
			samples = max_len;
		}
		_impl->session->begin_reversible_command ("fade in");
		ar->clear_changes ();
		ar->set_fade_in_active (true);
		ar->set_fade_in (FadeLinear, samples);
		_impl->session->add_command (new StatefulDiffCommand (ar));
		_impl->session->commit_reversible_command ();
		refresh_selection_snap ();
		refresh_history_flags ();
		_snap.detail = L"fade-in set";
		return true;
	} catch (...) {
		_snap.detail = L"fade-in failed";
		return false;
	}
}

bool
EngineHost::set_selected_fade_out_sec (float sec)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	std::shared_ptr<Region> region = _impl->selected_region.lock ();
	std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (region);
	if (!ar) {
		_snap.detail = L"fade-out — select an audio region";
		return false;
	}
	try {
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0) {
			sr = 48000;
		}
		sec = std::max (0.f, sec);
		samplecnt_t samples = (samplecnt_t)std::llround ((double)sec * (double)sr);
		samplecnt_t max_len = ar->length_samples () / 2;
		if (samples > max_len) {
			samples = max_len;
		}
		_impl->session->begin_reversible_command ("fade out");
		ar->clear_changes ();
		ar->set_fade_out_active (true);
		ar->set_fade_out (FadeLinear, samples);
		_impl->session->add_command (new StatefulDiffCommand (ar));
		_impl->session->commit_reversible_command ();
		refresh_selection_snap ();
		refresh_history_flags ();
		_snap.detail = L"fade-out set";
		return true;
	} catch (...) {
		_snap.detail = L"fade-out failed";
		return false;
	}
}

bool
EngineHost::apply_selected_crossfade (float length_sec)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	std::shared_ptr<Region> region = _impl->selected_region.lock ();
	std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (region);
	int track = _impl->selected_track;
	if (!ar || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		_snap.detail = L"crossfade — select an audio region";
		return false;
	}
	try {
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0) {
			sr = 48000;
		}
		length_sec = std::max (0.01f, std::min (0.5f, length_sec));
		samplecnt_t fade_samp = (samplecnt_t)std::llround ((double)length_sec * (double)sr);
		samplecnt_t half = ar->length_samples () / 2;
		if (fade_samp > half) {
			fade_samp = std::max ((samplecnt_t)1, half);
		}

		std::shared_ptr<Playlist> pl = ar->playlist ();
		if (!pl) {
			pl = _impl->tracks[track]->playlist ();
		}
		std::shared_ptr<AudioRegion> neighbor;
		if (pl) {
			/* Prefer region that starts at/after our end (next clip). */
			std::shared_ptr<Region> next = pl->find_next_region (ar->end_position (), Start, 1);
			if (!next) {
				next = pl->find_next_region (ar->position (), Start, -1);
			}
			if (next && next != region) {
				neighbor = std::dynamic_pointer_cast<AudioRegion> (next);
			}
		}

		_impl->session->begin_reversible_command ("crossfade / fades");
		ar->clear_changes ();
		ar->set_fade_in_active (true);
		ar->set_fade_out_active (true);
		ar->set_fade_in (FadeLinear, fade_samp);
		ar->set_fade_out (FadeLinear, fade_samp);
		_impl->session->add_command (new StatefulDiffCommand (ar));

		if (neighbor) {
			samplecnt_t nhalf = neighbor->length_samples () / 2;
			samplecnt_t nf = fade_samp;
			if (nf > nhalf) {
				nf = std::max ((samplecnt_t)1, nhalf);
			}
			neighbor->clear_changes ();
			/* If neighbor is after us, give it fade-in; if before, fade-out. */
			if (neighbor->position_sample () >= ar->last_sample ()) {
				neighbor->set_fade_in_active (true);
				neighbor->set_fade_in (FadeLinear, nf);
			} else {
				neighbor->set_fade_out_active (true);
				neighbor->set_fade_out (FadeLinear, nf);
			}
			_impl->session->add_command (new StatefulDiffCommand (neighbor));
		}
		_impl->session->commit_reversible_command ();
		refresh_selection_snap ();
		refresh_history_flags ();
		_snap.detail = neighbor ? L"crossfade fades applied" : L"region fades applied";
		std::fprintf (stderr,
		              "airdour_win_shell: fades %.0fms on selection%s\n",
		              length_sec * 1000.f, neighbor ? " + neighbor" : "");
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: crossfade exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"crossfade exception";
		return false;
	} catch (...) {
		_snap.detail = L"crossfade failed";
		return false;
	}
}

bool
EngineHost::trim_selected_front_to (float time_sec)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	std::shared_ptr<Region> region = _impl->selected_region.lock ();
	if (!region) {
		_snap.detail = L"trim front — select a region";
		return false;
	}
	try {
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0) {
			sr = 48000;
		}
		time_sec = snap_time (time_sec);
		if (time_sec < 0.f) {
			time_sec = 0.f;
		}
		samplepos_t sample = (samplepos_t)std::llround ((double)time_sec * (double)sr);
		samplepos_t pos = region->position_sample ();
		samplecnt_t len = region->length_samples ();
		samplepos_t end_excl = pos + len;
		samplecnt_t min_len = std::max ((samplecnt_t)1, (samplecnt_t)(sr / 50)); /* ~20 ms */
		if (sample + min_len >= end_excl) {
			sample = end_excl > min_len ? end_excl - min_len : 0;
		}
		if (sample == pos) {
			return true;
		}
		std::shared_ptr<Playlist> pl = region->playlist ();
		_impl->session->begin_reversible_command ("trim front");
		if (pl) {
			pl->clear_changes ();
			pl->clear_owned_changes ();
			pl->freeze ();
		}
		region->clear_changes ();
		region->trim_front (timepos_t (sample));
		if (pl) {
			std::vector<Command*> cmds;
			pl->rdiff (cmds);
			_impl->session->add_commands (cmds);
			pl->thaw ();
		} else {
			_impl->session->add_command (new StatefulDiffCommand (region));
		}
		_impl->session->commit_reversible_command ();
		refresh_region_flags ();
		refresh_selection_snap ();
		refresh_history_flags ();
		_snap.detail = L"trim front — peaks refresh";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: trim front exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"trim front exception";
		return false;
	} catch (...) {
		_snap.detail = L"trim front failed";
		return false;
	}
}

bool
EngineHost::trim_selected_end_to (float time_sec)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	std::shared_ptr<Region> region = _impl->selected_region.lock ();
	if (!region) {
		_snap.detail = L"trim end — select a region";
		return false;
	}
	try {
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0) {
			sr = 48000;
		}
		time_sec = snap_time (time_sec);
		if (time_sec < 0.f) {
			time_sec = 0.f;
		}
		/* selection_end_sec is exclusive end (position + length). */
		samplepos_t end_excl = (samplepos_t)std::llround ((double)time_sec * (double)sr);
		samplepos_t pos = region->position_sample ();
		samplecnt_t min_len = std::max ((samplecnt_t)1, (samplecnt_t)(sr / 50));
		if (end_excl <= pos + min_len) {
			end_excl = pos + min_len;
		}
		samplecnt_t new_len = end_excl - pos;
		if (new_len == region->length_samples ()) {
			return true;
		}
		std::shared_ptr<Playlist> pl = region->playlist ();
		_impl->session->begin_reversible_command ("trim end");
		if (pl) {
			pl->clear_changes ();
			pl->clear_owned_changes ();
			pl->freeze ();
		}
		region->clear_changes ();
		region->trim_to (timepos_t (pos), timecnt_t (new_len, timepos_t (pos)));
		if (pl) {
			std::vector<Command*> cmds;
			pl->rdiff (cmds);
			_impl->session->add_commands (cmds);
			pl->thaw ();
		} else {
			_impl->session->add_command (new StatefulDiffCommand (region));
		}
		_impl->session->commit_reversible_command ();
		refresh_region_flags ();
		refresh_selection_snap ();
		refresh_history_flags ();
		_snap.detail = L"trim end — peaks refresh";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: trim end exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"trim end exception";
		return false;
	} catch (...) {
		_snap.detail = L"trim end failed";
		return false;
	}
}

bool
EngineHost::move_selected_to (float time_sec)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	std::shared_ptr<Region> region = _impl->selected_region.lock ();
	if (!region) {
		_snap.detail = L"move — select a region";
		return false;
	}
	try {
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0) {
			sr = 48000;
		}
		time_sec = snap_time (time_sec);
		if (time_sec < 0.f) {
			time_sec = 0.f;
		}
		samplepos_t sample = (samplepos_t)std::llround ((double)time_sec * (double)sr);
		if (sample == region->position_sample ()) {
			return true;
		}
		std::shared_ptr<Playlist> pl = region->playlist ();
		_impl->session->begin_reversible_command ("move region");
		if (pl) {
			pl->clear_changes ();
			pl->clear_owned_changes ();
			pl->freeze ();
		}
		region->clear_changes ();
		region->set_position (timepos_t (sample));
		if (pl) {
			std::vector<Command*> cmds;
			pl->rdiff (cmds);
			_impl->session->add_commands (cmds);
			pl->thaw ();
		} else {
			_impl->session->add_command (new StatefulDiffCommand (region));
		}
		_impl->session->commit_reversible_command ();
		refresh_region_flags ();
		refresh_selection_snap ();
		refresh_history_flags ();
		_snap.detail = L"move region — peaks refresh";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: move region exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"move exception";
		return false;
	} catch (...) {
		_snap.detail = L"move failed";
		return false;
	}
}

namespace {

struct ShellProgress : public Progress {
	void set_overall_progress (float) override {}
};

Temporal::ratio_t
stretch_ratio_from_float (float r)
{
	if (!(r > 0.f) || !std::isfinite (r)) {
		r = 1.f;
	}
	if (std::fabs (r - 1.5f) < 0.01f) {
		return Temporal::ratio_t (3, 2);
	}
	if (std::fabs (r - 0.75f) < 0.01f) {
		return Temporal::ratio_t (3, 4);
	}
	if (std::fabs (r - 1.f) < 0.001f) {
		return Temporal::ratio_t (1, 1);
	}
	int64_t num = (int64_t)std::llround ((double)r * 1000.0);
	if (num < 1) {
		num = 1;
	}
	return Temporal::ratio_t (num, 1000);
}

/** Interim: change region length without resampling (pitch unchanged; content truncates/extends). */
bool
scale_selected_region_length (Session& session,
                              std::shared_ptr<Region> region,
                              float time_ratio,
                              std::wstring& detail)
{
	if (!region || !(time_ratio > 0.f) || !std::isfinite (time_ratio)) {
		detail = L"scale — bad ratio";
		return false;
	}
	if (std::fabs (time_ratio - 1.f) < 0.001f) {
		detail = L"scale — no-op";
		return true;
	}
	samplecnt_t old_len = region->length_samples ();
	samplecnt_t new_len = (samplecnt_t)std::llround ((double)old_len * (double)time_ratio);
	if (new_len < 1) {
		new_len = 1;
	}
	std::shared_ptr<Playlist> pl = region->playlist ();
	session.begin_reversible_command ("scale region");
	if (pl) {
		pl->clear_changes ();
		pl->clear_owned_changes ();
		pl->freeze ();
	}
	region->clear_changes ();
	region->set_length (timecnt_t (new_len, region->position ()));
	if (pl) {
		std::vector<Command*> cmds;
		pl->rdiff (cmds);
		session.add_commands (cmds);
		pl->thaw ();
	} else {
		session.add_command (new StatefulDiffCommand (region));
	}
	session.commit_reversible_command ();
	detail = L"scale region (interim, no pitch) — peaks refresh";
	return true;
}

} /* namespace */

bool
EngineHost::stretch_selected (float time_ratio)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	std::shared_ptr<Region> region = _impl->selected_region.lock ();
	std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (region);
	if (!ar) {
		_snap.detail = L"stretch — select an audio region";
		return false;
	}
	if (!(time_ratio > 0.f) || !std::isfinite (time_ratio)) {
		_snap.detail = L"stretch — bad ratio";
		return false;
	}
	if (std::fabs (time_ratio - 1.f) < 0.001f) {
		_snap.detail = L"stretch — no-op";
		return true;
	}
	try {
		std::shared_ptr<Playlist> pl = ar->playlist ();
		if (!pl) {
			_snap.detail = L"stretch — no playlist";
			return false;
		}

		samplecnt_t old_len = ar->length_samples ();
		Temporal::ratio_t frac = stretch_ratio_from_float (time_ratio);

#ifdef HAVE_RUBBERBAND
		TimeFXRequest req;
		req.algorithm = TimeFXRequest::Rubberband;
		req.time_fraction = frac;
		req.pitch_fraction = 1.0f;
		req.opts = 0;
		req.cancel = false;
		req.done = false;

		ShellProgress progress;
		RBStretch fx (*_impl->session, req);
		if (fx.run (ar, &progress) != 0 || fx.results.empty ()) {
			std::fprintf (stderr,
			              "airdour_win_shell: RBStretch failed — falling back to scale/slip\n");
			std::fflush (stderr);
			std::wstring d;
			if (!scale_selected_region_length (*_impl->session, ar, time_ratio, d)) {
				_snap.detail = L"stretch failed";
				return false;
			}
			refresh_region_flags ();
			refresh_selection_snap ();
			refresh_history_flags ();
			_snap.detail = d;
			return true;
		}

		std::shared_ptr<Region> neu = fx.results.front ();
		/* replace_region clones whole_file results — keep position for re-select. */
		timepos_t pos = ar->position ();
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0) {
			sr = 48000;
		}
		float pos_sec = (float)ar->position_sample () / (float)sr;
		int track = _impl->selected_track;

		_impl->session->begin_reversible_command ("time stretch");
		pl->clear_changes ();
		pl->clear_owned_changes ();
		pl->replace_region (ar, neu, pos);
		pl->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();

		/* Select the playlist-owned clone (neu may be whole_file / not on PL). */
		if (track < 0 || !select_region_at (track, pos_sec + 0.01f)) {
			_impl->selected_region = neu;
			refresh_selection_snap ();
		}
		refresh_region_flags ();
		refresh_history_flags ();

		samplecnt_t new_len = 0;
		if (std::shared_ptr<Region> sel = _impl->selected_region.lock ()) {
			new_len = sel->length_samples ();
		} else {
			new_len = neu->length_samples ();
		}
		std::fprintf (stderr,
		              "airdour_win_shell: stretch ok (RB %.3fx samples %lld→%lld)\n",
		              time_ratio, (long long)old_len, (long long)new_len);
		std::fflush (stderr);
		_snap.detail = L"time stretch (RubberBand) — peaks refresh";
		return true;
#else
		(void)frac;
		std::wstring d;
		if (!scale_selected_region_length (*_impl->session, ar, time_ratio, d)) {
			_snap.detail = L"stretch failed";
			return false;
		}
		refresh_region_flags ();
		refresh_selection_snap ();
		refresh_history_flags ();
		_snap.detail = d;
		std::fprintf (stderr,
		              "airdour_win_shell: stretch via scale/slip interim (no HAVE_RUBBERBAND)\n");
		std::fflush (stderr);
		return true;
#endif
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: stretch exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"stretch exception";
		return false;
	} catch (...) {
		_snap.detail = L"stretch failed";
		return false;
	}
}

bool
EngineHost::pitch_shift_selected (float semitones)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	std::shared_ptr<Region> region = _impl->selected_region.lock ();
	std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (region);
	if (!ar) {
		_snap.detail = L"pitch — select an audio region";
		return false;
	}
	if (!std::isfinite (semitones) || std::fabs (semitones) < 0.001f) {
		_snap.detail = L"pitch — no-op";
		return true;
	}
	/* Clamp to a musically sane range (±2 octaves). */
	if (semitones > 24.f) {
		semitones = 24.f;
	}
	if (semitones < -24.f) {
		semitones = -24.f;
	}
	try {
		std::shared_ptr<Playlist> pl = ar->playlist ();
		if (!pl) {
			_snap.detail = L"pitch — no playlist";
			return false;
		}

		samplecnt_t old_len = ar->length_samples ();
		float pitch_frac = (float)std::pow (2.0, (double)semitones / 12.0);

#ifdef HAVE_RUBBERBAND
		TimeFXRequest req;
		req.algorithm = TimeFXRequest::Rubberband;
		req.time_fraction = Temporal::ratio_t (1, 1); /* duration held */
		req.pitch_fraction = pitch_frac;
		req.opts = 0;
		req.cancel = false;
		req.done = false;

		ShellProgress progress;
		ARDOUR::Pitch fx (*_impl->session, req);
		if (fx.run (ar, &progress) != 0 || fx.results.empty ()) {
			std::fprintf (stderr, "airdour_win_shell: Pitch (RB) failed\n");
			std::fflush (stderr);
			_snap.detail = L"pitch failed";
			return false;
		}

		std::shared_ptr<Region> neu = fx.results.front ();
		timepos_t pos = ar->position ();
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0) {
			sr = 48000;
		}
		float pos_sec = (float)ar->position_sample () / (float)sr;
		int track = _impl->selected_track;

		_impl->session->begin_reversible_command ("pitch shift");
		pl->clear_changes ();
		pl->clear_owned_changes ();
		pl->replace_region (ar, neu, pos);
		pl->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();

		if (track < 0 || !select_region_at (track, pos_sec + 0.01f)) {
			_impl->selected_region = neu;
			refresh_selection_snap ();
		}
		refresh_region_flags ();
		refresh_history_flags ();

		samplecnt_t new_len = 0;
		if (std::shared_ptr<Region> sel = _impl->selected_region.lock ()) {
			new_len = sel->length_samples ();
		} else {
			new_len = neu->length_samples ();
		}
		std::fprintf (stderr,
		              "airdour_win_shell: pitch ok (%+.1fst ratio=%.4f samples %lld→%lld)\n",
		              semitones, pitch_frac, (long long)old_len, (long long)new_len);
		std::fflush (stderr);
		_snap.detail = L"pitch shift (RubberBand) — duration held";
		return true;
#else
		(void)pitch_frac;
		(void)old_len;
		_snap.detail = L"pitch unavailable (no RubberBand)";
		std::fprintf (stderr, "airdour_win_shell: pitch failed — no HAVE_RUBBERBAND\n");
		std::fflush (stderr);
		return false;
#endif
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: pitch exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"pitch exception";
		return false;
	} catch (...) {
		_snap.detail = L"pitch failed";
		return false;
	}
}

bool
EngineHost::reverse_selected ()
{
	if (!_impl || !_impl->session) {
		return false;
	}
	std::shared_ptr<Region> region = _impl->selected_region.lock ();
	std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (region);
	if (!ar) {
		_snap.detail = L"reverse — select an audio region";
		return false;
	}
	try {
		std::shared_ptr<Playlist> pl = ar->playlist ();
		if (!pl) {
			_snap.detail = L"reverse — no playlist";
			return false;
		}

		samplecnt_t old_len = ar->length_samples ();
		timepos_t pos = ar->position ();
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0) {
			sr = 48000;
		}
		float pos_sec = (float)ar->position_sample () / (float)sr;
		int track = _impl->selected_track;

		Reverse rev (*_impl->session);
		if (ar->apply (rev, nullptr) != 0 || rev.results.empty ()) {
			std::fprintf (stderr, "airdour_win_shell: Reverse filter failed\n");
			std::fflush (stderr);
			_snap.detail = L"reverse failed";
			return false;
		}

		std::shared_ptr<Region> neu = rev.results.front ();
		_impl->session->begin_reversible_command ("reverse region");
		pl->clear_changes ();
		pl->clear_owned_changes ();
		pl->replace_region (ar, neu, pos);
		pl->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();

		if (track < 0 || !select_region_at (track, pos_sec + 0.01f)) {
			_impl->selected_region = neu;
			refresh_selection_snap ();
		}
		refresh_region_flags ();
		refresh_history_flags ();

		samplecnt_t new_len = 0;
		if (std::shared_ptr<Region> sel = _impl->selected_region.lock ()) {
			new_len = sel->length_samples ();
		} else {
			new_len = neu->length_samples ();
		}
		std::fprintf (stderr,
		              "airdour_win_shell: reverse ok (samples %lld→%lld)\n",
		              (long long)old_len, (long long)new_len);
		std::fflush (stderr);
		_snap.detail = L"reverse region — peaks refresh";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: reverse exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"reverse exception";
		return false;
	} catch (...) {
		_snap.detail = L"reverse failed";
		return false;
	}
}

bool
EngineHost::normalize_selected (float target_dbfs)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	std::shared_ptr<Region> region = _impl->selected_region.lock ();
	std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (region);
	if (!ar) {
		_snap.detail = L"normalize — select an audio region";
		return false;
	}
	if (!std::isfinite (target_dbfs) || target_dbfs > 0.f) {
		target_dbfs = -1.f;
	}
	if (target_dbfs < -112.f) {
		target_dbfs = -112.f;
	}
	try {
		double max_amp = ar->maximum_amplitude (nullptr);
		if (max_amp < 0.0) {
			_snap.detail = L"normalize cancelled";
			return false;
		}
		if (max_amp < (double)GAIN_COEFF_SMALL) {
			_snap.detail = L"normalize — silence / no peak";
			return false;
		}

		float scale_before = ar->scale_amplitude ();
		_impl->session->begin_reversible_command ("normalize");
		ar->clear_changes ();
		ar->normalize ((float)max_amp, target_dbfs);
		_impl->session->add_command (new StatefulDiffCommand (ar));
		_impl->session->commit_reversible_command ();

		refresh_region_flags ();
		refresh_selection_snap ();
		refresh_history_flags ();

		float scale_after = ar->scale_amplitude ();
		std::fprintf (stderr,
		              "airdour_win_shell: normalize ok (peak=%.4f → %.1f dBFS scale %.4f→%.4f)\n",
		              max_amp, target_dbfs, scale_before, scale_after);
		std::fflush (stderr);
		_snap.detail = L"normalize — peaks refresh";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: normalize exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"normalize exception";
		return false;
	} catch (...) {
		_snap.detail = L"normalize failed";
		return false;
	}
}

bool
EngineHost::strip_silence_selected (float threshold_dbfs,
                                    float min_silence_sec,
                                    float fade_sec)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	std::shared_ptr<Region> region = _impl->selected_region.lock ();
	std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (region);
	if (!ar) {
		_snap.detail = L"strip silence — select an audio region";
		return false;
	}
	try {
		std::shared_ptr<Playlist> pl = ar->playlist ();
		if (!pl) {
			_snap.detail = L"strip silence — no playlist";
			return false;
		}

		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0) {
			sr = 48000;
		}
		if (!std::isfinite (threshold_dbfs) || threshold_dbfs > 0.f) {
			threshold_dbfs = -40.f;
		}
		if (threshold_dbfs < -120.f) {
			threshold_dbfs = -120.f;
		}
		if (!(min_silence_sec > 0.f) || !std::isfinite (min_silence_sec)) {
			min_silence_sec = 0.05f;
		}
		if (!(fade_sec >= 0.f) || !std::isfinite (fade_sec)) {
			fade_sec = 0.005f;
		}

		samplecnt_t min_len = (samplecnt_t)std::llround ((double)min_silence_sec * (double)sr);
		if (min_len < 1) {
			min_len = 1;
		}
		samplecnt_t fade_len = (samplecnt_t)std::llround ((double)fade_sec * (double)sr);
		if (fade_len < 0) {
			fade_len = 0;
		}

		Sample thresh = dB_to_coefficient (threshold_dbfs);
		InterThreadInfo itt;
		itt.cancel = false;
		itt.done = false;
		AudioIntervalResult silences = ar->find_silence (thresh, min_len, fade_len, itt);
		if (itt.cancel) {
			_snap.detail = L"strip silence cancelled";
			return false;
		}

		AudioIntervalMap smap;
		smap[ar] = silences;

		int n0 = track_region_count (_impl->selected_track);
		float pos_sec = (float)ar->position_sample () / (float)sr;
		int track = _impl->selected_track;

		StripSilence filter (*_impl->session, smap, fade_len);
		if (ar->apply (filter, nullptr) != 0) {
			_snap.detail = L"strip silence failed";
			return false;
		}

		if (filter.results.empty ()) {
			/* Entire region was silence — leave selection; nothing to replace. */
			_snap.detail = L"strip silence — all quiet (no change)";
			std::fprintf (stderr, "airdour_win_shell: strip silence — all silence, no replace\n");
			std::fflush (stderr);
			return true;
		}

		_impl->session->begin_reversible_command ("strip silence");
		pl->clear_changes ();
		pl->clear_owned_changes ();
		pl->freeze ();
		std::vector<std::shared_ptr<Region> >::iterator res = filter.results.begin ();
		pl->replace_region (ar, *res, (*res)->position ());
		++res;
		while (res != filter.results.end ()) {
			pl->add_region (*res, (*res)->position ());
			++res;
		}
		pl->thaw ();
		std::vector<Command*> cmds;
		pl->rdiff (cmds);
		_impl->session->add_commands (cmds);
		_impl->session->add_command (new StatefulDiffCommand (pl));
		_impl->session->commit_reversible_command ();

		if (track < 0 || !select_region_at (track, pos_sec + 0.01f)) {
			_impl->selected_region = filter.results.front ();
			_impl->selected_track = track;
			refresh_selection_snap ();
		}
		refresh_region_flags ();
		refresh_history_flags ();

		int n1 = track_region_count (track >= 0 ? track : 0);
		std::fprintf (stderr,
		              "airdour_win_shell: strip silence ok (thresh=%.1f dBFS silences=%u "
		              "results=%u regions %d→%d)\n",
		              threshold_dbfs, (unsigned)silences.size (),
		              (unsigned)filter.results.size (), n0, n1);
		std::fflush (stderr);
		_snap.detail = L"strip silence — peaks refresh";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: strip silence exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"strip silence exception";
		return false;
	} catch (...) {
		_snap.detail = L"strip silence failed";
		return false;
	}
}

bool
EngineHost::consolidate_range (int track, float start_sec, float end_sec)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		_snap.detail = L"consolidate — bad track";
		return false;
	}
	if (!(end_sec > start_sec) || !std::isfinite (start_sec) || !std::isfinite (end_sec)) {
		_snap.detail = L"consolidate — bad range";
		return false;
	}
	if (start_sec < 0.f) {
		start_sec = 0.f;
	}
	try {
		std::shared_ptr<AudioTrack> at = _impl->tracks[track];
		std::shared_ptr<Playlist> pl = at->playlist ();
		if (!pl) {
			_snap.detail = L"consolidate — no playlist";
			return false;
		}

		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0) {
			sr = 48000;
		}
		samplepos_t start = (samplepos_t)std::llround ((double)start_sec * (double)sr);
		samplepos_t end = (samplepos_t)std::llround ((double)end_sec * (double)sr);
		if (end <= start) {
			_snap.detail = L"consolidate — empty range";
			return false;
		}

		int n0 = track_region_count (track);

		InterThreadInfo itt;
		itt.cancel = false;
		itt.done = false;
		/* No processing: tap before processors (nil endpoint) — GTK consolidate. */
		std::shared_ptr<Region> bounced = at->bounce_range (
			start, end, itt, std::shared_ptr<Processor> (), false, "Consolidated", true);
		if (!bounced || itt.cancel) {
			std::fprintf (stderr, "airdour_win_shell: consolidate bounce_range failed\n");
			std::fflush (stderr);
			_snap.detail = L"consolidate bounce failed";
			return false;
		}

		PropertyList plist;
		plist.add (Properties::whole_file, false);
		std::shared_ptr<Region> copy = RegionFactory::create (bounced, plist);
		if (!copy) {
			_snap.detail = L"consolidate — region copy failed";
			return false;
		}

		timepos_t t0 (start);
		timepos_t t1 (end);

		_impl->session->begin_reversible_command ("consolidate");
		pl->clear_changes ();
		pl->clear_owned_changes ();

		std::list<TimelineRange> ranges;
		ranges.push_back (TimelineRange (t0, t1, 0));
		pl->cut (ranges);
		pl->add_region (copy, t0);

		std::vector<Command*> cmds;
		pl->rdiff (cmds);
		_impl->session->add_commands (cmds);
		_impl->session->add_command (new StatefulDiffCommand (pl));
		_impl->session->commit_reversible_command ();

		_impl->selected_track = track;
		_impl->selected_region = copy;
		refresh_region_flags ();
		refresh_selection_snap ();
		refresh_history_flags ();

		int n1 = track_region_count (track);
		std::fprintf (stderr,
		              "airdour_win_shell: consolidate ok (track %d [%.3f..%.3f]s "
		              "regions %d→%d)\n",
		              track, start_sec, end_sec, n0, n1);
		std::fflush (stderr);
		_snap.detail = L"consolidate — peaks refresh";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: consolidate exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"consolidate exception";
		return false;
	} catch (...) {
		_snap.detail = L"consolidate failed";
		return false;
	}
}

bool
EngineHost::consolidate_selected ()
{
	if (!_snap.has_selection || _snap.selection_track < 0) {
		_snap.detail = L"consolidate — select a region";
		return false;
	}
	return consolidate_range (_snap.selection_track,
	                          _snap.selection_start_sec,
	                          _snap.selection_end_sec);
}

bool
EngineHost::ensure_freeze_session_range (int track)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	try {
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0 && _impl->engine) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
		if (sr <= 0) {
			sr = 48000;
		}

		samplepos_t min_s = 0;
		samplepos_t max_e = (samplepos_t)sr; /* at least 1s */
		std::shared_ptr<Playlist> pl = _impl->tracks[track]->playlist ();
		if (pl) {
			std::shared_ptr<RegionList> regions = pl->region_list ();
			if (regions) {
				for (RegionList::const_iterator it = regions->begin ();
				     it != regions->end (); ++it) {
					if (!*it) {
						continue;
					}
					samplepos_t rs = (*it)->position ().samples ();
					samplepos_t re = (*it)->end_position ().samples ();
					if (re > rs && re > max_e) {
						max_e = re;
					}
					if (rs < min_s) {
						min_s = rs;
					}
				}
			}
		}
		/* Pad end so freeze bounce has room past last region. */
		max_e += (samplepos_t)sr;

		samplepos_t cur_s = _impl->session->current_start_sample ();
		samplepos_t cur_e = _impl->session->current_end_sample ();
		if (cur_e > cur_s && cur_s <= min_s && cur_e >= max_e) {
			return true;
		}
		if (cur_s < min_s) {
			min_s = cur_s;
		}
		if (cur_e > max_e) {
			max_e = cur_e;
		}
		if (max_e <= min_s) {
			max_e = min_s + (samplepos_t)sr;
		}
		_impl->session->set_session_extents (timepos_t (min_s), timepos_t (max_e));
		return _impl->session->current_end_sample () > _impl->session->current_start_sample ();
	} catch (...) {
		return false;
	}
}

bool
EngineHost::freeze_track (int track)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		_snap.detail = L"freeze — bad track";
		return false;
	}
	try {
		std::shared_ptr<AudioTrack> at = _impl->tracks[track];
		if (at->freeze_state () == Track::Frozen) {
			refresh_freeze_flags ();
			_snap.detail = L"freeze — already frozen";
			return true;
		}
		if (!at->bounceable (at->main_outs (), true)) {
			_snap.detail = L"freeze — track not bounceable";
			return false;
		}
		if (!ensure_freeze_session_range (track)) {
			_snap.detail = L"freeze — session range";
			return false;
		}

		/* GTK freeze_route: stop transport before bounce. */
		try {
			_impl->session->request_stop (false, false, TRS_UI);
		} catch (...) {
		}
		for (int i = 0; i < 20; ++i) {
			if (_impl->session->transport_stopped ()) {
				break;
			}
			Sleep (5);
		}

		InterThreadInfo itt;
		itt.cancel = false;
		itt.done = false;
		at->freeze_me (itt);
		if (itt.cancel || at->freeze_state () != Track::Frozen) {
			std::fprintf (stderr, "airdour_win_shell: freeze_me failed (state=%d cancel=%d)\n",
			              (int)at->freeze_state (), itt.cancel ? 1 : 0);
			std::fflush (stderr);
			_snap.detail = L"freeze failed";
			return false;
		}

		_impl->selected_track = -1;
		_impl->selected_region.reset ();
		refresh_region_flags ();
		refresh_selection_snap ();
		refresh_track_fx_snap ();
		refresh_history_flags ();

		std::fprintf (stderr, "airdour_win_shell: freeze ok (track %d frozen=%d)\n",
		              track, _snap.track_frozen[track] ? 1 : 0);
		std::fflush (stderr);
		_snap.detail = L"freeze — peaks refresh";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: freeze exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"freeze exception";
		return false;
	} catch (...) {
		_snap.detail = L"freeze failed";
		return false;
	}
}

bool
EngineHost::unfreeze_track (int track)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		_snap.detail = L"unfreeze — bad track";
		return false;
	}
	try {
		std::shared_ptr<AudioTrack> at = _impl->tracks[track];
		if (at->freeze_state () != Track::Frozen) {
			refresh_freeze_flags ();
			_snap.detail = L"unfreeze — not frozen";
			return true;
		}

		try {
			_impl->session->request_stop (false, false, TRS_UI);
		} catch (...) {
		}

		at->unfreeze ();
		if (at->freeze_state () == Track::Frozen) {
			_snap.detail = L"unfreeze failed";
			return false;
		}

		_impl->selected_track = -1;
		_impl->selected_region.reset ();
		refresh_region_flags ();
		refresh_selection_snap ();
		refresh_track_fx_snap ();
		refresh_history_flags ();

		std::fprintf (stderr, "airdour_win_shell: unfreeze ok (track %d)\n", track);
		std::fflush (stderr);
		_snap.detail = L"unfreeze — peaks refresh";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: unfreeze exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"unfreeze exception";
		return false;
	} catch (...) {
		_snap.detail = L"unfreeze failed";
		return false;
	}
}

bool
EngineHost::toggle_freeze_track (int track)
{
	if (track_is_frozen (track)) {
		return unfreeze_track (track);
	}
	return freeze_track (track);
}

bool
EngineHost::track_is_frozen (int track) const
{
	if (track < 0 || track >= EngineSnapshot::kMeters) {
		return false;
	}
	return _snap.track_frozen[track];
}

void
EngineHost::set_snap_enabled (bool on)
{
	_snap.snap_enabled = on;
	_snap.detail = on ? L"snap on" : L"snap off";
	if (!on) {
		_snap.grid_line_count = 0;
	}
}

bool
EngineHost::snap_enabled () const
{
	return _snap.snap_enabled;
}

void
EngineHost::toggle_snap ()
{
	set_snap_enabled (!_snap.snap_enabled);
}

void
EngineHost::set_snap_density (int density_0_to_3)
{
	if (density_0_to_3 < 0) {
		density_0_to_3 = 0;
	}
	if (density_0_to_3 > 3) {
		density_0_to_3 = 3;
	}
	_snap.snap_density = density_0_to_3;
	static const wchar_t* labels[] = { L"snap bar", L"snap beat", L"snap 1/2", L"snap 1/4" };
	_snap.detail = labels[_snap.snap_density];
}

void
EngineHost::cycle_snap_density ()
{
	set_snap_density ((_snap.snap_density + 1) % 4);
}

int
EngineHost::snap_density () const
{
	return _snap.snap_density;
}

namespace {

double
snap_unit_sec_fallback (int density)
{
	/* Fixed 120 BPM / 4/4 — matches win_ardour MIDI beat mapping. */
	const double beat = 60.0 / 120.0;
	switch (density) {
	case 0: return beat * 4.0; /* bar */
	case 2: return beat * 0.5;
	case 3: return beat * 0.25;
	default: return beat;
	}
}

void
snap_grid_params (int density, uint32_t* bar_mod, uint32_t* beat_div)
{
	switch (density) {
	case 0:
		*bar_mod = 1;
		*beat_div = 1;
		break;
	case 2:
		*bar_mod = 0;
		*beat_div = 2;
		break;
	case 3:
		*bar_mod = 0;
		*beat_div = 4;
		break;
	default:
		*bar_mod = 0;
		*beat_div = 1;
		break;
	}
}

} // namespace

float
EngineHost::snap_unit_sec () const
{
	return (float)snap_unit_sec_fallback (_snap.snap_density);
}

float
EngineHost::snap_time (float time_sec) const
{
	if (!_snap.snap_enabled) {
		return time_sec;
	}
	if (time_sec < 0.f) {
		time_sec = 0.f;
	}
	const int density = _snap.snap_density;

	try {
		Temporal::TempoMap::SharedPtr tm = Temporal::TempoMap::fetch ();
		if (tm) {
			samplecnt_t sr = 48000;
			if (_impl && _impl->session) {
				sr = _impl->session->sample_rate ();
				if (sr <= 0 && _impl->engine) {
					sr = (samplecnt_t)_impl->engine->sample_rate ();
				}
			}
			if (sr <= 0) {
				sr = 48000;
			}
			samplepos_t sample = (samplepos_t)std::llround ((double)time_sec * (double)sr);
			if (density == 0) {
				Temporal::BBT_Argument bbt = tm->bbt_at (timepos_t (sample));
				Temporal::BBT_Argument rounded = tm->round_to_bar (bbt);
				samplepos_t snapped = tm->sample_at (rounded);
				if (snapped < 0) {
					snapped = 0;
				}
				return (float)snapped / (float)sr;
			}
			Temporal::Beats q = tm->quarters_at_sample (sample);
			Temporal::Beats unit (1, 0);
			if (density == 2) {
				unit = Temporal::Beats (0, Temporal::Beats::PPQN / 2);
			} else if (density == 3) {
				unit = Temporal::Beats (0, Temporal::Beats::PPQN / 4);
			}
			if (unit.to_ticks () <= 0) {
				unit = Temporal::Beats (1, 0);
			}
			Temporal::Beats snapped_b = q.round_to_multiple (unit);
			samplepos_t snapped = tm->sample_at (snapped_b);
			if (snapped < 0) {
				snapped = 0;
			}
			return (float)snapped / (float)sr;
		}
	} catch (...) {
	}

	double unit = snap_unit_sec_fallback (density);
	if (unit < 1.0e-6) {
		return time_sec;
	}
	double n = std::round ((double)time_sec / unit);
	if (n < 0.0) {
		n = 0.0;
	}
	return (float)(n * unit);
}

void
EngineHost::refresh_grid_lines (float view_start_sec, float view_span_sec)
{
	_snap.grid_line_count = 0;
	if (!_snap.snap_enabled || view_span_sec <= 0.f) {
		return;
	}
	float view_end = view_start_sec + view_span_sec;

	try {
		Temporal::TempoMap::SharedPtr tm = Temporal::TempoMap::fetch ();
		samplecnt_t sr = 48000;
		if (_impl && _impl->session) {
			sr = _impl->session->sample_rate ();
			if (sr <= 0 && _impl->engine) {
				sr = (samplecnt_t)_impl->engine->sample_rate ();
			}
		}
		if (sr <= 0) {
			sr = 48000;
		}
		if (tm) {
			uint32_t bar_mod = 0;
			uint32_t beat_div = 1;
			snap_grid_params (_snap.snap_density, &bar_mod, &beat_div);
			samplepos_t s0 = (samplepos_t)std::llround ((double)std::max (0.f, view_start_sec) * (double)sr);
			samplepos_t s1 = (samplepos_t)std::llround ((double)view_end * (double)sr);
			if (s1 <= s0) {
				s1 = s0 + sr;
			}
			superclock_t sc0 = Temporal::samples_to_superclock (s0, sr);
			superclock_t sc1 = Temporal::samples_to_superclock (s1, sr);
			Temporal::TempoMapPoints pts;
			tm->grid (pts, sc0, sc1, bar_mod, beat_div);
			int n = 0;
			for (size_t i = 0; i < pts.size () && n < EngineSnapshot::kMaxGridLines; ++i) {
				samplepos_t smp = Temporal::superclock_to_samples (pts[i].sclock (), sr);
				float sec = (float)smp / (float)sr;
				if (sec < view_start_sec - 0.001f || sec > view_end + 0.001f) {
					continue;
				}
				_snap.grid_line_sec[n] = sec;
				_snap.grid_line_bar[n] = pts[i].bbt ().is_bar ();
				++n;
			}
			_snap.grid_line_count = n;
			return;
		}
	} catch (...) {
	}

	/* Fixed 120 BPM fallback grid. */
	double unit = snap_unit_sec_fallback (_snap.snap_density);
	double bar = snap_unit_sec_fallback (0);
	if (unit < 1.0e-6) {
		return;
	}
	double t0 = std::floor ((double)std::max (0.f, view_start_sec) / unit) * unit;
	int n = 0;
	for (double t = t0; t <= (double)view_end + unit * 0.5 && n < EngineSnapshot::kMaxGridLines; t += unit) {
		if (t < (double)view_start_sec - 1.0e-4) {
			continue;
		}
		_snap.grid_line_sec[n] = (float)t;
		_snap.grid_line_bar[n] = (std::fabs (std::fmod (t + 1.0e-6, bar)) < unit * 0.25);
		++n;
	}
	_snap.grid_line_count = n;
}

bool
EngineHost::smoke_snap ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-snap failed — no Session";
		return false;
	}
	try {
		set_snap_density (1); /* beat */
		set_snap_enabled (true);
		float s1 = snap_time (1.1f);
		if (std::fabs (s1 - 1.0f) > 0.08f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-snap FAILED (beat 1.1→%.3f)\n", s1);
			std::fflush (stderr);
			return false;
		}
		float s2 = snap_time (0.24f);
		if (std::fabs (s2 - 0.0f) > 0.08f && std::fabs (s2 - 0.5f) > 0.08f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-snap FAILED (beat 0.24→%.3f)\n", s2);
			std::fflush (stderr);
			return false;
		}
		set_snap_density (0); /* bar */
		float s3 = snap_time (1.1f);
		if (std::fabs (s3 - 0.0f) > 0.08f && std::fabs (s3 - 2.0f) > 0.08f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-snap FAILED (bar 1.1→%.3f)\n", s3);
			std::fflush (stderr);
			return false;
		}
		refresh_grid_lines (0.f, 4.f);
		int lines = _snap.grid_line_count;
		if (lines < 2) {
			std::fprintf (stderr, "airdour_win_shell: smoke-snap FAILED (grid lines=%d)\n",
			              lines);
			std::fflush (stderr);
			return false;
		}
		set_snap_enabled (false);
		float s4 = snap_time (1.1f);
		if (std::fabs (s4 - 1.1f) > 0.001f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-snap FAILED (off 1.1→%.3f)\n", s4);
			std::fflush (stderr);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap ok (beat 1.1→%.2f bar→%.2f lines=%d)\n",
		              s1, s3, lines);
		std::fflush (stderr);
		_snap.detail = L"smoke-snap ok";
		/* Leave snap off after smoke so interactive default stays free. */
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-snap exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_edit ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-edit failed — no Session";
		return false;
	}
	try {
		/* Prefs may enable snap — absolute trim/move times must not quantize. */
		const bool snap_was = _snap.snap_enabled;
		set_snap_enabled (false);
		collect_tracks ();
		if (_impl->tracks.empty () || !_impl->tracks[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (no Audio 1)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!import_test_tone (0, 220.f, 3.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (import)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		int n0 = track_region_count (0);
		if (n0 < 1) {
			std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (no regions after import)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!split_at (1.0f, 0)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (split)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		int n1 = track_region_count (0);
		if (n1 < n0 + 1) {
			std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (regions %d→%d)\n",
			              n0, n1);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!select_region_at (0, 0.5f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (select left)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!apply_selected_crossfade (0.08f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (crossfade)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (_snap.selection_fade_out_sec < 0.05f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (fade_out=%.3f)\n",
			              _snap.selection_fade_out_sec);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		/* Left region was [0..1]; trim end to 0.75s then front to 0.15s. */
		if (!trim_selected_end_to (0.75f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (trim end)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		float len1 = _snap.selection_end_sec - _snap.selection_start_sec;
		if (len1 < 0.65f || len1 > 0.85f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (trim end len=%.3f)\n",
			              len1);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!trim_selected_front_to (0.15f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (trim front)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (_snap.selection_start_sec < 0.10f || _snap.selection_start_sec > 0.20f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (trim front start=%.3f)\n",
			              _snap.selection_start_sec);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		float len2 = _snap.selection_end_sec - _snap.selection_start_sec;
		if (!move_selected_to (0.40f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (move)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (_snap.selection_start_sec < 0.35f || _snap.selection_start_sec > 0.45f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (move start=%.3f)\n",
			              _snap.selection_start_sec);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		float len3 = _snap.selection_end_sec - _snap.selection_start_sec;
		if (std::fabs (len3 - len2) > 0.05f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (move changed len %.3f→%.3f)\n",
			              len2, len3);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-edit ok (split %d→%d fade_out=%.0fms "
		              "trim+move start=%.2f len=%.2f)\n",
		              n0, n1, _snap.selection_fade_out_sec * 1000.f,
		              _snap.selection_start_sec, len3);
		std::fflush (stderr);
		_snap.detail = L"smoke-edit ok";
		set_snap_enabled (snap_was);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-edit exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_stretch ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-stretch failed — no Session";
		return false;
	}
	try {
		const bool snap_was = _snap.snap_enabled;
		set_snap_enabled (false);
		collect_tracks ();
		if (_impl->tracks.empty () || !_impl->tracks[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-stretch FAILED (no Audio 1)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		/* Short tone keeps RubberBand offline stretch snappy for CI/smokes. */
		if (!import_test_tone (0, 440.f, 0.8f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-stretch FAILED (import)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!select_region_at (0, 0.2f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-stretch FAILED (select)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		float len0 = _snap.selection_end_sec - _snap.selection_start_sec;
		if (len0 < 0.5f || len0 > 1.2f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-stretch FAILED (len0=%.3f)\n",
			              len0);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!stretch_selected (1.5f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-stretch FAILED (stretch)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		float len1 = _snap.selection_end_sec - _snap.selection_start_sec;
		/* Allow generous tolerance: RB length + scale interim both ~1.5×. */
		if (len1 < len0 * 1.25f || len1 > len0 * 1.85f) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-stretch FAILED (len %.3f→%.3f)\n",
			              len0, len1);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-stretch ok (len %.3f→%.3f ratio≈%.2f)\n",
		              len0, len1, len1 / len0);
		std::fflush (stderr);
		_snap.detail = L"smoke-stretch ok";
		set_snap_enabled (snap_was);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-stretch exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_pitch ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-pitch failed — no Session";
		return false;
	}
	try {
		const bool snap_was = _snap.snap_enabled;
		set_snap_enabled (false);
		collect_tracks ();
		if (_impl->tracks.empty () || !_impl->tracks[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-pitch FAILED (no Audio 1)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!import_test_tone (0, 440.f, 0.8f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-pitch FAILED (import)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!select_region_at (0, 0.2f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-pitch FAILED (select)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		float len0 = _snap.selection_end_sec - _snap.selection_start_sec;
		if (len0 < 0.5f || len0 > 1.2f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-pitch FAILED (len0=%.3f)\n",
			              len0);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!pitch_shift_selected (1.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-pitch FAILED (pitch +1)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		float len1 = _snap.selection_end_sec - _snap.selection_start_sec;
		/* Duration held: allow small RubberBand length jitter. */
		if (std::fabs (len1 - len0) > 0.08f) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-pitch FAILED (len drifted %.3f→%.3f)\n",
			              len0, len1);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-pitch ok (+1st len %.3f→%.3f held)\n",
		              len0, len1);
		std::fflush (stderr);
		_snap.detail = L"smoke-pitch ok";
		set_snap_enabled (snap_was);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pitch exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_reverse ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-reverse failed — no Session";
		return false;
	}
	try {
		const bool snap_was = _snap.snap_enabled;
		set_snap_enabled (false);
		collect_tracks ();
		if (_impl->tracks.empty () || !_impl->tracks[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-reverse FAILED (no Audio 1)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!import_test_tone (0, 330.f, 0.8f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-reverse FAILED (import)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!select_region_at (0, 0.2f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-reverse FAILED (select)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		float len0 = _snap.selection_end_sec - _snap.selection_start_sec;
		float start0 = _snap.selection_start_sec;
		if (len0 < 0.5f || len0 > 1.2f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-reverse FAILED (len0=%.3f)\n",
			              len0);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!reverse_selected ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-reverse FAILED (reverse)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		float len1 = _snap.selection_end_sec - _snap.selection_start_sec;
		float start1 = _snap.selection_start_sec;
		if (std::fabs (len1 - len0) > 0.05f || std::fabs (start1 - start0) > 0.05f) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-reverse FAILED (start/len %.3f/%.3f→%.3f/%.3f)\n",
			              start0, len0, start1, len1);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-reverse ok (start=%.3f len=%.3f held)\n",
		              start1, len1);
		std::fflush (stderr);
		_snap.detail = L"smoke-reverse ok";
		set_snap_enabled (snap_was);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-reverse exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_normalize ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-normalize failed — no Session";
		return false;
	}
	try {
		const bool snap_was = _snap.snap_enabled;
		set_snap_enabled (false);
		collect_tracks ();
		if (_impl->tracks.empty () || !_impl->tracks[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-normalize FAILED (no Audio 1)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!import_test_tone (0, 440.f, 0.8f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-normalize FAILED (import)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!select_region_at (0, 0.2f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-normalize FAILED (select)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		std::shared_ptr<Region> region = _impl->selected_region.lock ();
		std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (region);
		if (!ar) {
			std::fprintf (stderr, "airdour_win_shell: smoke-normalize FAILED (no AudioRegion)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		float scale0 = ar->scale_amplitude ();
		double peak0 = ar->maximum_amplitude (nullptr);
		if (peak0 < (double)GAIN_COEFF_SMALL) {
			std::fprintf (stderr, "airdour_win_shell: smoke-normalize FAILED (peak=%.6f)\n",
			              peak0);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		const float target_db = -1.f;
		if (!normalize_selected (target_db)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-normalize FAILED (normalize)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		ar = std::dynamic_pointer_cast<AudioRegion> (_impl->selected_region.lock ());
		if (!ar) {
			std::fprintf (stderr, "airdour_win_shell: smoke-normalize FAILED (selection lost)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		float scale1 = ar->scale_amplitude ();
		float expect = dB_to_coefficient (target_db) / (float)peak0;
		/* AudioRegion::normalize nudges unity target by FLT_EPSILON; −1 dBFS is fine. */
		if (std::fabs (scale1 - expect) > 0.05f) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-normalize FAILED (scale %.4f→%.4f expect≈%.4f)\n",
			              scale0, scale1, expect);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-normalize ok (−1 dBFS peak=%.3f scale %.3f→%.3f)\n",
		              peak0, scale0, scale1);
		std::fflush (stderr);
		_snap.detail = L"smoke-normalize ok";
		set_snap_enabled (snap_was);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-normalize exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_strip_silence ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-strip-silence failed — no Session";
		return false;
	}
	try {
		const bool snap_was = _snap.snap_enabled;
		set_snap_enabled (false);
		collect_tracks ();
		if (_impl->tracks.empty () || !_impl->tracks[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-strip-silence FAILED (no Audio 1)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}

		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0 && _impl->engine) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
		if (sr <= 0) {
			sr = 48000;
		}
		std::string wav = path_join (
			_impl->session_dir,
			string_compose ("import_gated_%1.wav", 1));
		/* silence | tone | silence | tone — strip should yield 2 audible regions. */
		if (!write_pcm16_wav_gated (wav, (int)sr, 440.f, 0.28f, 0.15f, 0.25f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-strip-silence FAILED (write gated wav)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!import_audio_file (0, wav)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-strip-silence FAILED (import)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!select_region_at (0, 0.2f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-strip-silence FAILED (select)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		int n0 = track_region_count (0);
		if (n0 < 1) {
			std::fprintf (stderr, "airdour_win_shell: smoke-strip-silence FAILED (n0=%d)\n", n0);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!strip_silence_selected (-40.f, 0.05f, 0.005f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-strip-silence FAILED (strip)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		int n1 = track_region_count (0);
		if (n1 < n0 + 1) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-strip-silence FAILED (regions %d→%d, want ≥%d)\n",
			              n0, n1, n0 + 1);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-strip-silence ok (regions %d→%d)\n",
		              n0, n1);
		std::fflush (stderr);
		_snap.detail = L"smoke-strip-silence ok";
		set_snap_enabled (snap_was);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-strip-silence exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_consolidate ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-consolidate failed — no Session";
		return false;
	}
	try {
		const bool snap_was = _snap.snap_enabled;
		set_snap_enabled (false);
		collect_tracks ();
		if (_impl->tracks.empty () || !_impl->tracks[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-consolidate FAILED (no Audio 1)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!import_test_tone (0, 440.f, 1.0f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-consolidate FAILED (import)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		int n0 = track_region_count (0);
		if (n0 < 1) {
			std::fprintf (stderr, "airdour_win_shell: smoke-consolidate FAILED (n0=%d)\n", n0);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!split_at (0.5f, 0)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-consolidate FAILED (split)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		int n1 = track_region_count (0);
		if (n1 < n0 + 1) {
			std::fprintf (stderr, "airdour_win_shell: smoke-consolidate FAILED (split regions %d→%d)\n",
			              n0, n1);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		/* Bounce range covering both halves → one consolidated region. */
		if (!consolidate_range (0, 0.f, 1.0f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-consolidate FAILED (consolidate)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		int n2 = track_region_count (0);
		if (n2 != 1) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-consolidate FAILED (regions %d→%d→%d want 1)\n",
			              n0, n1, n2);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!_snap.has_selection) {
			std::fprintf (stderr, "airdour_win_shell: smoke-consolidate FAILED (no selection)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		float len = _snap.selection_end_sec - _snap.selection_start_sec;
		if (len < 0.85f || len > 1.15f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-consolidate FAILED (len=%.3f)\n",
			              len);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-consolidate ok (regions %d→%d→%d len=%.2f)\n",
		              n0, n1, n2, len);
		std::fflush (stderr);
		_snap.detail = L"smoke-consolidate ok";
		set_snap_enabled (snap_was);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-consolidate exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_freeze ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-freeze failed — no Session";
		return false;
	}
	try {
		const bool snap_was = _snap.snap_enabled;
		set_snap_enabled (false);
		collect_tracks ();
		if (_impl->tracks.empty () || !_impl->tracks[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-freeze FAILED (no Audio 1)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!import_test_tone (0, 440.f, 1.0f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-freeze FAILED (import)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		int n0 = track_region_count (0);
		if (n0 < 1) {
			std::fprintf (stderr, "airdour_win_shell: smoke-freeze FAILED (n0=%d)\n", n0);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		refresh_freeze_flags ();
		if (_snap.track_frozen[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-freeze FAILED (already frozen)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!freeze_track (0) || !_snap.track_frozen[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-freeze FAILED (freeze)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		int n1 = track_region_count (0);
		if (n1 < 1) {
			std::fprintf (stderr, "airdour_win_shell: smoke-freeze FAILED (frozen regions=%d)\n", n1);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		std::shared_ptr<Playlist> pl = _impl->tracks[0]->playlist ();
		if (!pl || !pl->frozen ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-freeze FAILED (playlist not frozen)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		if (!unfreeze_track (0) || _snap.track_frozen[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-freeze FAILED (unfreeze)\n");
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		int n2 = track_region_count (0);
		if (n2 < 1) {
			std::fprintf (stderr, "airdour_win_shell: smoke-freeze FAILED (thawed regions=%d)\n", n2);
			std::fflush (stderr);
			set_snap_enabled (snap_was);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-freeze ok (regions %d→%d→%d frozen→thawed)\n",
		              n0, n1, n2);
		std::fflush (stderr);
		_snap.detail = L"smoke-freeze ok";
		set_snap_enabled (snap_was);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-freeze exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_undo ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-undo failed — no Session";
		return false;
	}
	try {
		/* Start from empty track 0 when possible. */
		if (_snap.track_has_regions[0]) {
			std::shared_ptr<Playlist> pl = _impl->tracks[0] ? _impl->tracks[0]->playlist () : std::shared_ptr<Playlist> ();
			if (pl) {
				std::shared_ptr<RegionList> all = pl->region_list ();
				if (all && !all->empty ()) {
					_impl->session->begin_reversible_command ("smoke clear");
					pl->clear_changes ();
					pl->clear_owned_changes ();
					RegionList copy = *all;
					for (RegionList::iterator it = copy.begin (); it != copy.end (); ++it) {
						if (*it) {
							pl->remove_region (*it);
						}
					}
					pl->rdiff_and_add_command (_impl->session);
					_impl->session->commit_reversible_command ();
				}
			}
			refresh_region_flags ();
		}

		if (!import_test_tone (0, 440.f, 1.2f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-undo FAILED (import)\n");
			std::fflush (stderr);
			return false;
		}
		if (!_snap.track_has_regions[0] || _snap.undo_depth == 0) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-undo FAILED (regions=%d undo_depth=%u)\n",
			              _snap.track_has_regions[0] ? 1 : 0, (unsigned)_snap.undo_depth);
			std::fflush (stderr);
			return false;
		}

		if (!undo (1) || _snap.track_has_regions[0]) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-undo FAILED (undo left regions=%d)\n",
			              _snap.track_has_regions[0] ? 1 : 0);
			std::fflush (stderr);
			return false;
		}

		if (!redo (1) || !_snap.track_has_regions[0]) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-undo FAILED (redo regions=%d)\n",
			              _snap.track_has_regions[0] ? 1 : 0);
			std::fflush (stderr);
			return false;
		}

		/* Leave session empty after verifying redo. */
		undo (1);
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-undo ok (import↔undo↔redo; undo_depth=%u)\n",
		              (unsigned)_snap.undo_depth);
		std::fflush (stderr);
		_snap.detail = L"smoke-undo ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-undo exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"smoke-undo exception";
		return false;
	} catch (...) {
		_snap.detail = L"smoke-undo unknown failure";
		return false;
	}
}

bool
EngineHost::smoke_history ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-history failed — no Session";
		return false;
	}
	try {
		if (!_snap.track_has_regions[0]) {
			if (!import_test_tone (0, 330.f, 1.0f) || !_snap.track_has_regions[0]) {
				std::fprintf (stderr, "airdour_win_shell: smoke-history FAILED (import)\n");
				std::fflush (stderr);
				return false;
			}
		}
		refresh_history_flags ();
		if (_snap.undo_depth == 0 || _snap.undo_list_count < 1 || !_snap.next_undo_name[0]) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-history FAILED (depth=%u list=%d next=%ls)\n",
			              (unsigned)_snap.undo_depth, _snap.undo_list_count, _snap.next_undo_name);
			std::fflush (stderr);
			return false;
		}
		uint32_t before = _snap.undo_depth;
		bool had_region = _snap.track_has_regions[0];
		if (!undo_to_index (0) || _snap.undo_depth >= before) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-history FAILED (jump depth %u→%u)\n",
			              (unsigned)before, (unsigned)_snap.undo_depth);
			std::fflush (stderr);
			return false;
		}
		if (had_region && _snap.track_has_regions[0]) {
			/* Jump should have removed the imported region when undoing import. */
			std::fprintf (stderr, "airdour_win_shell: smoke-history FAILED (region still present after jump)\n");
			std::fflush (stderr);
			return false;
		}
		if (!redo (1) || (had_region && !_snap.track_has_regions[0])) {
			std::fprintf (stderr, "airdour_win_shell: smoke-history FAILED (redo)\n");
			std::fflush (stderr);
			return false;
		}
		refresh_history_flags ();
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-history ok (list=%d next=\"%ls\" depths U%u/R%u)\n",
		              _snap.undo_list_count, _snap.next_undo_name,
		              (unsigned)_snap.undo_depth, (unsigned)_snap.redo_depth);
		std::fflush (stderr);
		_snap.detail = L"smoke-history ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-history exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

void
EngineHost::refresh_locations_snap ()
{
	_snap.loop_active = false;
	_snap.loop_playing = false;
	_snap.loop_start_sec = 0.f;
	_snap.loop_end_sec = 0.f;
	_snap.punch_range_ok = false;
	_snap.punch_enabled = false;
	_snap.punch_start_sec = 0.f;
	_snap.punch_end_sec = 0.f;
	_snap.marker_count = 0;
	for (int i = 0; i < EngineSnapshot::kMaxMarkers; ++i) {
		_snap.markers[i].sec = 0.f;
		_snap.markers[i].name[0] = L'\0';
	}
	if (!_impl || !_impl->session) {
		return;
	}
	try {
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0 && _impl->engine) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
		if (sr <= 0) {
			return;
		}
		Locations* locs = _impl->session->locations ();
		if (!locs) {
			return;
		}
		_snap.loop_playing = _impl->session->get_play_loop ();
		Location* loop = locs->auto_loop_location ();
		if (loop && !loop->is_hidden () && loop->end_sample () > loop->start_sample ()) {
			_snap.loop_active = true;
			_snap.loop_start_sec = (float)loop->start_sample () / (float)sr;
			_snap.loop_end_sec = (float)loop->end_sample () / (float)sr;
		}
		Location* punch = locs->auto_punch_location ();
		if (punch && !punch->is_hidden () && punch->end_sample () > punch->start_sample ()) {
			_snap.punch_range_ok = true;
			_snap.punch_start_sec = (float)punch->start_sample () / (float)sr;
			_snap.punch_end_sec = (float)punch->end_sample () / (float)sr;
		}
		_snap.punch_enabled = _impl->session->config.get_punch_in ()
		                      || _impl->session->config.get_punch_out ();
		const Locations::LocationList& list = locs->list ();
		int n = 0;
		for (Locations::LocationList::const_iterator it = list.begin ();
		     it != list.end () && n < EngineSnapshot::kMaxMarkers; ++it) {
			Location* loc = *it;
			if (!loc || !loc->is_mark () || loc->is_hidden () || loc->is_xrun ()) {
				continue;
			}
			_snap.markers[n].sec = (float)loc->start_sample () / (float)sr;
			std::wstring wname = utf8_to_wide (loc->name ());
			wcsncpy_s (_snap.markers[n].name, wname.c_str (), _TRUNCATE);
			++n;
		}
		_snap.marker_count = n;
	} catch (...) {
	}
}

bool
EngineHost::set_loop_range (float start_sec, float end_sec)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (end_sec < start_sec) {
		float t = start_sec;
		start_sec = end_sec;
		end_sec = t;
	}
	if (end_sec - start_sec < 0.05f) {
		end_sec = start_sec + 1.f;
	}
	try {
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0 && _impl->engine) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
		if (sr <= 0) {
			return false;
		}
		Locations* locs = _impl->session->locations ();
		if (!locs) {
			return false;
		}
		samplepos_t start_s = (samplepos_t)std::llround ((double)start_sec * (double)sr);
		samplepos_t end_s = (samplepos_t)std::llround ((double)end_sec * (double)sr);
		if (end_s <= start_s) {
			end_s = start_s + sr;
		}

		_impl->session->begin_reversible_command ("set auto loop range");
		Location* loop = locs->auto_loop_location ();
		if (!loop) {
			Location* loc = new Location (
				*_impl->session,
				timepos_t (start_s),
				timepos_t (end_s),
				"Loop",
				Location::IsAutoLoop);
			XMLNode& before = locs->get_state ();
			locs->add (loc, true);
			_impl->session->set_auto_loop_location (loc);
			XMLNode& after = locs->get_state ();
			_impl->session->add_command (new MementoCommand<Locations> (*locs, &before, &after));
		} else {
			XMLNode& before = loop->get_state ();
			loop->set_hidden (false, 0);
			loop->set (timepos_t (start_s), timepos_t (end_s));
			XMLNode& after = loop->get_state ();
			_impl->session->add_command (new MementoCommand<Location> (*loop, &before, &after));
		}
		_impl->session->commit_reversible_command ();
		refresh_locations_snap ();
		refresh_history_flags ();
		_snap.detail = L"loop range set";
		std::fprintf (stderr, "airdour_win_shell: loop [%.2f..%.2f]s\n",
		              _snap.loop_start_sec, _snap.loop_end_sec);
		std::fflush (stderr);
		return _snap.loop_active;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: set_loop_range exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"loop set failed";
		return false;
	} catch (...) {
		_snap.detail = L"loop set failed";
		return false;
	}
}

bool
EngineHost::set_loop_start (float start_sec)
{
	float end = _snap.loop_active ? _snap.loop_end_sec : (start_sec + 2.f);
	if (end <= start_sec + 0.05f) {
		end = start_sec + 2.f;
	}
	return set_loop_range (start_sec, end);
}

bool
EngineHost::set_loop_end (float end_sec)
{
	float start = _snap.loop_active ? _snap.loop_start_sec : 0.f;
	if (end_sec <= start + 0.05f) {
		start = std::max (0.f, end_sec - 2.f);
	}
	return set_loop_range (start, end_sec);
}

bool
EngineHost::set_loop_play (bool enable)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	try {
		if (enable && !_snap.loop_active) {
			/* Default loop around playhead if none yet. */
			float t = _snap.transport_sec;
			if (!set_loop_range (t, t + 2.f)) {
				return false;
			}
		}
		_impl->session->request_play_loop (enable, true);
		refresh_locations_snap ();
		_snap.detail = enable ? L"loop play on" : L"loop play off";
		std::fprintf (stderr, "airdour_win_shell: loop play %s\n", enable ? "on" : "off");
		std::fflush (stderr);
		return true;
	} catch (...) {
		_snap.detail = L"loop play failed";
		return false;
	}
}

bool
EngineHost::toggle_loop_play ()
{
	return set_loop_play (!_snap.loop_playing);
}

bool
EngineHost::set_punch_range (float start_sec, float end_sec)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	start_sec = snap_time (start_sec);
	end_sec = snap_time (end_sec);
	if (end_sec < start_sec) {
		float t = start_sec;
		start_sec = end_sec;
		end_sec = t;
	}
	if (end_sec - start_sec < 0.05f) {
		end_sec = start_sec + 1.f;
	}
	try {
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0 && _impl->engine) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
		if (sr <= 0) {
			return false;
		}
		Locations* locs = _impl->session->locations ();
		if (!locs) {
			return false;
		}
		samplepos_t start_s = (samplepos_t)std::llround ((double)start_sec * (double)sr);
		samplepos_t end_s = (samplepos_t)std::llround ((double)end_sec * (double)sr);
		if (end_s <= start_s) {
			end_s = start_s + sr;
		}

		_impl->session->begin_reversible_command ("set auto punch range");
		Location* punch = locs->auto_punch_location ();
		if (!punch) {
			Location* loc = new Location (
				*_impl->session,
				timepos_t (start_s),
				timepos_t (end_s),
				"Punch",
				Location::IsAutoPunch);
			XMLNode& before = locs->get_state ();
			locs->add (loc, true);
			_impl->session->set_auto_punch_location (loc);
			XMLNode& after = locs->get_state ();
			_impl->session->add_command (new MementoCommand<Locations> (*locs, &before, &after));
		} else {
			XMLNode& before = punch->get_state ();
			punch->set_hidden (false, 0);
			punch->set (timepos_t (start_s), timepos_t (end_s));
			XMLNode& after = punch->get_state ();
			_impl->session->add_command (new MementoCommand<Location> (*punch, &before, &after));
			/* Keep Session punch events wired to this location. */
			_impl->session->set_auto_punch_location (punch);
		}
		_impl->session->commit_reversible_command ();
		refresh_locations_snap ();
		refresh_history_flags ();
		_snap.detail = L"punch range set";
		std::fprintf (stderr, "airdour_win_shell: punch [%.2f..%.2f]s\n",
		              _snap.punch_start_sec, _snap.punch_end_sec);
		std::fflush (stderr);
		return _snap.punch_range_ok;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: set_punch_range exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"punch set failed";
		return false;
	} catch (...) {
		_snap.detail = L"punch set failed";
		return false;
	}
}

bool
EngineHost::set_punch_start (float start_sec)
{
	float end = _snap.punch_range_ok ? _snap.punch_end_sec : (start_sec + 2.f);
	if (end <= start_sec + 0.05f) {
		end = start_sec + 2.f;
	}
	return set_punch_range (start_sec, end);
}

bool
EngineHost::set_punch_end (float end_sec)
{
	float start = _snap.punch_range_ok ? _snap.punch_start_sec : 0.f;
	if (end_sec <= start + 0.05f) {
		start = std::max (0.f, end_sec - 2.f);
	}
	return set_punch_range (start, end_sec);
}

bool
EngineHost::copy_loop_to_punch ()
{
	if (!_snap.loop_active) {
		_snap.detail = L"copy punch — no loop range";
		return false;
	}
	return set_punch_range (_snap.loop_start_sec, _snap.loop_end_sec);
}

bool
EngineHost::set_punch_enabled (bool enable)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	try {
		if (enable) {
			if (!_snap.punch_range_ok) {
				if (_snap.loop_active) {
					if (!copy_loop_to_punch ()) {
						return false;
					}
				} else {
					float t = _snap.transport_sec;
					if (t < 0.f) {
						t = 0.f;
					}
					if (!set_punch_range (t, t + 2.f)) {
						return false;
					}
				}
			}
			/* Loop-play can block punch via Session punch/loop constraint. */
			if (_snap.loop_playing) {
				set_loop_play (false);
			}
			_impl->session->config.set_punch_in (true);
			_impl->session->config.set_punch_out (true);
			_snap.detail = L"punch armed — record only inside range";
		} else {
			_impl->session->config.set_punch_in (false);
			_impl->session->config.set_punch_out (false);
			_snap.detail = L"punch off";
		}
		refresh_locations_snap ();
		std::fprintf (stderr, "airdour_win_shell: punch %s (range=%d [%.2f..%.2f])\n",
		              enable ? "on" : "off",
		              _snap.punch_range_ok ? 1 : 0,
		              _snap.punch_start_sec, _snap.punch_end_sec);
		std::fflush (stderr);
		return enable ? _snap.punch_enabled : !_snap.punch_enabled;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: punch enable exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"punch enable failed";
		return false;
	} catch (...) {
		_snap.detail = L"punch enable failed";
		return false;
	}
}

bool
EngineHost::toggle_punch_enabled ()
{
	return set_punch_enabled (!_snap.punch_enabled);
}

void
EngineHost::refresh_tempo_snap ()
{
	_snap.tempo_bpm = 120.f;
	_snap.count_in_enabled = false;
	_snap.click_enabled = false;
	if (!_impl || !_impl->session) {
		return;
	}
	try {
		_snap.count_in_enabled = _impl->session->config.get_count_in ();
		_snap.click_enabled = Config->get_clicking ();
		Temporal::TempoMap::SharedPtr tm = Temporal::TempoMap::fetch ();
		if (tm) {
			Temporal::TempoPoint const& tp = tm->tempo_at (Temporal::timepos_t (0));
			double bpm = tp.quarter_notes_per_minute ();
			if (bpm > 1.0 && bpm < 1000.0) {
				_snap.tempo_bpm = (float)bpm;
			}
		}
	} catch (...) {
	}
}

bool
EngineHost::set_tempo_bpm (float bpm)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (bpm < 20.f) {
		bpm = 20.f;
	}
	if (bpm > 400.f) {
		bpm = 400.f;
	}
	try {
		Temporal::TempoMap::WritableSharedPtr tmap = Temporal::TempoMap::write_copy ();
		if (!tmap) {
			_snap.detail = L"tempo set failed — no map";
			return false;
		}
		Temporal::TempoMetric const& metric = tmap->metric_at (Temporal::timepos_t (0));
		/* Keep existing note type; set constant BPM. */
		int note_type = metric.tempo ().note_type ();
		if (note_type <= 0) {
			note_type = 4;
		}
		tmap->change_tempo (metric.get_editable_tempo (),
		                    Temporal::Tempo ((double)bpm, (uint8_t)note_type));
		Temporal::TempoMap::update (tmap);
		refresh_tempo_snap ();
		_snap.detail = L"tempo set";
		std::fprintf (stderr, "airdour_win_shell: tempo %.1f BPM\n", _snap.tempo_bpm);
		std::fflush (stderr);
		return std::fabs (_snap.tempo_bpm - bpm) < 0.6f;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: set_tempo_bpm exception: %s\n", e.what ());
		std::fflush (stderr);
		Temporal::TempoMap::abort_update ();
		_snap.detail = L"tempo set failed";
		return false;
	} catch (...) {
		Temporal::TempoMap::abort_update ();
		_snap.detail = L"tempo set failed";
		return false;
	}
}

bool
EngineHost::nudge_tempo_bpm (float delta)
{
	float next = _snap.tempo_bpm + delta;
	return set_tempo_bpm (next);
}

bool
EngineHost::set_count_in (bool enable)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	try {
		_impl->session->config.set_count_in (enable);
		refresh_tempo_snap ();
		_snap.detail = enable ? L"count-in on — 1–2 bars before record"
		                      : L"count-in off";
		std::fprintf (stderr, "airdour_win_shell: count-in %s\n", enable ? "on" : "off");
		std::fflush (stderr);
		return enable ? _snap.count_in_enabled : !_snap.count_in_enabled;
	} catch (...) {
		_snap.detail = L"count-in set failed";
		return false;
	}
}

bool
EngineHost::toggle_count_in ()
{
	return set_count_in (!_snap.count_in_enabled);
}

bool
EngineHost::set_clicking (bool enable)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	try {
		/* Ensure click-record-only is off so click is audible on Play, not only Rec. */
		if (enable && Config->get_click_record_only ()) {
			Config->set_click_record_only (false);
		}
		Config->set_clicking (enable);
		/* ParameterChanged updates Session::_clicking; also reconnect if needed. */
		if (enable) {
			_impl->session->reconnect_click_output ();
		}
		refresh_tempo_snap ();
		_snap.detail = enable ? L"click on — metronome"
		                      : L"click off";
		std::fprintf (stderr, "airdour_win_shell: click %s\n", enable ? "on" : "off");
		std::fflush (stderr);
		return enable ? _snap.click_enabled : !_snap.click_enabled;
	} catch (...) {
		_snap.detail = L"click set failed";
		return false;
	}
}

bool
EngineHost::toggle_clicking ()
{
	return set_clicking (!_snap.click_enabled);
}

bool
EngineHost::smoke_click ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-click failed — no Session";
		return false;
	}
	try {
		if (!set_clicking (false) || _snap.click_enabled || Config->get_clicking ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-click FAILED (disable)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_clicking (true) || !_snap.click_enabled || !Config->get_clicking ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-click FAILED (enable)\n");
			std::fflush (stderr);
			return false;
		}
		if (!_impl->session->click_io ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-click FAILED (no ClickIO)\n");
			std::fflush (stderr);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-click ok (off→on + ClickIO)\n");
		std::fflush (stderr);
		_snap.detail = L"smoke-click ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-click exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::add_audio_track ()
{
	if (!_impl || !_impl->session) {
		return false;
	}
	collect_tracks ();
	if ((int)_impl->tracks.size () >= EngineSnapshot::kMeters) {
		_snap.detail = L"add audio — at max tracks";
		return false;
	}
	try {
		AudioTrackList created = _impl->session->new_audio_track (
			1, 2,
			std::shared_ptr<RouteGroup> (),
			1u,
			"Audio",
			PresentationInfo::max_order);
		if (created.empty () || !created.front ()) {
			_snap.detail = L"add audio failed";
			return false;
		}
		size_t idx = _impl->tracks.size ();
		if (idx < (size_t)EngineSnapshot::kMeters
		    && created.front ()->gain_control ()) {
			created.front ()->gain_control ()->set_value (
				_track_gain[idx < (size_t)EngineSnapshot::kMeters ? idx : 0],
				Controllable::NoGroup);
		}
		collect_tracks ();
		ensure_aux_sends ();
		refresh_region_flags ();
		refresh_mute_solo_flags ();
		refresh_pan_flags ();
		refresh_send_flags ();
		_snap.detail = L"audio track added";
		std::fprintf (stderr, "airdour_win_shell: add audio → count=%d\n",
		              _snap.audio_track_count);
		std::fflush (stderr);
		return _snap.audio_track_count > 0;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: add_audio_track: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"add audio failed";
		return false;
	} catch (...) {
		_snap.detail = L"add audio failed";
		return false;
	}
}

bool
EngineHost::add_midi_track ()
{
	if (!ensure_midi_track ()) {
		return false;
	}
	collect_tracks ();
	_snap.detail = L"MIDI track ready";
	return _snap.midi_track_count > 0 || _snap.midi_track_ok;
}

bool
EngineHost::remove_audio_track (int index, bool require_empty_or_selected)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	collect_tracks ();
	if ((int)_impl->tracks.size () <= 1) {
		_snap.detail = L"remove audio — need ≥1";
		return false;
	}
	if (index < 0 || index >= (int)_impl->tracks.size () || !_impl->tracks[index]) {
		_snap.detail = L"remove audio — bad index";
		return false;
	}
	refresh_region_flags ();
	bool empty = !_snap.track_has_regions[index];
	bool selected = (_snap.selection_track == index);
	if (require_empty_or_selected && !empty && !selected) {
		_snap.detail = L"remove audio — not empty/selected";
		return false;
	}
	try {
		std::shared_ptr<Route> r = _impl->tracks[index];
		if (_snap.selection_track == index) {
			_snap.has_selection = false;
			_snap.selection_track = -1;
			_impl->selected_track = -1;
			_impl->selected_region.reset ();
		} else if (_snap.selection_track > index) {
			_snap.selection_track -= 1;
			_impl->selected_track = _snap.selection_track;
		}
		_impl->session->remove_route (r);
		collect_tracks ();
		ensure_aux_sends ();
		refresh_region_flags ();
		refresh_mute_solo_flags ();
		refresh_pan_flags ();
		refresh_send_flags ();
		refresh_selection_snap ();
		_snap.detail = L"audio track removed";
		std::fprintf (stderr, "airdour_win_shell: remove audio[%d] → count=%d\n",
		              index, _snap.audio_track_count);
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: remove_audio_track: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"remove audio failed";
		return false;
	} catch (...) {
		_snap.detail = L"remove audio failed";
		return false;
	}
}

bool
EngineHost::remove_selected_or_empty_audio_track ()
{
	collect_tracks ();
	refresh_region_flags ();
	if (_snap.has_selection && _snap.selection_track >= 0) {
		return remove_audio_track (_snap.selection_track, false);
	}
	for (int i = (int)_impl->tracks.size () - 1; i >= 0; --i) {
		if (i < EngineSnapshot::kMeters && !_snap.track_has_regions[i]) {
			return remove_audio_track (i, true);
		}
	}
	_snap.detail = L"remove — no selected/empty audio";
	return false;
}

bool
EngineHost::remove_midi_track ()
{
	if (!_impl || !_impl->session || !_impl->midi_track) {
		_snap.detail = L"remove MIDI — none";
		return false;
	}
	try {
		std::shared_ptr<Route> r = _impl->midi_track;
		_impl->midi_region.reset ();
		_impl->midi_track.reset ();
		_impl->midi_synth_ok = false;
		_impl->midi_synth_to_master = false;
		_impl->midi_synth_name.clear ();
		_impl->selected_midi_note_id = -1;
		_impl->session->remove_route (r);
		collect_tracks ();
		refresh_midi_snap ();
		_snap.midi_track_count = 0;
		_snap.detail = L"MIDI track removed";
		std::fprintf (stderr, "airdour_win_shell: remove MIDI\n");
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: remove_midi_track: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"remove MIDI failed";
		return false;
	} catch (...) {
		_snap.detail = L"remove MIDI failed";
		return false;
	}
}

bool
EngineHost::smoke_tracks ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-tracks failed — no Session";
		return false;
	}
	try {
		collect_tracks ();
		const int n0 = _snap.audio_track_count;
		if (n0 < 1) {
			std::fprintf (stderr, "airdour_win_shell: smoke-tracks FAILED (no audio)\n");
			std::fflush (stderr);
			return false;
		}
		/* Default session now fills Audio 1–6 (kMeters). At max: remove then
		 * re-add; otherwise add then remove. */
		if (n0 >= EngineSnapshot::kMeters) {
			if (!remove_audio_track (n0 - 1, true)
			    || _snap.audio_track_count != n0 - 1) {
				std::fprintf (stderr, "airdour_win_shell: smoke-tracks FAILED (remove@max → %d)\n",
				              _snap.audio_track_count);
				std::fflush (stderr);
				return false;
			}
			if (!add_audio_track () || _snap.audio_track_count != n0) {
				std::fprintf (stderr, "airdour_win_shell: smoke-tracks FAILED (re-add → %d)\n",
				              _snap.audio_track_count);
				std::fflush (stderr);
				return false;
			}
		} else {
			if (!add_audio_track () || _snap.audio_track_count != n0 + 1) {
				std::fprintf (stderr, "airdour_win_shell: smoke-tracks FAILED (add → %d)\n",
				              _snap.audio_track_count);
				std::fflush (stderr);
				return false;
			}
			const int n1 = _snap.audio_track_count;
			/* New track is empty — remove it. */
			if (!remove_audio_track (n1 - 1, true) || _snap.audio_track_count != n0) {
				std::fprintf (stderr, "airdour_win_shell: smoke-tracks FAILED (remove → %d)\n",
				              _snap.audio_track_count);
				std::fflush (stderr);
				return false;
			}
		}
		bool had_midi = _snap.midi_track_ok || _impl->midi_track;
		if (had_midi) {
			if (!remove_midi_track () || _snap.midi_track_count != 0) {
				std::fprintf (stderr, "airdour_win_shell: smoke-tracks FAILED (remove MIDI)\n");
				std::fflush (stderr);
				return false;
			}
		}
		if (!add_midi_track () || (!_snap.midi_track_ok && _snap.midi_track_count < 1)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-tracks FAILED (add MIDI)\n");
			std::fflush (stderr);
			return false;
		}
		collect_tracks ();
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-tracks ok (audio %d→…→%d midi ok)\n",
		              n0, _snap.audio_track_count);
		std::fflush (stderr);
		_snap.detail = L"smoke-tracks ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-tracks exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_name (int track, const std::string& name)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	collect_tracks ();
	if (track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		_snap.detail = L"rename — bad index";
		return false;
	}
	if (name.empty ()) {
		_snap.detail = L"rename — empty name";
		return false;
	}
	try {
		std::shared_ptr<Track> t = std::dynamic_pointer_cast<Track> (_impl->tracks[track]);
		if (!t) {
			_snap.detail = L"rename — not a Track";
			return false;
		}
		if (!t->set_name (name)) {
			_snap.detail = L"rename failed (rec-armed?)";
			std::fprintf (stderr, "airdour_win_shell: set_track_name[%d] failed\n", track);
			std::fflush (stderr);
			return false;
		}
		collect_tracks ();
		_snap.detail = L"track renamed";
		std::fprintf (stderr, "airdour_win_shell: rename audio[%d] → %s\n",
		              track, wide_to_utf8 (_snap.track_name[track]).c_str ());
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: set_track_name: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"rename failed";
		return false;
	} catch (...) {
		_snap.detail = L"rename failed";
		return false;
	}
}

bool
EngineHost::set_midi_track_name (const std::string& name)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (!_impl->midi_track) {
		_snap.detail = L"rename MIDI — none";
		return false;
	}
	if (name.empty ()) {
		_snap.detail = L"rename — empty name";
		return false;
	}
	try {
		std::shared_ptr<Track> t = std::dynamic_pointer_cast<Track> (_impl->midi_track);
		if (!t || !t->set_name (name)) {
			_snap.detail = L"rename MIDI failed (rec-armed?)";
			return false;
		}
		collect_tracks ();
		_snap.detail = L"MIDI renamed";
		std::fprintf (stderr, "airdour_win_shell: rename MIDI → %s\n",
		              wide_to_utf8 (_snap.midi_track_name).c_str ());
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: set_midi_track_name: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"rename MIDI failed";
		return false;
	} catch (...) {
		_snap.detail = L"rename MIDI failed";
		return false;
	}
}

bool
EngineHost::move_audio_track (int index, int delta)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	collect_tracks ();
	const int n = (int)_impl->tracks.size ();
	if (n < 2) {
		_snap.detail = L"reorder — need ≥2 audio";
		return false;
	}
	const int other = index + delta;
	if (index < 0 || index >= n || other < 0 || other >= n
	    || !_impl->tracks[index] || !_impl->tracks[other]) {
		_snap.detail = L"reorder — edge / bad index";
		return false;
	}
	try {
		std::shared_ptr<AudioTrack> a = _impl->tracks[index];
		std::shared_ptr<AudioTrack> b = _impl->tracks[other];
		PresentationInfo::order_t oa = a->presentation_info ().order ();
		PresentationInfo::order_t ob = b->presentation_info ().order ();
		{
			PresentationInfo::ChangeSuspender cs;
			a->set_presentation_order (ob);
			b->set_presentation_order (oa);
		}
		/* Mirror UI gain cache with the swapped Session order. */
		if (index < EngineSnapshot::kMeters && other < EngineSnapshot::kMeters) {
			float tmp = _track_gain[index];
			_track_gain[index] = _track_gain[other];
			_track_gain[other] = tmp;
			if (index == 0 || other == 0) {
				_gain = _track_gain[0];
				_snap.gain = _gain;
			}
		}
		if (_snap.selection_track == index) {
			_snap.selection_track = other;
			_impl->selected_track = other;
		} else if (_snap.selection_track == other) {
			_snap.selection_track = index;
			_impl->selected_track = index;
		}
		collect_tracks ();
		/* Prefer Session gain_control as source of truth after reorder. */
		for (size_t i = 0; i < _impl->tracks.size () && i < (size_t)EngineSnapshot::kMeters; ++i) {
			if (_impl->tracks[i] && _impl->tracks[i]->gain_control ()) {
				_track_gain[i] = (float)_impl->tracks[i]->gain_control ()->get_value ();
			}
		}
		_gain = _track_gain[0];
		_snap.gain = _gain;
		refresh_region_flags ();
		refresh_mute_solo_flags ();
		refresh_pan_flags ();
		refresh_send_flags ();
		refresh_selection_snap ();
		_snap.detail = L"tracks reordered";
		std::fprintf (stderr, "airdour_win_shell: move audio[%d] %+d → [%d]\n",
		              index, delta, other);
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: move_audio_track: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"reorder failed";
		return false;
	} catch (...) {
		_snap.detail = L"reorder failed";
		return false;
	}
}

bool
EngineHost::smoke_rename ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-rename failed — no Session";
		return false;
	}
	try {
		collect_tracks ();
		if (_snap.audio_track_count < 2) {
			if (!add_audio_track () || _snap.audio_track_count < 2) {
				std::fprintf (stderr, "airdour_win_shell: smoke-rename FAILED (need ≥2 audio)\n");
				std::fflush (stderr);
				return false;
			}
		}
		const std::string name0 = wide_to_utf8 (_snap.track_name[0]);
		const std::string name1 = wide_to_utf8 (_snap.track_name[1]);
		if (!set_track_name (0, "Kick")) {
			std::fprintf (stderr, "airdour_win_shell: smoke-rename FAILED (rename Kick)\n");
			std::fflush (stderr);
			return false;
		}
		const std::string after = wide_to_utf8 (_snap.track_name[0]);
		if (after.find ("Kick") == std::string::npos) {
			std::fprintf (stderr, "airdour_win_shell: smoke-rename FAILED (name=%s)\n",
			              after.c_str ());
			std::fflush (stderr);
			return false;
		}
		if (!move_audio_track (0, +1)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-rename FAILED (swap down)\n");
			std::fflush (stderr);
			return false;
		}
		const std::string at1 = wide_to_utf8 (_snap.track_name[1]);
		if (at1.find ("Kick") == std::string::npos) {
			std::fprintf (stderr, "airdour_win_shell: smoke-rename FAILED (Kick not at [1]=%s)\n",
			              at1.c_str ());
			std::fflush (stderr);
			return false;
		}
		if (!move_audio_track (1, -1)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-rename FAILED (swap up)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_track_name (0, name0.empty () ? "Audio1" : name0)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-rename FAILED (restore name)\n");
			std::fflush (stderr);
			return false;
		}
		(void)name1;
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-rename ok (Kick rename + reorder + restore)\n");
		std::fflush (stderr);
		_snap.detail = L"smoke-rename ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-rename exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

void
EngineHost::refresh_group_flags ()
{
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_snap.track_in_group[i] = false;
	}
	_snap.group_member_count = 0;
	_snap.group_active = false;
	_snap.group_shares_mute = false;
	_snap.group_shares_solo = false;
	_snap.group_shares_gain = false;
	_snap.group_name[0] = L'\0';
	if (!_impl) {
		return;
	}
	try {
		std::shared_ptr<RouteGroup> rg = _impl->shell_group;
		if (!rg && _impl->session) {
			rg = _impl->session->route_group_by_name ("Group 1");
			_impl->shell_group = rg;
		}
		if (!rg) {
			return;
		}
		copy_name_to_snap (_snap.group_name, EngineSnapshot::kGroupNameLen, rg->name ());
		_snap.group_active = rg->is_active ();
		_snap.group_shares_mute = rg->is_mute ();
		_snap.group_shares_solo = rg->is_solo ();
		_snap.group_shares_gain = rg->is_gain ();
		int n = 0;
		for (int i = 0; i < (int)_impl->tracks.size () && i < EngineSnapshot::kMeters; ++i) {
			if (_impl->tracks[i] && _impl->tracks[i]->route_group () == rg) {
				_snap.track_in_group[i] = true;
				++n;
			}
		}
		_snap.group_member_count = n;
	} catch (...) {
	}
}

bool
EngineHost::create_route_group (const int* indices, int n)
{
	if (!_impl || !_impl->session || !indices || n < 2) {
		_snap.detail = L"group — need ≥2 tracks";
		return false;
	}
	collect_tracks ();
	try {
		std::shared_ptr<RouteGroup> rg = _impl->session->route_group_by_name ("Group 1");
		if (!rg) {
			rg = _impl->session->new_route_group ("Group 1");
			if (!rg) {
				_snap.detail = L"group create failed";
				return false;
			}
			_impl->session->add_route_group (rg);
		}
		/* Honest scope: mute + solo + gain only. */
		rg->set_active (true, nullptr);
		rg->set_mute (true);
		rg->set_solo (true);
		rg->set_gain (true);
		rg->set_recenable (false);
		rg->set_select (false);
		rg->set_route_active (false);
		rg->set_color (false);
		rg->set_monitoring (false);
		rg->set_sursend_enable (false);

		rg->clear ();
		int added = 0;
		for (int k = 0; k < n; ++k) {
			int idx = indices[k];
			if (idx < 0 || idx >= (int)_impl->tracks.size () || !_impl->tracks[idx]) {
				continue;
			}
			if (rg->add (_impl->tracks[idx]) == 0) {
				++added;
			}
		}
		if (added < 2) {
			rg->clear ();
			_impl->shell_group = rg;
			refresh_group_flags ();
			_snap.detail = L"group — <2 valid tracks";
			return false;
		}
		_impl->shell_group = rg;
		refresh_group_flags ();
		_snap.detail = L"route group ready";
		std::fprintf (stderr, "airdour_win_shell: RouteGroup \"%s\" members=%d mute/solo/gain\n",
		              rg->name ().c_str (), _snap.group_member_count);
		std::fflush (stderr);
		return _snap.group_member_count >= 2;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: create_route_group: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"group failed";
		return false;
	} catch (...) {
		_snap.detail = L"group failed";
		return false;
	}
}

bool
EngineHost::clear_route_group ()
{
	if (!_impl || !_impl->session) {
		return false;
	}
	try {
		std::shared_ptr<RouteGroup> rg = _impl->shell_group;
		if (!rg) {
			rg = _impl->session->route_group_by_name ("Group 1");
		}
		if (!rg) {
			refresh_group_flags ();
			_snap.detail = L"no group";
			return true;
		}
		_impl->session->remove_route_group (rg);
		_impl->shell_group.reset ();
		refresh_group_flags ();
		_snap.detail = L"group cleared";
		std::fprintf (stderr, "airdour_win_shell: RouteGroup cleared\n");
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: clear_route_group: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_route_group_active (bool enable)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	try {
		std::shared_ptr<RouteGroup> rg = _impl->shell_group;
		if (!rg) {
			rg = _impl->session->route_group_by_name ("Group 1");
			_impl->shell_group = rg;
		}
		if (!rg) {
			_snap.detail = L"no group to activate";
			return false;
		}
		rg->set_active (enable, nullptr);
		refresh_group_flags ();
		_snap.detail = enable ? L"group active" : L"group inactive";
		return _snap.group_active == enable;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_route_group_active ()
{
	refresh_group_flags ();
	return set_route_group_active (!_snap.group_active);
}

bool
EngineHost::smoke_group ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-group failed — no Session";
		return false;
	}
	try {
		collect_tracks ();
		if (_snap.audio_track_count < 2) {
			std::fprintf (stderr, "airdour_win_shell: smoke-group FAILED (need ≥2 audio)\n");
			std::fflush (stderr);
			return false;
		}
		const int idxs[2] = { 0, 1 };
		if (!create_route_group (idxs, 2) || _snap.group_member_count < 2
		    || !_snap.track_in_group[0] || !_snap.track_in_group[1]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-group FAILED (create)\n");
			std::fflush (stderr);
			return false;
		}
		set_track_muted (0, false);
		set_track_muted (1, false);
		set_track_soloed (0, false);
		set_track_soloed (1, false);
		if (!set_track_muted (0, true) || !_snap.track_muted[0] || !_snap.track_muted[1]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-group FAILED (mute propagate %d/%d)\n",
			              (int)_snap.track_muted[0], (int)_snap.track_muted[1]);
			std::fflush (stderr);
			clear_route_group ();
			return false;
		}
		if (!set_track_muted (0, false) || _snap.track_muted[0] || _snap.track_muted[1]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-group FAILED (unmute)\n");
			std::fflush (stderr);
			clear_route_group ();
			return false;
		}
		if (!set_track_soloed (0, true) || !_snap.track_soloed[0] || !_snap.track_soloed[1]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-group FAILED (solo propagate)\n");
			std::fflush (stderr);
			clear_route_group ();
			return false;
		}
		set_track_soloed (0, false);
		set_track_gain (0, 0.42f);
		Sleep (60);
		refresh_group_flags ();
		collect_tracks ();
		float g0 = _snap.track_gain[0];
		float g1 = _snap.track_gain[1];
		if (std::fabs (g0 - 0.42f) > 0.08f || std::fabs (g1 - 0.42f) > 0.08f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-group FAILED (gain %.2f/%.2f)\n",
			              g0, g1);
			std::fflush (stderr);
			clear_route_group ();
			return false;
		}
		if (!clear_route_group () || _snap.group_member_count != 0) {
			std::fprintf (stderr, "airdour_win_shell: smoke-group FAILED (clear)\n");
			std::fflush (stderr);
			return false;
		}
		set_track_muted (0, true);
		wait_until ([&] () {
			refresh_mute_solo_flags ();
			return _snap.track_muted[0];
		});
		if (!_snap.track_muted[0] || _snap.track_muted[1]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-group FAILED (ungrouped mute)\n");
			std::fflush (stderr);
			set_track_muted (0, false);
			return false;
		}
		set_track_muted (0, false);
		std::fprintf (stderr, "airdour_win_shell: smoke-group ok (mute/solo/gain + clear)\n");
		std::fflush (stderr);
		_snap.detail = L"smoke-group ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

void
EngineHost::refresh_monitor_flags ()
{
	_snap.monitor_ok = false;
	_snap.monitor_dim = false;
	_snap.monitor_mono = false;
	_snap.monitor_cut = false;
	if (!_impl || !_impl->session) {
		return;
	}
	try {
		std::shared_ptr<Route> mon = _impl->session->monitor_out ();
		if (!mon || !mon->monitor_control ()) {
			return;
		}
		std::shared_ptr<MonitorProcessor> mp = mon->monitor_control ();
		_snap.monitor_ok = true;
		_snap.monitor_dim = mp->dim_all ();
		_snap.monitor_mono = mp->mono ();
		_snap.monitor_cut = mp->cut_all ();
	} catch (...) {
	}
}

bool
EngineHost::ensure_monitor_section ()
{
	if (!_impl || !_impl->session) {
		return false;
	}
	try {
		if (_impl->session->monitor_out ()
		    && _impl->session->monitor_out ()->monitor_control ()) {
			refresh_monitor_flags ();
			return _snap.monitor_ok;
		}
		if (!_impl->session->master_out ()) {
			_snap.detail = L"monitor — no master";
			return false;
		}
		/* Public path: RC Config use-monitor-bus → Session::config_changed
		 * → add_monitor_section() (private). */
		if (!Config->get_use_monitor_bus ()) {
			Config->set_use_monitor_bus (true);
		}
		refresh_monitor_flags ();
		if (!_snap.monitor_ok) {
			_snap.detail = L"monitor create failed";
			std::fprintf (stderr, "airdour_win_shell: ensure_monitor_section FAILED\n");
			std::fflush (stderr);
			return false;
		}
		_snap.detail = L"monitor section ready";
		std::fprintf (stderr, "airdour_win_shell: Monitor section ready (dim/mono/cut)\n");
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: ensure_monitor_section: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"monitor failed";
		return false;
	} catch (...) {
		_snap.detail = L"monitor failed";
		return false;
	}
}

bool
EngineHost::set_monitor_dim (bool enable)
{
	if (!ensure_monitor_section ()) {
		return false;
	}
	try {
		std::shared_ptr<MonitorProcessor> mp =
			_impl->session->monitor_out ()->monitor_control ();
		mp->set_dim_all (enable);
		refresh_monitor_flags ();
		_snap.detail = enable ? L"monitor dim" : L"monitor undim";
		return _snap.monitor_dim == enable;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_monitor_dim ()
{
	refresh_monitor_flags ();
	return set_monitor_dim (!_snap.monitor_dim);
}

bool
EngineHost::set_monitor_mono (bool enable)
{
	if (!ensure_monitor_section ()) {
		return false;
	}
	try {
		std::shared_ptr<MonitorProcessor> mp =
			_impl->session->monitor_out ()->monitor_control ();
		mp->set_mono (enable);
		refresh_monitor_flags ();
		_snap.detail = enable ? L"monitor mono" : L"monitor stereo";
		return _snap.monitor_mono == enable;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_monitor_mono ()
{
	refresh_monitor_flags ();
	return set_monitor_mono (!_snap.monitor_mono);
}

bool
EngineHost::set_monitor_cut (bool enable)
{
	if (!ensure_monitor_section ()) {
		return false;
	}
	try {
		std::shared_ptr<MonitorProcessor> mp =
			_impl->session->monitor_out ()->monitor_control ();
		mp->set_cut_all (enable);
		refresh_monitor_flags ();
		_snap.detail = enable ? L"monitor mute" : L"monitor unmute";
		return _snap.monitor_cut == enable;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_monitor_cut ()
{
	refresh_monitor_flags ();
	return set_monitor_cut (!_snap.monitor_cut);
}

bool
EngineHost::smoke_monitor ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-monitor failed — no Session";
		return false;
	}
	try {
		if (!ensure_monitor_section ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-monitor FAILED (ensure)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_monitor_dim (true) || !_snap.monitor_dim) {
			std::fprintf (stderr, "airdour_win_shell: smoke-monitor FAILED (dim on)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_monitor_mono (true) || !_snap.monitor_mono) {
			std::fprintf (stderr, "airdour_win_shell: smoke-monitor FAILED (mono on)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_monitor_cut (true) || !_snap.monitor_cut) {
			std::fprintf (stderr, "airdour_win_shell: smoke-monitor FAILED (cut on)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_monitor_dim (false) || _snap.monitor_dim
		    || !set_monitor_mono (false) || _snap.monitor_mono
		    || !set_monitor_cut (false) || _snap.monitor_cut) {
			std::fprintf (stderr, "airdour_win_shell: smoke-monitor FAILED (clear)\n");
			std::fflush (stderr);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-monitor ok (dim/mono/cut)\n");
		std::fflush (stderr);
		_snap.detail = L"smoke-monitor ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-monitor exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}


bool
EngineHost::smoke_punch_toggle ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-punch-toggle failed — no Session";
		return false;
	}
	try {
		if (!set_punch_range (0.5f, 2.5f) || !_snap.punch_range_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-punch-toggle FAILED (range)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_punch_enabled (false) || _snap.punch_enabled) {
			std::fprintf (stderr, "airdour_win_shell: smoke-punch-toggle FAILED (start off)\n");
			std::fflush (stderr);
			return false;
		}
		if (!toggle_punch_enabled () || !_snap.punch_enabled) {
			std::fprintf (stderr, "airdour_win_shell: smoke-punch-toggle FAILED (on)\n");
			std::fflush (stderr);
			return false;
		}
		if (!_impl->session->config.get_punch_in ()
		    || !_impl->session->config.get_punch_out ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-punch-toggle FAILED (config)\n");
			std::fflush (stderr);
			return false;
		}
		if (!toggle_punch_enabled () || _snap.punch_enabled) {
			std::fprintf (stderr, "airdour_win_shell: smoke-punch-toggle FAILED (off)\n");
			std::fflush (stderr);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-punch-toggle ok (off→on→off)\n");
		std::fflush (stderr);
		_snap.detail = L"smoke-punch-toggle ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-punch-toggle exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_tempo ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-tempo failed — no Session";
		return false;
	}
	try {
		/* Anchor at 120 so snap expectations are stable. */
		if (!set_tempo_bpm (120.f) || std::fabs (_snap.tempo_bpm - 120.f) > 0.6f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-tempo FAILED (set 120 → %.1f)\n",
			              _snap.tempo_bpm);
			std::fflush (stderr);
			return false;
		}
		set_snap_density (1);
		set_snap_enabled (true);
		float at120 = snap_time (0.60f);
		if (std::fabs (at120 - 0.50f) > 0.08f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-tempo FAILED (120 BPM snap 0.60→%.3f)\n",
			              at120);
			std::fflush (stderr);
			return false;
		}
		if (!set_tempo_bpm (60.f) || std::fabs (_snap.tempo_bpm - 60.f) > 0.6f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-tempo FAILED (set 60 → %.1f)\n",
			              _snap.tempo_bpm);
			std::fflush (stderr);
			return false;
		}
		float at60 = snap_time (0.60f);
		if (std::fabs (at60 - 1.00f) > 0.08f && std::fabs (at60 - 0.00f) > 0.08f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-tempo FAILED (60 BPM snap 0.60→%.3f)\n",
			              at60);
			std::fflush (stderr);
			return false;
		}
		if (!set_tempo_bpm (140.f) || std::fabs (_snap.tempo_bpm - 140.f) > 0.6f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-tempo FAILED (set 140 → %.1f)\n",
			              _snap.tempo_bpm);
			std::fflush (stderr);
			return false;
		}
		if (!set_count_in (true) || !_snap.count_in_enabled) {
			std::fprintf (stderr, "airdour_win_shell: smoke-tempo FAILED (count-in on)\n");
			std::fflush (stderr);
			return false;
		}
		if (!_impl->session->config.get_count_in ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-tempo FAILED (config count-in)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_count_in (false) || _snap.count_in_enabled) {
			std::fprintf (stderr, "airdour_win_shell: smoke-tempo FAILED (count-in off)\n");
			std::fflush (stderr);
			return false;
		}
		/* Restore defaults for later smokes / interactive. */
		set_tempo_bpm (120.f);
		set_snap_enabled (false);
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-tempo ok "
		              "(120→%.2f 60→%.2f bpm=140 count-in)\n",
		              at120, at60);
		std::fflush (stderr);
		_snap.detail = L"smoke-tempo ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-tempo exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_punch ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-punch failed — no Session";
		return false;
	}
	try {
		if (!set_punch_range (0.5f, 2.5f) || !_snap.punch_range_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-punch FAILED (set range)\n");
			std::fflush (stderr);
			return false;
		}
		if (std::fabs (_snap.punch_start_sec - 0.5f) > 0.05f
		    || std::fabs (_snap.punch_end_sec - 2.5f) > 0.05f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-punch FAILED (range %.2f..%.2f)\n",
			              _snap.punch_start_sec, _snap.punch_end_sec);
			std::fflush (stderr);
			return false;
		}
		if (!set_punch_enabled (true) || !_snap.punch_enabled) {
			std::fprintf (stderr, "airdour_win_shell: smoke-punch FAILED (enable)\n");
			std::fflush (stderr);
			return false;
		}
		if (!_impl->session->config.get_punch_in ()
		    || !_impl->session->config.get_punch_out ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-punch FAILED (config in/out)\n");
			std::fflush (stderr);
			return false;
		}
		if (!_snap.punch_enabled || !_snap.punch_range_ok
		    || !_impl->session->locations ()->auto_punch_location ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-punch FAILED (punch location)\n");
			std::fflush (stderr);
			return false;
		}
		/* Copy-from-loop path: set loop then copy. */
		if (!set_loop_range (1.0f, 3.0f) || !copy_loop_to_punch ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-punch FAILED (copy loop)\n");
			std::fflush (stderr);
			return false;
		}
		if (std::fabs (_snap.punch_start_sec - 1.0f) > 0.05f
		    || std::fabs (_snap.punch_end_sec - 3.0f) > 0.05f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-punch FAILED (copied %.2f..%.2f)\n",
			              _snap.punch_start_sec, _snap.punch_end_sec);
			std::fflush (stderr);
			return false;
		}
		if (!set_punch_enabled (false) || _snap.punch_enabled) {
			std::fprintf (stderr, "airdour_win_shell: smoke-punch FAILED (disable)\n");
			std::fflush (stderr);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-punch ok (range+enable+copy-loop+disable)\n");
		std::fflush (stderr);
		_snap.detail = L"smoke-punch ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-punch exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::add_marker_at (float time_sec, const std::string& name)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	try {
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0 && _impl->engine) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
		if (sr <= 0) {
			return false;
		}
		Locations* locs = _impl->session->locations ();
		if (!locs) {
			return false;
		}
		if (time_sec < 0.f) {
			time_sec = 0.f;
		}
		samplepos_t sample = (samplepos_t)std::llround ((double)time_sec * (double)sr);
		std::string marker_name = name;
		if (marker_name.empty ()) {
			locs->next_available_name (marker_name, "mark");
		}
		timepos_t where (sample);
		Location* location = new Location (
			*_impl->session, where, where, marker_name, Location::IsMark);

		_impl->session->begin_reversible_command ("add marker");
		XMLNode& before = locs->get_state ();
		locs->add (location, true);
		XMLNode& after = locs->get_state ();
		_impl->session->add_command (new MementoCommand<Locations> (*locs, &before, &after));
		_impl->session->commit_reversible_command ();

		refresh_locations_snap ();
		refresh_history_flags ();
		_snap.detail = L"marker added";
		std::fprintf (stderr, "airdour_win_shell: marker \"%s\" @ %.2fs (count=%d)\n",
		              marker_name.c_str (), time_sec, _snap.marker_count);
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: add_marker exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"marker add failed";
		return false;
	} catch (...) {
		_snap.detail = L"marker add failed";
		return false;
	}
}

bool
EngineHost::remove_marker_near (float time_sec, float slop_sec)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	try {
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0 && _impl->engine) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
		if (sr <= 0) {
			return false;
		}
		Locations* locs = _impl->session->locations ();
		if (!locs) {
			return false;
		}
		samplepos_t sample = (samplepos_t)std::llround ((double)time_sec * (double)sr);
		samplecnt_t slop = (samplecnt_t)std::llround ((double)slop_sec * (double)sr);
		if (slop < 1) {
			slop = 1;
		}
		Location* hit = locs->mark_at (timepos_t (sample), timecnt_t ((samplepos_t)slop), Location::IsMark);
		if (!hit) {
			_snap.detail = L"no marker near click";
			return false;
		}
		_impl->session->begin_reversible_command ("remove marker");
		XMLNode& before = locs->get_state ();
		locs->remove (hit);
		XMLNode& after = locs->get_state ();
		_impl->session->add_command (new MementoCommand<Locations> (*locs, &before, &after));
		_impl->session->commit_reversible_command ();
		refresh_locations_snap ();
		refresh_history_flags ();
		_snap.detail = L"marker removed";
		std::fprintf (stderr, "airdour_win_shell: removed marker near %.2fs (count=%d)\n",
		              time_sec, _snap.marker_count);
		std::fflush (stderr);
		return true;
	} catch (...) {
		_snap.detail = L"marker remove failed";
		return false;
	}
}

bool
EngineHost::locate_to (float time_sec, bool snap_to_marker, float slop_sec)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	try {
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0 && _impl->engine) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
		if (sr <= 0) {
			return false;
		}
		time_sec = snap_time (time_sec);
		if (time_sec < 0.f) {
			time_sec = 0.f;
		}
		samplepos_t sample = (samplepos_t)std::llround ((double)time_sec * (double)sr);
		if (snap_to_marker) {
			Locations* locs = _impl->session->locations ();
			if (locs) {
				samplecnt_t slop = (samplecnt_t)std::llround ((double)slop_sec * (double)sr);
				if (slop < 1) {
					slop = 1;
				}
				Location* hit = locs->mark_at (timepos_t (sample), timecnt_t ((samplepos_t)slop), Location::IsMark);
				if (hit) {
					sample = hit->start_sample ();
					time_sec = (float)sample / (float)sr;
				}
			}
		}
		_impl->session->request_locate (sample, true, MustStop, TRS_UI);
		_snap.transport_sec = time_sec;
		_snap.transport_speed = 0.f;
		refresh_clock_snap ();
		_snap.detail = _snap.snap_enabled ? L"located (grid snap)"
		              : (snap_to_marker ? L"located (snap mark)" : L"located");
		return true;
	} catch (...) {
		_snap.detail = L"locate failed";
		return false;
	}
}

bool
EngineHost::scrub_to (float time_sec)
{
	/* Drag scrub: continuous locate, no mark snap (grid snap still applies). */
	return locate_to (time_sec, false, 0.f);
}

void
EngineHost::refresh_clock_snap ()
{
	_snap.bbt_bars = 1;
	_snap.bbt_beats = 1;
	_snap.bbt_ticks = 0;
	_snap.tc_hours = 0;
	_snap.tc_minutes = 0;
	_snap.tc_seconds = 0;
	_snap.tc_frames = 0;
	if (!_impl || !_impl->session) {
		return;
	}
	try {
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0 && _impl->engine) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
		samplepos_t sample = _impl->session->transport_sample ();
		if (sr > 0 && _snap.transport_sec > 0.f) {
			/* Prefer snap transport_sec when freshly located (event may lag). */
			samplepos_t from_sec = (samplepos_t)std::llround ((double)_snap.transport_sec * (double)sr);
			if (std::llabs ((long long)from_sec - (long long)sample) > (long long)(sr / 50)) {
				sample = from_sec;
			}
		}
		_snap.transport_speed = (float)_impl->session->actual_speed ();

		Temporal::TempoMap::SharedPtr tm = Temporal::TempoMap::fetch ();
		if (tm) {
			Temporal::BBT_Argument bbt = tm->bbt_at (timepos_t (sample));
			_snap.bbt_bars = (int)bbt.bars;
			_snap.bbt_beats = (int)bbt.beats;
			_snap.bbt_ticks = (int)bbt.ticks;
		}

		Timecode::Time tc;
		_impl->session->timecode_time (sample, tc);
		_snap.tc_hours = (int)tc.hours;
		_snap.tc_minutes = (int)tc.minutes;
		_snap.tc_seconds = (int)tc.seconds;
		_snap.tc_frames = (int)tc.frames;
	} catch (...) {
	}
}

bool
EngineHost::set_transport_speed (double speed)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	try {
		if (speed > 8.0) {
			speed = 8.0;
		}
		if (speed < -8.0) {
			speed = -8.0;
		}
		if (std::fabs (speed) < 1.0e-4) {
			/* Do not request_transport_speed(0) — TransportFSM asserts speed≠0
			 * on SetTransportSpeed; stop via request_stop like transport_stop. */
			_impl->session->request_stop (false, false, TRS_UI);
			refresh_clock_snap ();
			/* Request is async — keep UI snap at the commanded speed. */
			_snap.transport_speed = 0.f;
			_snap.transport_rolling = false;
			_snap.detail = L"shuttle stop";
			return true;
		}
		_impl->session->request_transport_speed (speed, TRS_UI);
		_impl->session->request_roll (TRS_UI);
		refresh_clock_snap ();
		_snap.transport_speed = (float)speed;
		_snap.transport_rolling = true;
		_snap.detail = L"shuttle";
		return true;
	} catch (...) {
		_snap.detail = L"shuttle failed";
		return false;
	}
}

bool
EngineHost::stop_shuttle ()
{
	return set_transport_speed (0.0);
}

bool
EngineHost::smoke_locate ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-locate failed — no Session";
		return false;
	}
	try {
		/* Start from a known origin. */
		if (!locate_to (0.f, false, 0.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-locate FAILED (locate 0)\n");
			std::fflush (stderr);
			return false;
		}
		if (!locate_to (1.0f, false, 0.f)
		    || std::fabs (_snap.transport_sec - 1.0f) > 0.08f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-locate FAILED (locate 1.0 → %.3f)\n",
			              _snap.transport_sec);
			std::fflush (stderr);
			return false;
		}
		if (!scrub_to (2.5f) || std::fabs (_snap.transport_sec - 2.5f) > 0.08f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-locate FAILED (scrub 2.5 → %.3f)\n",
			              _snap.transport_sec);
			std::fflush (stderr);
			return false;
		}
		refresh_clock_snap ();
		if (_snap.bbt_bars < 1 || _snap.bbt_beats < 1) {
			std::fprintf (stderr, "airdour_win_shell: smoke-locate FAILED (BBT %d|%d|%d)\n",
			              _snap.bbt_bars, _snap.bbt_beats, _snap.bbt_ticks);
			std::fflush (stderr);
			return false;
		}
		/* Shuttle forward briefly then stop — must not leave rolling. */
		if (!set_transport_speed (2.0) || std::fabs (_snap.transport_speed - 2.f) > 0.05f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-locate FAILED (shuttle +2 → %.2f)\n",
			              _snap.transport_speed);
			std::fflush (stderr);
			return false;
		}
		if (!stop_shuttle () || std::fabs (_snap.transport_speed) > 0.05f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-locate FAILED (shuttle stop)\n");
			std::fflush (stderr);
			return false;
		}
		/* Re-anchor after shuttle so --timeout stays quiet. */
		locate_to (1.0f, false, 0.f);
		stop_shuttle ();
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-locate ok (locate/scrub + BBT %d|%d|%d TC %02d:%02d:%02d:%02d)\n",
		              _snap.bbt_bars, _snap.bbt_beats, _snap.bbt_ticks,
		              _snap.tc_hours, _snap.tc_minutes, _snap.tc_seconds, _snap.tc_frames);
		std::fflush (stderr);
		_snap.detail = L"smoke-locate ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-locate exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::session_extent_sec (float* start_sec, float* end_sec) const
{
	if (!start_sec || !end_sec) {
		return false;
	}
	*start_sec = 0.f;
	*end_sec = 0.f;
	int64_t s = 0;
	int64_t e = 0;
	if (!compute_export_range (&s, &e) || e <= s) {
		return false;
	}
	samplecnt_t sr = 48000;
	if (_impl && _impl->session) {
		sr = _impl->session->sample_rate ();
		if (sr <= 0 && _impl->engine) {
			sr = (samplecnt_t)_impl->engine->sample_rate ();
		}
	}
	if (sr <= 0) {
		sr = 48000;
	}
	*start_sec = (float)s / (float)sr;
	*end_sec = (float)e / (float)sr;
	return *end_sec > *start_sec;
}

bool
EngineHost::regions_extent_sec (float* start_sec, float* end_sec) const
{
	if (!start_sec || !end_sec || !_impl || !_impl->session) {
		return false;
	}
	*start_sec = 0.f;
	*end_sec = 0.f;
	samplecnt_t sr = _impl->session->sample_rate ();
	if (sr <= 0 && _impl->engine) {
		sr = (samplecnt_t)_impl->engine->sample_rate ();
	}
	if (sr <= 0) {
		return false;
	}

	samplepos_t min_s = std::numeric_limits<samplepos_t>::max ();
	samplepos_t max_e = 0;
	bool any = false;
	try {
		for (size_t t = 0; t < _impl->tracks.size (); ++t) {
			if (!_impl->tracks[t] || !_impl->tracks[t]->playlist ()) {
				continue;
			}
			std::shared_ptr<RegionList> regions = _impl->tracks[t]->playlist ()->region_list ();
			if (!regions) {
				continue;
			}
			for (RegionList::const_iterator it = regions->begin (); it != regions->end (); ++it) {
				if (!*it) {
					continue;
				}
				samplepos_t rs = (*it)->position ().samples ();
				samplepos_t re = (*it)->end_position ().samples ();
				if (re <= rs) {
					continue;
				}
				if (rs < min_s) {
					min_s = rs;
				}
				if (re > max_e) {
					max_e = re;
				}
				any = true;
			}
		}
		if (_impl->midi_track && _impl->midi_track->playlist ()) {
			std::shared_ptr<RegionList> regions = _impl->midi_track->playlist ()->region_list ();
			if (regions) {
				for (RegionList::const_iterator it = regions->begin (); it != regions->end (); ++it) {
					if (!*it) {
						continue;
					}
					samplepos_t rs = (*it)->position ().samples ();
					samplepos_t re = (*it)->end_position ().samples ();
					if (re <= rs) {
						continue;
					}
					if (rs < min_s) {
						min_s = rs;
					}
					if (re > max_e) {
						max_e = re;
					}
					any = true;
				}
			}
		}
	} catch (...) {
		return false;
	}
	if (!any || max_e <= min_s) {
		return false;
	}
	*start_sec = (float)min_s / (float)sr;
	*end_sec = (float)max_e / (float)sr;
	return true;
}

bool
EngineHost::smoke_zoom ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-zoom failed — no Session";
		return false;
	}
	try {
		if (!_snap.track_has_regions[0]) {
			if (!import_test_tone (0, 220.f, 2.f) || !_snap.track_has_regions[0]) {
				std::fprintf (stderr, "airdour_win_shell: smoke-zoom FAILED (import tone)\n");
				std::fflush (stderr);
				return false;
			}
		}
		float s0 = 0.f, e0 = 0.f;
		if (!session_extent_sec (&s0, &e0) || (e0 - s0) < 0.5f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-zoom FAILED (session extent %.2f..%.2f)\n",
			              s0, e0);
			std::fflush (stderr);
			return false;
		}
		float s1 = 0.f, e1 = 0.f;
		if (!regions_extent_sec (&s1, &e1) || (e1 - s1) < 0.5f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-zoom FAILED (regions extent %.2f..%.2f)\n",
			              s1, e1);
			std::fflush (stderr);
			return false;
		}
		/* Select the tone region so shell can fit-selection. */
		if (!select_region_at (0, s1 + 0.1f) || !_snap.has_selection) {
			std::fprintf (stderr, "airdour_win_shell: smoke-zoom FAILED (select region)\n");
			std::fflush (stderr);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-zoom ok (session %.2f..%.2f regions %.2f..%.2f sel %.2f..%.2f)\n",
		              s0, e0, s1, e1, _snap.selection_start_sec, _snap.selection_end_sec);
		std::fflush (stderr);
		_snap.detail = L"smoke-zoom ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-zoom exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_loop_markers ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-loop failed — no Session";
		return false;
	}
	try {
		if (!set_loop_range (0.5f, 2.5f) || !_snap.loop_active) {
			std::fprintf (stderr, "airdour_win_shell: smoke-loop FAILED (set range)\n");
			std::fflush (stderr);
			return false;
		}
		if (std::fabs (_snap.loop_start_sec - 0.5f) > 0.05f
		    || std::fabs (_snap.loop_end_sec - 2.5f) > 0.05f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-loop FAILED (range mismatch %.2f..%.2f)\n",
			              _snap.loop_start_sec, _snap.loop_end_sec);
			std::fflush (stderr);
			return false;
		}
		if (!add_marker_at (1.0f, "smokeA") || !add_marker_at (2.0f, "smokeB")) {
			std::fprintf (stderr, "airdour_win_shell: smoke-loop FAILED (add markers)\n");
			std::fflush (stderr);
			return false;
		}
		if (_snap.marker_count < 2) {
			std::fprintf (stderr, "airdour_win_shell: smoke-loop FAILED (marker_count=%d)\n",
			              _snap.marker_count);
			std::fflush (stderr);
			return false;
		}
		if (!remove_marker_near (2.0f, 0.4f) || _snap.marker_count < 1) {
			std::fprintf (stderr, "airdour_win_shell: smoke-loop FAILED (remove)\n");
			std::fflush (stderr);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop ok (loop [%.2f..%.2f] markers=%d)\n",
		              _snap.loop_start_sec, _snap.loop_end_sec, _snap.marker_count);
		std::fflush (stderr);
		_snap.detail = L"smoke-loop ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-loop exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::configure_backend_midi ()
{
	if (!_impl || !_impl->engine) {
		return false;
	}
	std::shared_ptr<AudioBackend> be = _impl->engine->current_backend ();
	if (!be) {
		return false;
	}
	try {
		std::vector<std::string> opts = be->enumerate_midi_options ();
		std::string chosen;
		/* Prefer real WinMME; Dummy uses loopback/silence ports. Never PortAudio. */
		for (size_t i = 0; i < opts.size (); ++i) {
			if (opts[i] == "WinMME") {
				chosen = opts[i];
				break;
			}
		}
		if (chosen.empty ()) {
			for (size_t i = 0; i < opts.size (); ++i) {
				if (opts[i].find ("Loopback") != std::string::npos) {
					chosen = opts[i];
					break;
				}
			}
		}
		if (chosen.empty () && !opts.empty ()) {
			/* Prefer any option that is not "None" / "No MIDI". */
			for (size_t i = 0; i < opts.size (); ++i) {
				if (opts[i].find ("No MIDI") == std::string::npos
				    && opts[i] != "None") {
					chosen = opts[i];
					break;
				}
			}
		}
		if (chosen.empty () && !opts.empty ()) {
			chosen = opts[0];
		}
		if (!chosen.empty ()) {
			be->set_midi_option (chosen);
			_snap.midi_option = utf8_to_wide (chosen);
			std::fprintf (stderr, "airdour_win_shell: MIDI option → %s\n", chosen.c_str ());
			std::fflush (stderr);
			return true;
		}
	} catch (...) {
	}
	return false;
}

namespace {

/* Bundled ACE Reasonable Synth (libs/plugins/reasonablesynth.lv2). */
static const char* kReasonableSynthUri = "https://community.ardour.org/node/7596";
/* Bundled ACE Reverb (libs/plugins/a-reverb.lv2) — audio-track FX, no cairo. */
static const char* kAceReverbUri = "urn:ardour:a-reverb";
/* LV2 control port index for Blend (wet mix); see a-reverb.ttl.in. */
static const uint32_t kAceReverbBlendPort = 4;
/* Bundled ACE Compressor (libs/plugins/a-comp.lv2) — DSP-only, no cairo. */
static const char* kAceCompUri = "urn:ardour:a-comp";
static const uint32_t kAceCompRatioPort = 3;
static const uint32_t kAceCompThresholdPort = 4;
static const uint32_t kAceCompSidechainPort = 9;
/* Bundled ACE EQ (libs/plugins/a-eq.lv2) — DSP-only, no cairo. */
static const char* kAceEqUri = "urn:ardour:a-eq";
static const uint32_t kAceEqGainLPort = 1; /* Gain L */
static const uint32_t kAceEqGain2Port = 6; /* Gain 2 (mid) */
static const uint32_t kAceEqGainHPort = 15; /* Gain H */
/* Bundled ACE Delay (libs/plugins/a-delay.lv2) — DSP-only, no cairo. */
static const char* kAceDelayUri = "urn:ardour:a-delay";
static const uint32_t kAceDelayTimePort = 5; /* Time ms */
static const uint32_t kAceDelayFeedbackPort = 8; /* Feedback % */
/* Bundled ACE Limiter (libs/plugins/a-limiter.lv2) — DSP-only. */
static const char* kAceLimiterUri = "urn:ardour:a-limiter";
static const uint32_t kAceLimiterCeilingPort = 0; /* Ceiling dB */
static const uint32_t kAceLimiterReleasePort = 1; /* Release ms */
/* Bundled ACE Gate (libs/plugins/a-gate.lv2) — DSP-only. */
static const char* kAceGateUri = "urn:ardour:a-gate";
static const uint32_t kAceGateThresholdPort = 0; /* Threshold dB */
static const uint32_t kAceGateHoldPort = 1; /* Hold ms */

static float
comp_threshold_to_norm (float db)
{
	db = std::max (-60.f, std::min (0.f, db));
	return (db - (-60.f)) / 60.f;
}

static float
comp_norm_to_threshold (float n)
{
	n = std::max (0.f, std::min (1.f, n));
	return -60.f + n * 60.f;
}

static float
comp_ratio_to_norm (float ratio)
{
	ratio = std::max (1.f, std::min (20.f, ratio));
	return (ratio - 1.f) / 19.f;
}

static float
comp_norm_to_ratio (float n)
{
	n = std::max (0.f, std::min (1.f, n));
	return 1.f + n * 19.f;
}

static float
eq_gain_to_norm (float db)
{
	db = std::max (-20.f, std::min (20.f, db));
	return (db - (-20.f)) / 40.f;
}

static float
eq_norm_to_gain (float n)
{
	n = std::max (0.f, std::min (1.f, n));
	return -20.f + n * 40.f;
}

/* Time port is logarithmic (1..8000 ms). */
static float
delay_time_to_norm (float ms)
{
	ms = std::max (1.f, std::min (8000.f, ms));
	return std::log (ms) / std::log (8000.f);
}

static float
delay_norm_to_time (float n)
{
	n = std::max (0.f, std::min (1.f, n));
	return std::pow (8000.f, n);
}

static float
delay_feedback_to_norm (float pct)
{
	pct = std::max (0.f, std::min (100.f, pct));
	return pct / 100.f;
}

static float
delay_norm_to_feedback (float n)
{
	n = std::max (0.f, std::min (1.f, n));
	return n * 100.f;
}

static float
limiter_ceiling_to_norm (float db)
{
	db = std::max (-20.f, std::min (0.f, db));
	return (db - (-20.f)) / 20.f;
}

static float
limiter_norm_to_ceiling (float n)
{
	n = std::max (0.f, std::min (1.f, n));
	return -20.f + n * 20.f;
}

static float
limiter_release_to_norm (float ms)
{
	ms = std::max (1.f, std::min (2000.f, ms));
	return (ms - 1.f) / 1999.f;
}

static float
limiter_norm_to_release (float n)
{
	n = std::max (0.f, std::min (1.f, n));
	return 1.f + n * 1999.f;
}

static float
gate_threshold_to_norm (float db)
{
	db = std::max (-60.f, std::min (0.f, db));
	return (db - (-60.f)) / 60.f;
}

static float
gate_norm_to_threshold (float n)
{
	n = std::max (0.f, std::min (1.f, n));
	return -60.f + n * 60.f;
}

static float
gate_hold_to_norm (float ms)
{
	ms = std::max (1.f, std::min (500.f, ms));
	return (ms - 1.f) / 499.f;
}

static float
gate_norm_to_hold (float n)
{
	n = std::max (0.f, std::min (1.f, n));
	return 1.f + n * 499.f;
}

} // namespace

bool
EngineHost::ensure_plugins_scanned ()
{
	if (!_impl) {
		return false;
	}
	if (_impl->plugins_scanned) {
		return true;
	}
	try {
		/* Cache-only first (fast LV2 + any prior VST3 cache). */
		PluginManager::instance ().refresh (true);
		_impl->plugins_scanned = true;
		std::fprintf (stderr,
		              "airdour_win_shell: PluginManager refresh done (LV2=%zu VST3=%zu)\n",
		              PluginManager::instance ().lv2_plugin_info ().size (),
		              PluginManager::instance ().vst3_plugin_info ().size ());
		std::fflush (stderr);
		return true;
	} catch (...) {
		std::fprintf (stderr, "airdour_win_shell: PluginManager refresh failed\n");
		std::fflush (stderr);
		return false;
	}
}

/** Apply VST3_PATH / AIRDOUR_SMOKE_VST3 then full (non-cache) plugin refresh. */
static bool
scan_vst3_fresh ()
{
	try {
		const char* smoke = std::getenv ("AIRDOUR_SMOKE_VST3");
		const char* path = std::getenv ("VST3_PATH");
		std::string extra;
		if (path && *path) {
			extra = path;
		}
		if (smoke && *smoke) {
			/* File → parent dir; dir used as-is. */
			if (Glib::file_test (smoke, Glib::FILE_TEST_IS_REGULAR)) {
				std::string dir = Glib::path_get_dirname (smoke);
				if (!extra.empty ()) {
					extra += G_SEARCHPATH_SEPARATOR_S;
				}
				extra += dir;
			} else if (Glib::file_test (smoke, Glib::FILE_TEST_IS_DIR)) {
				if (!extra.empty ()) {
					extra += G_SEARCHPATH_SEPARATOR_S;
				}
				extra += smoke;
			}
		}
		if (!extra.empty ()) {
			Config->set_plugin_path_vst3 (extra);
			std::fprintf (stderr, "airdour_win_shell: VST3 search path += %s\n", extra.c_str ());
			std::fflush (stderr);
		}
		Config->set_use_vst3 (true);
		/* Non-cache refresh so unscanned modules are discovered (in-host if
		 * ardour-vst3-scanner.exe is absent). */
		PluginManager::instance ().refresh (false);
		const PluginInfoList& v3 = PluginManager::instance ().vst3_plugin_info ();
		std::fprintf (stderr, "airdour_win_shell: VST3 scan done (count=%zu)\n", v3.size ());
		size_t shown = 0;
		for (PluginInfoList::const_iterator i = v3.begin (); i != v3.end () && shown < 12; ++i, ++shown) {
			if (!*i) {
				continue;
			}
			std::fprintf (stderr, "airdour_win_shell:   VST3[%zu] %s (%s)\n",
			              shown, (*i)->name.c_str (), (*i)->creator.c_str ());
		}
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: VST3 scan exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		std::fprintf (stderr, "airdour_win_shell: VST3 scan failed\n");
		std::fflush (stderr);
		return false;
	}
}

static PluginInfoPtr
find_smoke_vst3_info ()
{
	const char* want = std::getenv ("AIRDOUR_SMOKE_VST3");
	std::string want_name;
	if (want && *want) {
		if (Glib::file_test (want, Glib::FILE_TEST_IS_REGULAR)
		    || Glib::file_test (want, Glib::FILE_TEST_IS_DIR)) {
			want_name = Glib::path_get_basename (want);
			/* Strip .vst3 */
			std::string::size_type dot = want_name.rfind (".vst3");
			if (dot != std::string::npos) {
				want_name = want_name.substr (0, dot);
			}
		} else {
			want_name = want; /* treat as display-name substring */
		}
	}

	try {
		const PluginInfoList& plugs = PluginManager::instance ().vst3_plugin_info ();
		PluginInfoPtr first_fx;
		for (PluginInfoList::const_iterator i = plugs.begin (); i != plugs.end (); ++i) {
			if (!*i) {
				continue;
			}
			PluginInfoPtr info = *i;
			if (info->is_instrument ()) {
				continue;
			}
			if (info->n_inputs.n_audio () < 1 || info->n_outputs.n_audio () < 1) {
				continue;
			}
			if (!want_name.empty ()) {
				if (info->name.find (want_name) != std::string::npos
				    || info->path.find (want_name) != std::string::npos) {
					return info;
				}
				continue;
			}
			if (!first_fx) {
				first_fx = info;
			}
		}
		return first_fx;
	} catch (...) {
	}
	return PluginInfoPtr ();
}

static PluginInfoPtr
find_reasonable_synth_info ()
{
	try {
		const PluginInfoList& plugs = PluginManager::instance ().lv2_plugin_info ();
		for (PluginInfoList::const_iterator i = plugs.begin (); i != plugs.end (); ++i) {
			if (!*i) {
				continue;
			}
			if ((*i)->unique_id == kReasonableSynthUri
			    || (*i)->name.find ("Reasonable Synth") != std::string::npos) {
				return *i;
			}
		}
	} catch (...) {
	}
	return PluginInfoPtr ();
}

static PluginInfoPtr
find_ace_reverb_info ()
{
	try {
		const PluginInfoList& plugs = PluginManager::instance ().lv2_plugin_info ();
		for (PluginInfoList::const_iterator i = plugs.begin (); i != plugs.end (); ++i) {
			if (!*i) {
				continue;
			}
			if ((*i)->unique_id == kAceReverbUri
			    || (*i)->name.find ("ACE Reverb") != std::string::npos
			    || (*i)->name.find ("a-reverb") != std::string::npos) {
				return *i;
			}
		}
	} catch (...) {
	}
	return PluginInfoPtr ();
}

static PluginInfoPtr
find_ace_comp_info ()
{
	try {
		const PluginInfoList& plugs = PluginManager::instance ().lv2_plugin_info ();
		for (PluginInfoList::const_iterator i = plugs.begin (); i != plugs.end (); ++i) {
			if (!*i) {
				continue;
			}
			if ((*i)->unique_id == kAceCompUri
			    || (*i)->name.find ("ACE Compressor") != std::string::npos
			    || (*i)->name.find ("a-comp") != std::string::npos) {
				/* Prefer mono URI over stereo (#stereo). */
				if ((*i)->unique_id.find ("#stereo") != std::string::npos) {
					continue;
				}
				return *i;
			}
		}
		/* Fallback: any ACE Compressor including stereo. */
		for (PluginInfoList::const_iterator i = plugs.begin (); i != plugs.end (); ++i) {
			if (*i && (*i)->name.find ("ACE Compressor") != std::string::npos) {
				return *i;
			}
		}
	} catch (...) {
	}
	return PluginInfoPtr ();
}

static PluginInfoPtr
find_ace_eq_info ()
{
	try {
		const PluginInfoList& plugs = PluginManager::instance ().lv2_plugin_info ();
		for (PluginInfoList::const_iterator i = plugs.begin (); i != plugs.end (); ++i) {
			if (!*i) {
				continue;
			}
			if ((*i)->unique_id == kAceEqUri
			    || (*i)->name.find ("ACE EQ") != std::string::npos
			    || (*i)->name.find ("a-eq") != std::string::npos) {
				return *i;
			}
		}
	} catch (...) {
	}
	return PluginInfoPtr ();
}

static PluginInfoPtr
find_ace_delay_info ()
{
	try {
		const PluginInfoList& plugs = PluginManager::instance ().lv2_plugin_info ();
		for (PluginInfoList::const_iterator i = plugs.begin (); i != plugs.end (); ++i) {
			if (!*i) {
				continue;
			}
			if ((*i)->unique_id == kAceDelayUri
			    || (*i)->name.find ("ACE Delay") != std::string::npos
			    || (*i)->name.find ("a-delay") != std::string::npos) {
				return *i;
			}
		}
	} catch (...) {
	}
	return PluginInfoPtr ();
}

static PluginInfoPtr
find_ace_limiter_info ()
{
	try {
		const PluginInfoList& plugs = PluginManager::instance ().lv2_plugin_info ();
		for (PluginInfoList::const_iterator i = plugs.begin (); i != plugs.end (); ++i) {
			if (!*i) {
				continue;
			}
			if ((*i)->unique_id == kAceLimiterUri
			    || (*i)->name.find ("ACE Limiter") != std::string::npos
			    || (*i)->name.find ("a-limiter") != std::string::npos) {
				return *i;
			}
		}
	} catch (...) {
	}
	return PluginInfoPtr ();
}

static PluginInfoPtr
find_ace_gate_info ()
{
	try {
		const PluginInfoList& plugs = PluginManager::instance ().lv2_plugin_info ();
		for (PluginInfoList::const_iterator i = plugs.begin (); i != plugs.end (); ++i) {
			if (!*i) {
				continue;
			}
			if ((*i)->unique_id == kAceGateUri
			    || (*i)->name.find ("ACE Gate") != std::string::npos
			    || (*i)->name.find ("a-gate") != std::string::npos) {
				return *i;
			}
		}
	} catch (...) {
	}
	return PluginInfoPtr ();
}

static std::shared_ptr<PluginInsert>
find_ace_reverb_insert (std::shared_ptr<Route> route, std::string* out_name = nullptr)
{
	std::shared_ptr<PluginInsert> found;
	if (!route) {
		return found;
	}
	route->foreach_processor ([&] (std::weak_ptr<Processor> wp) {
		if (found) {
			return;
		}
		std::shared_ptr<Processor> p = wp.lock ();
		std::shared_ptr<PluginInsert> pi = std::dynamic_pointer_cast<PluginInsert> (p);
		if (!pi || !pi->plugin ()) {
			return;
		}
		std::string uid = pi->plugin ()->unique_id ();
		std::string pname = pi->name ();
		if (uid == kAceReverbUri
		    || pname.find ("ACE Reverb") != std::string::npos
		    || pname.find ("a-reverb") != std::string::npos) {
			found = pi;
			if (out_name) {
				*out_name = pname.empty () ? "ACE Reverb" : pname;
			}
		}
	});
	return found;
}

static bool
track_has_ace_reverb (std::shared_ptr<Route> route, std::string* out_name)
{
	return (bool)find_ace_reverb_insert (route, out_name);
}

static std::shared_ptr<PluginInsert>
find_ace_comp_insert (std::shared_ptr<Route> route, std::string* out_name = nullptr)
{
	std::shared_ptr<PluginInsert> found;
	if (!route) {
		return found;
	}
	route->foreach_processor ([&] (std::weak_ptr<Processor> wp) {
		if (found) {
			return;
		}
		std::shared_ptr<Processor> p = wp.lock ();
		std::shared_ptr<PluginInsert> pi = std::dynamic_pointer_cast<PluginInsert> (p);
		if (!pi || !pi->plugin ()) {
			return;
		}
		std::string uid = pi->plugin ()->unique_id ();
		std::string pname = pi->name ();
		if (uid.find (kAceCompUri) == 0
		    || pname.find ("ACE Compressor") != std::string::npos
		    || pname.find ("a-comp") != std::string::npos) {
			found = pi;
			if (out_name) {
				*out_name = pname.empty () ? "ACE Compressor" : pname;
			}
		}
	});
	return found;
}

static std::shared_ptr<PluginInsert>
find_ace_eq_insert (std::shared_ptr<Route> route, std::string* out_name = nullptr)
{
	std::shared_ptr<PluginInsert> found;
	if (!route) {
		return found;
	}
	route->foreach_processor ([&] (std::weak_ptr<Processor> wp) {
		if (found) {
			return;
		}
		std::shared_ptr<Processor> p = wp.lock ();
		std::shared_ptr<PluginInsert> pi = std::dynamic_pointer_cast<PluginInsert> (p);
		if (!pi || !pi->plugin ()) {
			return;
		}
		std::string uid = pi->plugin ()->unique_id ();
		std::string pname = pi->name ();
		if (uid.find (kAceEqUri) == 0
		    || pname.find ("ACE EQ") != std::string::npos
		    || pname.find ("a-eq") != std::string::npos) {
			found = pi;
			if (out_name) {
				*out_name = pname.empty () ? "ACE EQ" : pname;
			}
		}
	});
	return found;
}

static std::shared_ptr<PluginInsert>
find_ace_delay_insert (std::shared_ptr<Route> route, std::string* out_name = nullptr)
{
	std::shared_ptr<PluginInsert> found;
	if (!route) {
		return found;
	}
	route->foreach_processor ([&] (std::weak_ptr<Processor> wp) {
		if (found) {
			return;
		}
		std::shared_ptr<Processor> p = wp.lock ();
		std::shared_ptr<PluginInsert> pi = std::dynamic_pointer_cast<PluginInsert> (p);
		if (!pi || !pi->plugin ()) {
			return;
		}
		std::string uid = pi->plugin ()->unique_id ();
		std::string pname = pi->name ();
		if (uid.find (kAceDelayUri) == 0
		    || pname.find ("ACE Delay") != std::string::npos
		    || pname.find ("a-delay") != std::string::npos) {
			found = pi;
			if (out_name) {
				*out_name = pname.empty () ? "ACE Delay" : pname;
			}
		}
	});
	return found;
}

static std::shared_ptr<PluginInsert>
find_ace_limiter_insert (std::shared_ptr<Route> route, std::string* out_name = nullptr)
{
	std::shared_ptr<PluginInsert> found;
	if (!route) {
		return found;
	}
	route->foreach_processor ([&] (std::weak_ptr<Processor> wp) {
		if (found) {
			return;
		}
		std::shared_ptr<Processor> p = wp.lock ();
		std::shared_ptr<PluginInsert> pi = std::dynamic_pointer_cast<PluginInsert> (p);
		if (!pi || !pi->plugin ()) {
			return;
		}
		std::string uid = pi->plugin ()->unique_id ();
		std::string pname = pi->name ();
		if (uid.find (kAceLimiterUri) == 0
		    || pname.find ("ACE Limiter") != std::string::npos
		    || pname.find ("a-limiter") != std::string::npos) {
			found = pi;
			if (out_name) {
				*out_name = pname.empty () ? "ACE Limiter" : pname;
			}
		}
	});
	return found;
}

static std::shared_ptr<PluginInsert>
find_ace_gate_insert (std::shared_ptr<Route> route, std::string* out_name = nullptr)
{
	std::shared_ptr<PluginInsert> found;
	if (!route) {
		return found;
	}
	route->foreach_processor ([&] (std::weak_ptr<Processor> wp) {
		if (found) {
			return;
		}
		std::shared_ptr<Processor> p = wp.lock ();
		std::shared_ptr<PluginInsert> pi = std::dynamic_pointer_cast<PluginInsert> (p);
		if (!pi || !pi->plugin ()) {
			return;
		}
		std::string uid = pi->plugin ()->unique_id ();
		std::string pname = pi->name ();
		if (uid.find (kAceGateUri) == 0
		    || pname.find ("ACE Gate") != std::string::npos
		    || pname.find ("a-gate") != std::string::npos) {
			found = pi;
			if (out_name) {
				*out_name = pname.empty () ? "ACE Gate" : pname;
			}
		}
	});
	return found;
}

void
EngineHost::refresh_midi_notes_snap ()
{
	_snap.midi_note_count = 0;
	_snap.midi_region_ok = false;
	_snap.midi_synth_ok = _impl && _impl->midi_synth_ok;
	_snap.midi_synth_to_master = _impl && _impl->midi_synth_to_master;
	_snap.midi_synth_name = (_impl && !_impl->midi_synth_name.empty ())
	                            ? utf8_to_wide (_impl->midi_synth_name)
	                            : std::wstring ();
	_snap.midi_selected_note_id = _impl ? _impl->selected_midi_note_id : -1;
	_snap.midi_out_connected = _impl && _impl->midi_out_connected;
	_snap.midi_out_name = (_impl && !_impl->midi_out_port.empty ())
	                          ? utf8_to_wide (_impl->midi_out_port)
	                          : std::wstring ();
	for (int i = 0; i < EngineSnapshot::kMaxMidiNotes; ++i) {
		_snap.midi_notes[i].id = -1;
		_snap.midi_notes[i].start_sec = 0.f;
		_snap.midi_notes[i].length_sec = 0.f;
		_snap.midi_notes[i].pitch = 0;
		_snap.midi_notes[i].velocity = 0;
		_snap.midi_notes[i].selected = false;
	}
	if (!_impl || !_impl->midi_region || !_impl->engine) {
		return;
	}
	try {
		std::shared_ptr<MidiModel> model = _impl->midi_region->model ();
		if (!model) {
			return;
		}
		_snap.midi_region_ok = true;
		/* Map beats→seconds at a fixed 120 BPM to avoid TempoMap locks while
		 * the process thread may be reading the same MidiModel. */
		const double sec_per_beat = 60.0 / 120.0;
		const MidiModel::Notes& notes = model->notes ();
		int n = 0;
		for (MidiModel::Notes::const_iterator it = notes.begin ();
		     it != notes.end () && n < EngineSnapshot::kMaxMidiNotes; ++it) {
			const std::shared_ptr<Evoral::Note<Temporal::Beats> >& note = *it;
			if (!note) {
				continue;
			}
			const double t_beats = Temporal::DoubleableBeats (note->time ()).to_double ();
			const double l_beats = Temporal::DoubleableBeats (note->length ()).to_double ();
			float start_sec = (float)(t_beats * sec_per_beat);
			float len = (float)(l_beats * sec_per_beat);
			if (len < 0.01f) {
				len = 0.01f;
			}
			_snap.midi_notes[n].id = note->id ();
			_snap.midi_notes[n].start_sec = start_sec;
			_snap.midi_notes[n].length_sec = len;
			_snap.midi_notes[n].pitch = note->note ();
			_snap.midi_notes[n].velocity = note->velocity ();
			_snap.midi_notes[n].selected =
			    (_impl->selected_midi_note_id >= 0
			     && note->id () == _impl->selected_midi_note_id);
			++n;
		}
		_snap.midi_note_count = n;
	} catch (...) {
	}
}

void
EngineHost::refresh_midi_snap ()
{
	_snap.midi_ok = false;
	_snap.midi_track_ok = (_impl && _impl->midi_track);
	_snap.midi_activity = _impl ? _impl->midi_activity : 0.f;
	_snap.midi_device_count = 0;
	for (int i = 0; i < EngineSnapshot::kMaxMidiDevices; ++i) {
		_snap.midi_devices[i].name[0] = L'\0';
		_snap.midi_devices[i].input = false;
	}
	if (!_impl || !_impl->engine) {
		refresh_midi_notes_snap ();
		return;
	}
	try {
		std::shared_ptr<AudioBackend> be = _impl->engine->current_backend ();
		if (!be) {
			refresh_midi_notes_snap ();
			return;
		}
		_snap.midi_option = utf8_to_wide (be->midi_option ());
		std::vector<AudioBackend::DeviceStatus> devs = be->enumerate_midi_devices ();
		int n = 0;
		for (size_t i = 0; i < devs.size () && n < EngineSnapshot::kMaxMidiDevices; ++i) {
			std::wstring w = utf8_to_wide (devs[i].name);
			wcsncpy_s (_snap.midi_devices[n].name, w.c_str (), _TRUNCATE);
			/* Heuristic: names containing "out" / "synth" → output. */
			std::string lower = devs[i].name;
			for (size_t c = 0; c < lower.size (); ++c) {
				if (lower[c] >= 'A' && lower[c] <= 'Z') {
					lower[c] = (char)(lower[c] - 'A' + 'a');
				}
			}
			_snap.midi_devices[n].input =
			    (lower.find ("out") == std::string::npos
			     && lower.find ("synth") == std::string::npos
			     && lower.find ("playback") == std::string::npos);
			++n;
		}
		_snap.midi_device_count = n;

		/* Physical MIDI ports from backend. */
		std::vector<std::string> phys_in;
		std::vector<std::string> phys_out;
		be->get_physical_inputs (DataType::MIDI, phys_in);
		be->get_physical_outputs (DataType::MIDI, phys_out);
		_snap.midi_ok = (_snap.midi_option.find (L"None") == std::wstring::npos
		                 && _snap.midi_option.find (L"No MIDI") == std::wstring::npos)
		                || !phys_in.empty () || !phys_out.empty () || n > 0;

		/* Activity: if MIDI track PeakMeter has energy, bump; else decay. */
		if (_impl->midi_track) {
			std::shared_ptr<PeakMeter> pm = _impl->midi_track->peak_meter ();
			float peak = peak_linear_from_meter (pm);
			if (peak > _impl->midi_activity) {
				_impl->midi_activity = peak;
			} else {
				_impl->midi_activity *= 0.92f;
				if (_impl->midi_activity < 0.01f) {
					_impl->midi_activity = 0.f;
				}
			}
			_snap.midi_activity = _impl->midi_activity;
		}
		_snap.midi_track_ok = (bool)_impl->midi_track;

		/* Recover midi_region pointer from playlist if needed. */
		if (_impl->midi_track && !_impl->midi_region) {
			std::shared_ptr<Playlist> pl = _impl->midi_track->playlist ();
			if (pl) {
				pl->foreach_region ([&] (std::shared_ptr<Region> r) {
					if (_impl->midi_region) {
						return;
					}
					std::shared_ptr<MidiRegion> mr =
					    std::dynamic_pointer_cast<MidiRegion> (r);
					if (mr) {
						_impl->midi_region = mr;
					}
				});
			}
		}
	} catch (...) {
	}
	refresh_midi_notes_snap ();
}

bool
EngineHost::connect_midi_output ()
{
	if (!_impl || !_impl->engine || !_impl->midi_track) {
		return false;
	}
	if (_impl->midi_out_connected) {
		_snap.midi_out_connected = true;
		_snap.midi_out_name = utf8_to_wide (_impl->midi_out_port);
		return true;
	}
	try {
		std::vector<std::string> outs;
		_impl->engine->get_physical_outputs (DataType::MIDI, outs);
		if (outs.empty ()) {
			_snap.detail = L"no physical MIDI outs (GS/softsynth not found)";
			return false;
		}
		std::shared_ptr<IO> out_io = _impl->midi_track->output ();
		if (!out_io || !out_io->nth (0)) {
			return false;
		}
		/* Prefer a synth/wavetable port when several exist. */
		std::string chosen = outs[0];
		for (size_t i = 0; i < outs.size (); ++i) {
			std::string lower = outs[i];
			for (size_t c = 0; c < lower.size (); ++c) {
				if (lower[c] >= 'A' && lower[c] <= 'Z') {
					lower[c] = (char)(lower[c] - 'A' + 'a');
				}
			}
			if (lower.find ("synth") != std::string::npos
			    || lower.find ("wavetable") != std::string::npos
			    || lower.find ("gs") != std::string::npos) {
				chosen = outs[i];
				break;
			}
		}
		out_io->connect (out_io->nth (0), chosen);
		_impl->midi_out_connected = true;
		_impl->midi_out_port = chosen;
		_snap.midi_out_connected = true;
		_snap.midi_out_name = utf8_to_wide (chosen);
		std::fprintf (stderr, "airdour_win_shell: MIDI out → %s\n", chosen.c_str ());
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: MIDI out connect failed: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::attach_midi_synth ()
{
	if (!_impl || !_impl->session || !_impl->midi_track) {
		return false;
	}
	if (_impl->midi_synth_ok && _impl->midi_track->the_instrument ()) {
		_snap.midi_synth_ok = true;
		_snap.midi_synth_to_master = _impl->midi_synth_to_master;
		_snap.midi_synth_name = utf8_to_wide (_impl->midi_synth_name);
		return true;
	}
	ensure_plugins_scanned ();
	try {
		if (_impl->midi_track->the_instrument ()) {
			_impl->midi_synth_ok = true;
			_impl->midi_synth_name = _impl->midi_track->the_instrument ()->name ();
			_impl->midi_synth_to_master =
			    _impl->midi_track->output ()
			    && _impl->midi_track->output ()->n_ports ().n_audio () > 0;
			refresh_midi_notes_snap ();
			return true;
		}

		PluginInfoPtr info = find_reasonable_synth_info ();
		if (!info) {
			_snap.detail = L"ACE Reasonable Synth not found (check ARDOUR_DLL_PATH/LV2)";
			std::fprintf (stderr, "airdour_win_shell: Reasonable Synth LV2 missing\n");
			std::fflush (stderr);
			return false;
		}

		PluginPtr plugin = info->load (*_impl->session);
		if (!plugin) {
			_snap.detail = L"Reasonable Synth load failed";
			return false;
		}

		std::shared_ptr<PluginInsert> pi (
		    new PluginInsert (*_impl->session, *_impl->midi_track, plugin));
		pi->set_strict_io (true);
		if (_impl->midi_track->add_processor (pi, PreFader) != 0) {
			_snap.detail = L"Reasonable Synth add_processor failed";
			return false;
		}

		/* Ensure stereo audio outs and connect to master when possible. */
		std::shared_ptr<Route> master = _impl->session->master_out ();
		bool to_master = false;
		if (master && _impl->midi_track->output () && master->input ()) {
			ChanCount want = _impl->midi_track->output ()->n_ports ();
			want.set (DataType::AUDIO, 2);
			{
				PBD::Mutex::Lock lm (_impl->engine->process_lock ());
				_impl->midi_track->output ()->ensure_io (want, false);
			}
			uint32_t n_out = _impl->midi_track->output()->n_ports().n_audio();
			uint32_t n_in = master->input()->n_ports().n_audio();
			uint32_t n = std::min (n_out, n_in);
			for (uint32_t i = 0; i < n; ++i) {
				std::shared_ptr<Port> our = _impl->midi_track->output ()->nth (i);
				std::shared_ptr<Port> theirs = master->input ()->nth (i);
				if (our && theirs) {
					_impl->midi_track->output ()->connect (our, theirs->name ());
					to_master = true;
				}
			}
		}

		_impl->midi_synth_ok = true;
		_impl->midi_synth_to_master = to_master;
		_impl->midi_synth_name = info->name.empty () ? "ACE Reasonable Synth" : info->name;
		refresh_midi_notes_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: synth \"%s\" on MIDI track (to_master=%d)\n",
		              _impl->midi_synth_name.c_str (), to_master ? 1 : 0);
		std::fflush (stderr);
		_snap.detail = L"MIDI soft synth ready";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: attach_midi_synth: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

void
EngineHost::refresh_track_fx_snap ()
{
	_snap.track_fx_ok = false;
	_snap.track_fx_bypassed = false;
	_snap.track_fx_param = _impl ? _impl->track_fx_param : 0.3f;
	_snap.track_fx_name.clear ();
	_snap.track_fx_param_name = L"Blend";
	if (!_impl || _impl->tracks.empty () || !_impl->tracks[0]) {
		return;
	}
	std::string name;
	std::shared_ptr<PluginInsert> pi = _impl->track_fx.lock ();
	if (!pi) {
		pi = find_ace_reverb_insert (_impl->tracks[0], &name);
		if (pi) {
			_impl->track_fx = pi;
		}
	} else {
		name = pi->name ().empty () ? "ACE Reverb" : pi->name ();
	}
	if (!pi) {
		_impl->track_fx_ok = false;
		return;
	}
	_impl->track_fx_ok = true;
	_impl->track_fx_name = name.empty () ? "ACE Reverb" : name;
	_impl->track_fx_bypassed = !pi->active ();
	try {
		std::shared_ptr<AutomationControl> ac = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceReverbBlendPort));
		if (ac) {
			_impl->track_fx_param = (float)ac->get_value ();
		}
	} catch (...) {
	}
	_snap.track_fx_ok = true;
	_snap.track_fx_bypassed = _impl->track_fx_bypassed;
	_snap.track_fx_param = _impl->track_fx_param;
	_snap.track_fx_name = utf8_to_wide (_impl->track_fx_name);
	_snap.track_fx_param_name = L"Blend";
}

bool
EngineHost::insert_track_fx (int track)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		_snap.detail = L"insert FX: no audio track";
		return false;
	}
	std::shared_ptr<AudioTrack> at = _impl->tracks[track];
	std::string existing;
	std::shared_ptr<PluginInsert> already = find_ace_reverb_insert (at, &existing);
	if (already) {
		_impl->track_fx = already;
		_impl->track_fx_ok = true;
		_impl->track_fx_name = existing;
		refresh_track_fx_snap ();
		return true;
	}

	ensure_plugins_scanned ();
	try {
		PluginInfoPtr info = find_ace_reverb_info ();
		if (!info) {
			_snap.detail = L"ACE Reverb not found (check ARDOUR_DLL_PATH/LV2)";
			std::fprintf (stderr, "airdour_win_shell: ACE Reverb LV2 missing\n");
			std::fflush (stderr);
			return false;
		}

		PluginPtr plugin = info->load (*_impl->session);
		if (!plugin) {
			_snap.detail = L"ACE Reverb load failed";
			return false;
		}

		std::shared_ptr<PluginInsert> pi (
		    new PluginInsert (*_impl->session, *at, plugin));
		pi->set_strict_io (true);
		if (at->add_processor (pi, PreFader) != 0) {
			_snap.detail = L"ACE Reverb add_processor failed";
			return false;
		}

		_impl->track_fx = pi;
		_impl->track_fx_ok = true;
		_impl->track_fx_bypassed = !pi->active ();
		_impl->track_fx_name = info->name.empty () ? "ACE Reverb" : info->name;
		/* Seed Blend from plugin default if control exists. */
		try {
			std::shared_ptr<AutomationControl> ac = pi->automation_control (
			    Evoral::Parameter (PluginAutomation, 0, kAceReverbBlendPort));
			if (ac) {
				_impl->track_fx_param = (float)ac->get_value ();
			}
		} catch (...) {
		}
		refresh_track_fx_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: FX \"%s\" on Audio %d (pre-fader)\n",
		              _impl->track_fx_name.c_str (), track + 1);
		std::fflush (stderr);
		_snap.detail = L"ACE Reverb on Audio 1";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: insert_track_fx: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_fx_bypassed (int track, bool bypassed)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_fx (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_fx.lock ();
	if (!pi) {
		pi = find_ace_reverb_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_fx = pi;
	}
	try {
		if (bypassed) {
			pi->deactivate ();
		} else {
			pi->activate ();
		}
		_impl->track_fx_bypassed = !pi->active ();
		refresh_track_fx_snap ();
		std::fprintf (stderr, "airdour_win_shell: FX bypass=%d (active=%d)\n",
		              _impl->track_fx_bypassed ? 1 : 0, pi->active () ? 1 : 0);
		std::fflush (stderr);
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_track_fx_bypass (int track)
{
	refresh_track_fx_snap ();
	return set_track_fx_bypassed (track, !_snap.track_fx_bypassed);
}

bool
EngineHost::set_track_fx_param (int track, float value_0_1)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_fx (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_fx.lock ();
	if (!pi) {
		pi = find_ace_reverb_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_fx = pi;
	}
	float v = std::max (0.f, std::min (1.f, value_0_1));
	try {
		std::shared_ptr<AutomationControl> ac = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceReverbBlendPort));
		if (!ac) {
			_snap.detail = L"ACE Reverb Blend control missing";
			return false;
		}
		ac->set_value (v, Controllable::NoGroup);
		_impl->track_fx_param = (float)ac->get_value ();
		refresh_track_fx_snap ();
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: set_track_fx_param: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

float
EngineHost::track_fx_param (int track) const
{
	(void)track;
	return _snap.track_fx_param;
}

void
EngineHost::refresh_track_comp_snap ()
{
	_snap.track_comp_ok = false;
	_snap.track_comp_bypassed = false;
	_snap.track_comp_threshold_db = _impl ? _impl->track_comp_threshold_db : 0.f;
	_snap.track_comp_ratio = _impl ? _impl->track_comp_ratio : 4.f;
	_snap.track_comp_threshold_norm = comp_threshold_to_norm (_snap.track_comp_threshold_db);
	_snap.track_comp_ratio_norm = comp_ratio_to_norm (_snap.track_comp_ratio);
	_snap.track_comp_sidechain = _impl ? _impl->track_comp_sidechain : false;
	_snap.track_comp_sidechain_src = _impl ? _impl->track_comp_sidechain_src : -1;
	_snap.track_comp_name.clear ();
	if (!_impl || _impl->tracks.size () < 2 || !_impl->tracks[1]) {
		return;
	}
	std::string name;
	std::shared_ptr<PluginInsert> pi = _impl->track_comp.lock ();
	if (!pi) {
		pi = find_ace_comp_insert (_impl->tracks[1], &name);
		if (pi) {
			_impl->track_comp = pi;
		}
	} else {
		name = pi->name ().empty () ? "ACE Compressor" : pi->name ();
	}
	if (!pi) {
		_impl->track_comp_ok = false;
		return;
	}
	_impl->track_comp_ok = true;
	_impl->track_comp_name = name.empty () ? "ACE Compressor" : name;
	_impl->track_comp_bypassed = !pi->active ();
	try {
		std::shared_ptr<AutomationControl> thr = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceCompThresholdPort));
		std::shared_ptr<AutomationControl> rat = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceCompRatioPort));
		if (thr) {
			_impl->track_comp_threshold_db = (float)thr->get_value ();
		}
		if (rat) {
			_impl->track_comp_ratio = (float)rat->get_value ();
		}
		std::shared_ptr<AutomationControl> sc = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceCompSidechainPort));
		if (sc) {
			_impl->track_comp_sidechain = sc->get_value () > 0.5
			                              && pi->has_sidechain ()
			                              && pi->sidechain_input ()
			                              && pi->sidechain_input ()->connected ();
		}
	} catch (...) {
	}
	_snap.track_comp_ok = true;
	_snap.track_comp_bypassed = _impl->track_comp_bypassed;
	_snap.track_comp_threshold_db = _impl->track_comp_threshold_db;
	_snap.track_comp_ratio = _impl->track_comp_ratio;
	_snap.track_comp_threshold_norm = comp_threshold_to_norm (_impl->track_comp_threshold_db);
	_snap.track_comp_ratio_norm = comp_ratio_to_norm (_impl->track_comp_ratio);
	_snap.track_comp_sidechain = _impl->track_comp_sidechain;
	_snap.track_comp_sidechain_src = _impl->track_comp_sidechain_src;
	_snap.track_comp_name = utf8_to_wide (_impl->track_comp_name);
}

bool
EngineHost::insert_track_comp (int track)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		_snap.detail = L"insert Comp: no audio track";
		return false;
	}
	std::shared_ptr<AudioTrack> at = _impl->tracks[track];
	std::string existing;
	std::shared_ptr<PluginInsert> already = find_ace_comp_insert (at, &existing);
	if (already) {
		_impl->track_comp = already;
		_impl->track_comp_ok = true;
		_impl->track_comp_name = existing;
		refresh_track_comp_snap ();
		return true;
	}

	ensure_plugins_scanned ();
	try {
		PluginInfoPtr info = find_ace_comp_info ();
		if (!info) {
			_snap.detail = L"ACE Compressor not found (check ARDOUR_DLL_PATH/LV2)";
			std::fprintf (stderr, "airdour_win_shell: ACE Compressor LV2 missing\n");
			std::fflush (stderr);
			return false;
		}

		PluginPtr plugin = info->load (*_impl->session);
		if (!plugin) {
			_snap.detail = L"ACE Compressor load failed";
			return false;
		}

		std::shared_ptr<PluginInsert> pi (
		    new PluginInsert (*_impl->session, *at, plugin));
		pi->set_strict_io (true);
		if (at->add_processor (pi, PreFader) != 0) {
			_snap.detail = L"ACE Compressor add_processor failed";
			return false;
		}

		_impl->track_comp = pi;
		_impl->track_comp_ok = true;
		_impl->track_comp_bypassed = !pi->active ();
		_impl->track_comp_name = info->name.empty () ? "ACE Compressor" : info->name;
		try {
			std::shared_ptr<AutomationControl> thr = pi->automation_control (
			    Evoral::Parameter (PluginAutomation, 0, kAceCompThresholdPort));
			std::shared_ptr<AutomationControl> rat = pi->automation_control (
			    Evoral::Parameter (PluginAutomation, 0, kAceCompRatioPort));
			if (thr) {
				_impl->track_comp_threshold_db = (float)thr->get_value ();
			}
			if (rat) {
				_impl->track_comp_ratio = (float)rat->get_value ();
			}
		} catch (...) {
		}
		refresh_track_comp_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: Comp \"%s\" on Audio %d (pre-fader)\n",
		              _impl->track_comp_name.c_str (), track + 1);
		std::fflush (stderr);
		_snap.detail = L"ACE Compressor on Audio 2";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: insert_track_comp: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_comp_bypassed (int track, bool bypassed)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_comp (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_comp.lock ();
	if (!pi) {
		pi = find_ace_comp_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_comp = pi;
	}
	try {
		if (bypassed) {
			pi->deactivate ();
		} else {
			pi->activate ();
		}
		_impl->track_comp_bypassed = !pi->active ();
		refresh_track_comp_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_track_comp_bypass (int track)
{
	refresh_track_comp_snap ();
	return set_track_comp_bypassed (track, !_snap.track_comp_bypassed);
}

bool
EngineHost::set_track_comp_threshold_db (int track, float db)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_comp (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_comp.lock ();
	if (!pi) {
		pi = find_ace_comp_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_comp = pi;
	}
	db = std::max (-60.f, std::min (0.f, db));
	try {
		std::shared_ptr<AutomationControl> ac = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceCompThresholdPort));
		if (!ac) {
			_snap.detail = L"ACE Comp Threshold missing";
			return false;
		}
		ac->set_value (db, Controllable::NoGroup);
		_impl->track_comp_threshold_db = (float)ac->get_value ();
		refresh_track_comp_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_comp_threshold_norm (int track, float norm_0_1)
{
	return set_track_comp_threshold_db (track, comp_norm_to_threshold (norm_0_1));
}

bool
EngineHost::set_track_comp_ratio (int track, float ratio)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_comp (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_comp.lock ();
	if (!pi) {
		pi = find_ace_comp_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_comp = pi;
	}
	ratio = std::max (1.f, std::min (20.f, ratio));
	try {
		std::shared_ptr<AutomationControl> ac = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceCompRatioPort));
		if (!ac) {
			_snap.detail = L"ACE Comp Ratio missing";
			return false;
		}
		ac->set_value (ratio, Controllable::NoGroup);
		_impl->track_comp_ratio = (float)ac->get_value ();
		refresh_track_comp_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_comp_ratio_norm (int track, float norm_0_1)
{
	return set_track_comp_ratio (track, comp_norm_to_ratio (norm_0_1));
}

bool
EngineHost::set_track_comp_sidechain (int track, int src_track, bool enable)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_comp (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_comp.lock ();
	if (!pi) {
		pi = find_ace_comp_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_comp = pi;
	}
	std::shared_ptr<AudioTrack> at = _impl->tracks[track];
	try {
		if (!enable) {
			std::shared_ptr<AutomationControl> sc = pi->automation_control (
			    Evoral::Parameter (PluginAutomation, 0, kAceCompSidechainPort));
			if (sc) {
				sc->set_value (0.0, Controllable::NoGroup);
			}
			if (pi->has_sidechain ()) {
				at->remove_sidechain (pi);
			}
			_impl->track_comp_sidechain = false;
			_impl->track_comp_sidechain_src = -1;
			refresh_track_comp_snap ();
			std::fprintf (stderr, "airdour_win_shell: Comp SC off (track %d)\n", track);
			std::fflush (stderr);
			return true;
		}

		if (src_track < 0 || src_track >= (int)_impl->tracks.size ()
		    || !_impl->tracks[src_track] || src_track == track) {
			_snap.detail = L"Comp SC — bad source track";
			return false;
		}

		if (!pi->has_sidechain ()) {
			if (!at->add_sidechain (pi)) {
				_snap.detail = L"Comp SC — add_sidechain failed";
				std::fprintf (stderr, "airdour_win_shell: Comp add_sidechain failed\n");
				std::fflush (stderr);
				return false;
			}
		}

		std::shared_ptr<IO> sc_in = pi->sidechain_input ();
		std::shared_ptr<IO> src_out = _impl->tracks[src_track]->output ();
		if (!sc_in || !src_out || !sc_in->nth (0) || !src_out->nth (0)) {
			_snap.detail = L"Comp SC — missing ports";
			return false;
		}
		/* Disconnect existing then wire src audio out → SC in. */
		try {
			sc_in->disconnect ();
		} catch (...) {
		}
		if (sc_in->connect (sc_in->nth (0), src_out->nth (0)->name ()) != 0) {
			_snap.detail = L"Comp SC — connect failed";
			std::fprintf (stderr, "airdour_win_shell: Comp SC connect failed\n");
			std::fflush (stderr);
			return false;
		}

		std::shared_ptr<AutomationControl> sc = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceCompSidechainPort));
		if (sc) {
			sc->set_value (1.0, Controllable::NoGroup);
		}
		_impl->track_comp_sidechain = true;
		_impl->track_comp_sidechain_src = src_track;
		refresh_track_comp_snap ();
		std::fprintf (stderr, "airdour_win_shell: Comp SC Audio %d → Audio %d\n",
		              src_track + 1, track + 1);
		std::fflush (stderr);
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: Comp SC exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"Comp SC exception";
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_comp ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-comp failed — no Session";
		return false;
	}
	try {
		collect_tracks ();
		if (_impl->tracks.size () < 2 || !_impl->tracks[1]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-comp FAILED (need Audio 2)\n");
			std::fflush (stderr);
			return false;
		}
		if (!insert_track_comp (1) || !_snap.track_comp_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-comp FAILED (insert)\n");
			std::fflush (stderr);
			return false;
		}
		std::string verified;
		if (!find_ace_comp_insert (_impl->tracks[1], &verified)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-comp FAILED (not on route)\n");
			std::fflush (stderr);
			return false;
		}

		if (!set_track_comp_bypassed (1, false) || _snap.track_comp_bypassed) {
			std::fprintf (stderr, "airdour_win_shell: smoke-comp FAILED (bypass clear)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_track_comp_threshold_db (1, -18.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-comp FAILED (threshold)\n");
			std::fflush (stderr);
			return false;
		}
		if (_snap.track_comp_threshold_db < -19.f || _snap.track_comp_threshold_db > -17.f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-comp FAILED (thr=%.1f)\n",
			              _snap.track_comp_threshold_db);
			std::fflush (stderr);
			return false;
		}
		if (!set_track_comp_ratio (1, 8.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-comp FAILED (ratio)\n");
			std::fflush (stderr);
			return false;
		}
		if (_snap.track_comp_ratio < 7.5f || _snap.track_comp_ratio > 8.5f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-comp FAILED (ratio=%.2f)\n",
			              _snap.track_comp_ratio);
			std::fflush (stderr);
			return false;
		}

		bool sc_ok = set_track_comp_sidechain (1, 0, true);
		if (sc_ok && !_snap.track_comp_sidechain) {
			sc_ok = false;
		}
		/* Sidechain is best-effort — still pass if threshold/ratio work. */
		if (sc_ok) {
			set_track_comp_sidechain (1, 0, false);
		}

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-comp ok (fx=%s thr=%.1f ratio=%.1f sc=%d)\n",
		              verified.c_str (), _snap.track_comp_threshold_db,
		              _snap.track_comp_ratio, sc_ok ? 1 : 0);
		std::fflush (stderr);
		_snap.detail = L"smoke-comp ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-comp exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

void
EngineHost::refresh_track_eq_snap ()
{
	_snap.track_eq_ok = false;
	_snap.track_eq_bypassed = false;
	_snap.track_eq_gain_l_db = _impl ? _impl->track_eq_gain_l_db : 0.f;
	_snap.track_eq_gain_m_db = _impl ? _impl->track_eq_gain_m_db : 0.f;
	_snap.track_eq_gain_h_db = _impl ? _impl->track_eq_gain_h_db : 0.f;
	_snap.track_eq_gain_l_norm = eq_gain_to_norm (_snap.track_eq_gain_l_db);
	_snap.track_eq_gain_m_norm = eq_gain_to_norm (_snap.track_eq_gain_m_db);
	_snap.track_eq_gain_h_norm = eq_gain_to_norm (_snap.track_eq_gain_h_db);
	_snap.track_eq_name.clear ();
	if (!_impl || _impl->tracks.size () < 3 || !_impl->tracks[2]) {
		return;
	}
	std::string name;
	std::shared_ptr<PluginInsert> pi = _impl->track_eq.lock ();
	if (!pi) {
		pi = find_ace_eq_insert (_impl->tracks[2], &name);
		if (pi) {
			_impl->track_eq = pi;
		}
	} else {
		name = pi->name ().empty () ? "ACE EQ" : pi->name ();
	}
	if (!pi) {
		_impl->track_eq_ok = false;
		return;
	}
	_impl->track_eq_ok = true;
	_impl->track_eq_name = name.empty () ? "ACE EQ" : name;
	_impl->track_eq_bypassed = !pi->active ();
	try {
		std::shared_ptr<AutomationControl> gl = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceEqGainLPort));
		std::shared_ptr<AutomationControl> gm = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceEqGain2Port));
		std::shared_ptr<AutomationControl> gh = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceEqGainHPort));
		if (gl) {
			_impl->track_eq_gain_l_db = (float)gl->get_value ();
		}
		if (gm) {
			_impl->track_eq_gain_m_db = (float)gm->get_value ();
		}
		if (gh) {
			_impl->track_eq_gain_h_db = (float)gh->get_value ();
		}
	} catch (...) {
	}
	_snap.track_eq_ok = true;
	_snap.track_eq_bypassed = _impl->track_eq_bypassed;
	_snap.track_eq_gain_l_db = _impl->track_eq_gain_l_db;
	_snap.track_eq_gain_m_db = _impl->track_eq_gain_m_db;
	_snap.track_eq_gain_h_db = _impl->track_eq_gain_h_db;
	_snap.track_eq_gain_l_norm = eq_gain_to_norm (_impl->track_eq_gain_l_db);
	_snap.track_eq_gain_m_norm = eq_gain_to_norm (_impl->track_eq_gain_m_db);
	_snap.track_eq_gain_h_norm = eq_gain_to_norm (_impl->track_eq_gain_h_db);
	_snap.track_eq_name = utf8_to_wide (_impl->track_eq_name);
}

bool
EngineHost::insert_track_eq (int track)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		_snap.detail = L"insert EQ: no audio track";
		return false;
	}
	std::shared_ptr<AudioTrack> at = _impl->tracks[track];
	std::string existing;
	std::shared_ptr<PluginInsert> already = find_ace_eq_insert (at, &existing);
	if (already) {
		_impl->track_eq = already;
		_impl->track_eq_ok = true;
		_impl->track_eq_name = existing;
		refresh_track_eq_snap ();
		return true;
	}

	ensure_plugins_scanned ();
	try {
		PluginInfoPtr info = find_ace_eq_info ();
		if (!info) {
			_snap.detail = L"ACE EQ not found (check ARDOUR_DLL_PATH/LV2)";
			std::fprintf (stderr, "airdour_win_shell: ACE EQ LV2 missing\n");
			std::fflush (stderr);
			return false;
		}

		PluginPtr plugin = info->load (*_impl->session);
		if (!plugin) {
			_snap.detail = L"ACE EQ load failed";
			return false;
		}

		std::shared_ptr<PluginInsert> pi (
		    new PluginInsert (*_impl->session, *at, plugin));
		pi->set_strict_io (true);
		if (at->add_processor (pi, PreFader) != 0) {
			_snap.detail = L"ACE EQ add_processor failed";
			return false;
		}

		_impl->track_eq = pi;
		_impl->track_eq_ok = true;
		_impl->track_eq_bypassed = !pi->active ();
		_impl->track_eq_name = info->name.empty () ? "ACE EQ" : info->name;
		try {
			std::shared_ptr<AutomationControl> gl = pi->automation_control (
			    Evoral::Parameter (PluginAutomation, 0, kAceEqGainLPort));
			std::shared_ptr<AutomationControl> gm = pi->automation_control (
			    Evoral::Parameter (PluginAutomation, 0, kAceEqGain2Port));
			std::shared_ptr<AutomationControl> gh = pi->automation_control (
			    Evoral::Parameter (PluginAutomation, 0, kAceEqGainHPort));
			if (gl) {
				_impl->track_eq_gain_l_db = (float)gl->get_value ();
			}
			if (gm) {
				_impl->track_eq_gain_m_db = (float)gm->get_value ();
			}
			if (gh) {
				_impl->track_eq_gain_h_db = (float)gh->get_value ();
			}
		} catch (...) {
		}
		refresh_track_eq_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: EQ \"%s\" on Audio %d (pre-fader)\n",
		              _impl->track_eq_name.c_str (), track + 1);
		std::fflush (stderr);
		_snap.detail = L"ACE EQ on Audio 3";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: insert_track_eq: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_eq_bypassed (int track, bool bypassed)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_eq (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_eq.lock ();
	if (!pi) {
		pi = find_ace_eq_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_eq = pi;
	}
	try {
		if (bypassed) {
			pi->deactivate ();
		} else {
			pi->activate ();
		}
		_impl->track_eq_bypassed = !pi->active ();
		refresh_track_eq_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_track_eq_bypass (int track)
{
	refresh_track_eq_snap ();
	return set_track_eq_bypassed (track, !_snap.track_eq_bypassed);
}

bool
EngineHost::set_track_eq_gain_l_db (int track, float db)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_eq (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_eq.lock ();
	if (!pi) {
		pi = find_ace_eq_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_eq = pi;
	}
	db = std::max (-20.f, std::min (20.f, db));
	try {
		std::shared_ptr<AutomationControl> ac = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceEqGainLPort));
		if (!ac) {
			_snap.detail = L"ACE EQ Gain L missing";
			return false;
		}
		ac->set_value (db, Controllable::NoGroup);
		_impl->track_eq_gain_l_db = (float)ac->get_value ();
		refresh_track_eq_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_eq_gain_m_db (int track, float db)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_eq (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_eq.lock ();
	if (!pi) {
		pi = find_ace_eq_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_eq = pi;
	}
	db = std::max (-20.f, std::min (20.f, db));
	try {
		std::shared_ptr<AutomationControl> ac = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceEqGain2Port));
		if (!ac) {
			_snap.detail = L"ACE EQ Gain 2 missing";
			return false;
		}
		ac->set_value (db, Controllable::NoGroup);
		_impl->track_eq_gain_m_db = (float)ac->get_value ();
		refresh_track_eq_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_eq_gain_h_db (int track, float db)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_eq (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_eq.lock ();
	if (!pi) {
		pi = find_ace_eq_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_eq = pi;
	}
	db = std::max (-20.f, std::min (20.f, db));
	try {
		std::shared_ptr<AutomationControl> ac = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceEqGainHPort));
		if (!ac) {
			_snap.detail = L"ACE EQ Gain H missing";
			return false;
		}
		ac->set_value (db, Controllable::NoGroup);
		_impl->track_eq_gain_h_db = (float)ac->get_value ();
		refresh_track_eq_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_eq_gain_l_norm (int track, float norm_0_1)
{
	return set_track_eq_gain_l_db (track, eq_norm_to_gain (norm_0_1));
}

bool
EngineHost::set_track_eq_gain_m_norm (int track, float norm_0_1)
{
	return set_track_eq_gain_m_db (track, eq_norm_to_gain (norm_0_1));
}

bool
EngineHost::set_track_eq_gain_h_norm (int track, float norm_0_1)
{
	return set_track_eq_gain_h_db (track, eq_norm_to_gain (norm_0_1));
}

bool
EngineHost::smoke_eq ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-eq failed — no Session";
		return false;
	}
	try {
		collect_tracks ();
		if (_impl->tracks.size () < 3 || !_impl->tracks[2]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-eq FAILED (need Audio 3)\n");
			std::fflush (stderr);
			return false;
		}
		if (!insert_track_eq (2) || !_snap.track_eq_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-eq FAILED (insert)\n");
			std::fflush (stderr);
			return false;
		}
		std::string verified;
		if (!find_ace_eq_insert (_impl->tracks[2], &verified)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-eq FAILED (not on route)\n");
			std::fflush (stderr);
			return false;
		}

		if (!set_track_eq_bypassed (2, false) || _snap.track_eq_bypassed) {
			std::fprintf (stderr, "airdour_win_shell: smoke-eq FAILED (bypass clear)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_track_eq_gain_l_db (2, -6.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-eq FAILED (Gain L)\n");
			std::fflush (stderr);
			return false;
		}
		if (_snap.track_eq_gain_l_db < -6.5f || _snap.track_eq_gain_l_db > -5.5f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-eq FAILED (L=%.1f)\n",
			              _snap.track_eq_gain_l_db);
			std::fflush (stderr);
			return false;
		}
		if (!set_track_eq_gain_m_db (2, 3.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-eq FAILED (Gain 2)\n");
			std::fflush (stderr);
			return false;
		}
		if (_snap.track_eq_gain_m_db < 2.5f || _snap.track_eq_gain_m_db > 3.5f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-eq FAILED (M=%.1f)\n",
			              _snap.track_eq_gain_m_db);
			std::fflush (stderr);
			return false;
		}
		if (!set_track_eq_gain_h_db (2, -3.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-eq FAILED (Gain H)\n");
			std::fflush (stderr);
			return false;
		}
		if (_snap.track_eq_gain_h_db < -3.5f || _snap.track_eq_gain_h_db > -2.5f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-eq FAILED (H=%.1f)\n",
			              _snap.track_eq_gain_h_db);
			std::fflush (stderr);
			return false;
		}

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-eq ok (fx=%s L=%.1f M=%.1f H=%.1f)\n",
		              verified.c_str (), _snap.track_eq_gain_l_db,
		              _snap.track_eq_gain_m_db, _snap.track_eq_gain_h_db);
		std::fflush (stderr);
		_snap.detail = L"smoke-eq ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-eq exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

void
EngineHost::refresh_track_delay_snap ()
{
	_snap.track_delay_ok = false;
	_snap.track_delay_bypassed = false;
	_snap.track_delay_time_ms = _impl ? _impl->track_delay_time_ms : 160.f;
	_snap.track_delay_feedback_pct = _impl ? _impl->track_delay_feedback_pct : 20.f;
	_snap.track_delay_time_norm = delay_time_to_norm (_snap.track_delay_time_ms);
	_snap.track_delay_feedback_norm = delay_feedback_to_norm (_snap.track_delay_feedback_pct);
	_snap.track_delay_name.clear ();
	if (!_impl || _impl->tracks.size () < 4 || !_impl->tracks[3]) {
		return;
	}
	std::string name;
	std::shared_ptr<PluginInsert> pi = _impl->track_delay.lock ();
	if (!pi) {
		pi = find_ace_delay_insert (_impl->tracks[3], &name);
		if (pi) {
			_impl->track_delay = pi;
		}
	} else {
		name = pi->name ().empty () ? "ACE Delay" : pi->name ();
	}
	if (!pi) {
		_impl->track_delay_ok = false;
		return;
	}
	_impl->track_delay_ok = true;
	_impl->track_delay_name = name.empty () ? "ACE Delay" : name;
	_impl->track_delay_bypassed = !pi->active ();
	try {
		std::shared_ptr<AutomationControl> tm = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceDelayTimePort));
		std::shared_ptr<AutomationControl> fb = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceDelayFeedbackPort));
		if (tm) {
			_impl->track_delay_time_ms = (float)tm->get_value ();
		}
		if (fb) {
			_impl->track_delay_feedback_pct = (float)fb->get_value ();
		}
	} catch (...) {
	}
	_snap.track_delay_ok = true;
	_snap.track_delay_bypassed = _impl->track_delay_bypassed;
	_snap.track_delay_time_ms = _impl->track_delay_time_ms;
	_snap.track_delay_feedback_pct = _impl->track_delay_feedback_pct;
	_snap.track_delay_time_norm = delay_time_to_norm (_impl->track_delay_time_ms);
	_snap.track_delay_feedback_norm = delay_feedback_to_norm (_impl->track_delay_feedback_pct);
	_snap.track_delay_name = utf8_to_wide (_impl->track_delay_name);
}

bool
EngineHost::insert_track_delay (int track)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		_snap.detail = L"insert Delay: no audio track";
		return false;
	}
	std::shared_ptr<AudioTrack> at = _impl->tracks[track];
	std::string existing;
	std::shared_ptr<PluginInsert> already = find_ace_delay_insert (at, &existing);
	if (already) {
		_impl->track_delay = already;
		_impl->track_delay_ok = true;
		_impl->track_delay_name = existing;
		refresh_track_delay_snap ();
		return true;
	}

	ensure_plugins_scanned ();
	try {
		PluginInfoPtr info = find_ace_delay_info ();
		if (!info) {
			_snap.detail = L"ACE Delay not found (check ARDOUR_DLL_PATH/LV2)";
			std::fprintf (stderr, "airdour_win_shell: ACE Delay LV2 missing\n");
			std::fflush (stderr);
			return false;
		}

		PluginPtr plugin = info->load (*_impl->session);
		if (!plugin) {
			_snap.detail = L"ACE Delay load failed";
			return false;
		}

		std::shared_ptr<PluginInsert> pi (
		    new PluginInsert (*_impl->session, *at, plugin));
		pi->set_strict_io (true);
		if (at->add_processor (pi, PreFader) != 0) {
			_snap.detail = L"ACE Delay add_processor failed";
			return false;
		}

		_impl->track_delay = pi;
		_impl->track_delay_ok = true;
		_impl->track_delay_bypassed = !pi->active ();
		_impl->track_delay_name = info->name.empty () ? "ACE Delay" : info->name;
		try {
			std::shared_ptr<AutomationControl> tm = pi->automation_control (
			    Evoral::Parameter (PluginAutomation, 0, kAceDelayTimePort));
			std::shared_ptr<AutomationControl> fb = pi->automation_control (
			    Evoral::Parameter (PluginAutomation, 0, kAceDelayFeedbackPort));
			if (tm) {
				_impl->track_delay_time_ms = (float)tm->get_value ();
			}
			if (fb) {
				_impl->track_delay_feedback_pct = (float)fb->get_value ();
			}
		} catch (...) {
		}
		refresh_track_delay_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: Delay \"%s\" on Audio %d (pre-fader)\n",
		              _impl->track_delay_name.c_str (), track + 1);
		std::fflush (stderr);
		_snap.detail = L"ACE Delay on Audio 4";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: insert_track_delay: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_delay_bypassed (int track, bool bypassed)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_delay (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_delay.lock ();
	if (!pi) {
		pi = find_ace_delay_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_delay = pi;
	}
	try {
		if (bypassed) {
			pi->deactivate ();
		} else {
			pi->activate ();
		}
		_impl->track_delay_bypassed = !pi->active ();
		refresh_track_delay_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_track_delay_bypass (int track)
{
	refresh_track_delay_snap ();
	return set_track_delay_bypassed (track, !_snap.track_delay_bypassed);
}

bool
EngineHost::set_track_delay_time_ms (int track, float ms)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_delay (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_delay.lock ();
	if (!pi) {
		pi = find_ace_delay_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_delay = pi;
	}
	ms = std::max (1.f, std::min (8000.f, ms));
	try {
		std::shared_ptr<AutomationControl> ac = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceDelayTimePort));
		if (!ac) {
			_snap.detail = L"ACE Delay Time missing";
			return false;
		}
		ac->set_value (ms, Controllable::NoGroup);
		_impl->track_delay_time_ms = (float)ac->get_value ();
		refresh_track_delay_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_delay_feedback_pct (int track, float pct)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_delay (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_delay.lock ();
	if (!pi) {
		pi = find_ace_delay_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_delay = pi;
	}
	pct = std::max (0.f, std::min (100.f, pct));
	try {
		std::shared_ptr<AutomationControl> ac = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceDelayFeedbackPort));
		if (!ac) {
			_snap.detail = L"ACE Delay Feedback missing";
			return false;
		}
		ac->set_value (pct, Controllable::NoGroup);
		_impl->track_delay_feedback_pct = (float)ac->get_value ();
		refresh_track_delay_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_delay_time_norm (int track, float norm_0_1)
{
	return set_track_delay_time_ms (track, delay_norm_to_time (norm_0_1));
}

bool
EngineHost::set_track_delay_feedback_norm (int track, float norm_0_1)
{
	return set_track_delay_feedback_pct (track, delay_norm_to_feedback (norm_0_1));
}

bool
EngineHost::smoke_delay ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-delay failed — no Session";
		return false;
	}
	try {
		collect_tracks ();
		if (_impl->tracks.size () < 4 || !_impl->tracks[3]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-delay FAILED (need Audio 4)\n");
			std::fflush (stderr);
			return false;
		}
		if (!insert_track_delay (3) || !_snap.track_delay_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-delay FAILED (insert)\n");
			std::fflush (stderr);
			return false;
		}
		std::string verified;
		if (!find_ace_delay_insert (_impl->tracks[3], &verified)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-delay FAILED (not on route)\n");
			std::fflush (stderr);
			return false;
		}

		if (!set_track_delay_bypassed (3, false) || _snap.track_delay_bypassed) {
			std::fprintf (stderr, "airdour_win_shell: smoke-delay FAILED (bypass clear)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_track_delay_time_ms (3, 250.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-delay FAILED (Time)\n");
			std::fflush (stderr);
			return false;
		}
		if (_snap.track_delay_time_ms < 240.f || _snap.track_delay_time_ms > 260.f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-delay FAILED (time=%.1f)\n",
			              _snap.track_delay_time_ms);
			std::fflush (stderr);
			return false;
		}
		if (!set_track_delay_feedback_pct (3, 35.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-delay FAILED (Feedback)\n");
			std::fflush (stderr);
			return false;
		}
		if (_snap.track_delay_feedback_pct < 30.f || _snap.track_delay_feedback_pct > 40.f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-delay FAILED (fb=%.1f)\n",
			              _snap.track_delay_feedback_pct);
			std::fflush (stderr);
			return false;
		}

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-delay ok (fx=%s time=%.0fms fb=%.0f%%)\n",
		              verified.c_str (), _snap.track_delay_time_ms,
		              _snap.track_delay_feedback_pct);
		std::fflush (stderr);
		_snap.detail = L"smoke-delay ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-delay exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

void
EngineHost::refresh_track_limiter_snap ()
{
	_snap.track_limiter_ok = false;
	_snap.track_limiter_bypassed = false;
	_snap.track_limiter_ceiling_db = _impl ? _impl->track_limiter_ceiling_db : -0.3f;
	_snap.track_limiter_release_ms = _impl ? _impl->track_limiter_release_ms : 100.f;
	_snap.track_limiter_ceiling_norm = limiter_ceiling_to_norm (_snap.track_limiter_ceiling_db);
	_snap.track_limiter_release_norm = limiter_release_to_norm (_snap.track_limiter_release_ms);
	_snap.track_limiter_name.clear ();
	if (!_impl || _impl->tracks.size () < 5 || !_impl->tracks[4]) {
		return;
	}
	std::string name;
	std::shared_ptr<PluginInsert> pi = _impl->track_limiter.lock ();
	if (!pi) {
		pi = find_ace_limiter_insert (_impl->tracks[4], &name);
		if (pi) {
			_impl->track_limiter = pi;
		}
	} else {
		name = pi->name ().empty () ? "ACE Limiter" : pi->name ();
	}
	if (!pi) {
		_impl->track_limiter_ok = false;
		return;
	}
	_impl->track_limiter_ok = true;
	_impl->track_limiter_name = name.empty () ? "ACE Limiter" : name;
	_impl->track_limiter_bypassed = !pi->active ();
	try {
		std::shared_ptr<AutomationControl> ceil = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceLimiterCeilingPort));
		std::shared_ptr<AutomationControl> rel = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceLimiterReleasePort));
		if (ceil) {
			_impl->track_limiter_ceiling_db = (float)ceil->get_value ();
		}
		if (rel) {
			_impl->track_limiter_release_ms = (float)rel->get_value ();
		}
	} catch (...) {
	}
	_snap.track_limiter_ok = true;
	_snap.track_limiter_bypassed = _impl->track_limiter_bypassed;
	_snap.track_limiter_ceiling_db = _impl->track_limiter_ceiling_db;
	_snap.track_limiter_release_ms = _impl->track_limiter_release_ms;
	_snap.track_limiter_ceiling_norm = limiter_ceiling_to_norm (_impl->track_limiter_ceiling_db);
	_snap.track_limiter_release_norm = limiter_release_to_norm (_impl->track_limiter_release_ms);
	_snap.track_limiter_name = utf8_to_wide (_impl->track_limiter_name);
}

bool
EngineHost::insert_track_limiter (int track)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		_snap.detail = L"insert Limiter: no audio track";
		return false;
	}
	std::shared_ptr<AudioTrack> at = _impl->tracks[track];
	std::string existing;
	std::shared_ptr<PluginInsert> already = find_ace_limiter_insert (at, &existing);
	if (already) {
		_impl->track_limiter = already;
		_impl->track_limiter_ok = true;
		_impl->track_limiter_name = existing;
		refresh_track_limiter_snap ();
		return true;
	}

	ensure_plugins_scanned ();
	try {
		PluginInfoPtr info = find_ace_limiter_info ();
		if (!info) {
			_snap.detail = L"ACE Limiter not found (check ARDOUR_DLL_PATH/LV2)";
			std::fprintf (stderr, "airdour_win_shell: ACE Limiter LV2 missing\n");
			std::fflush (stderr);
			return false;
		}

		PluginPtr plugin = info->load (*_impl->session);
		if (!plugin) {
			_snap.detail = L"ACE Limiter load failed";
			return false;
		}

		std::shared_ptr<PluginInsert> pi (
		    new PluginInsert (*_impl->session, *at, plugin));
		pi->set_strict_io (true);
		if (at->add_processor (pi, PreFader) != 0) {
			_snap.detail = L"ACE Limiter add_processor failed";
			return false;
		}

		_impl->track_limiter = pi;
		_impl->track_limiter_ok = true;
		_impl->track_limiter_bypassed = !pi->active ();
		_impl->track_limiter_name = info->name.empty () ? "ACE Limiter" : info->name;
		try {
			std::shared_ptr<AutomationControl> ceil = pi->automation_control (
			    Evoral::Parameter (PluginAutomation, 0, kAceLimiterCeilingPort));
			std::shared_ptr<AutomationControl> rel = pi->automation_control (
			    Evoral::Parameter (PluginAutomation, 0, kAceLimiterReleasePort));
			if (ceil) {
				_impl->track_limiter_ceiling_db = (float)ceil->get_value ();
			}
			if (rel) {
				_impl->track_limiter_release_ms = (float)rel->get_value ();
			}
		} catch (...) {
		}
		refresh_track_limiter_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: Limiter \"%s\" on Audio %d (pre-fader)\n",
		              _impl->track_limiter_name.c_str (), track + 1);
		std::fflush (stderr);
		_snap.detail = L"ACE Limiter on Audio 5";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: insert_track_limiter: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_limiter_bypassed (int track, bool bypassed)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_limiter (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_limiter.lock ();
	if (!pi) {
		pi = find_ace_limiter_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_limiter = pi;
	}
	try {
		if (bypassed) {
			pi->deactivate ();
		} else {
			pi->activate ();
		}
		_impl->track_limiter_bypassed = !pi->active ();
		refresh_track_limiter_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_track_limiter_bypass (int track)
{
	refresh_track_limiter_snap ();
	return set_track_limiter_bypassed (track, !_snap.track_limiter_bypassed);
}

bool
EngineHost::set_track_limiter_ceiling_db (int track, float db)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_limiter (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_limiter.lock ();
	if (!pi) {
		pi = find_ace_limiter_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_limiter = pi;
	}
	db = std::max (-20.f, std::min (0.f, db));
	try {
		std::shared_ptr<AutomationControl> ac = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceLimiterCeilingPort));
		if (!ac) {
			_snap.detail = L"ACE Limiter Ceiling missing";
			return false;
		}
		ac->set_value (db, Controllable::NoGroup);
		_impl->track_limiter_ceiling_db = (float)ac->get_value ();
		refresh_track_limiter_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_limiter_release_ms (int track, float ms)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_limiter (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_limiter.lock ();
	if (!pi) {
		pi = find_ace_limiter_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_limiter = pi;
	}
	ms = std::max (1.f, std::min (2000.f, ms));
	try {
		std::shared_ptr<AutomationControl> ac = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceLimiterReleasePort));
		if (!ac) {
			_snap.detail = L"ACE Limiter Release missing";
			return false;
		}
		ac->set_value (ms, Controllable::NoGroup);
		_impl->track_limiter_release_ms = (float)ac->get_value ();
		refresh_track_limiter_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_limiter_ceiling_norm (int track, float norm_0_1)
{
	return set_track_limiter_ceiling_db (track, limiter_norm_to_ceiling (norm_0_1));
}

bool
EngineHost::set_track_limiter_release_norm (int track, float norm_0_1)
{
	return set_track_limiter_release_ms (track, limiter_norm_to_release (norm_0_1));
}

bool
EngineHost::smoke_limiter ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-limiter failed — no Session";
		return false;
	}
	try {
		collect_tracks ();
		if (_impl->tracks.size () < 5 || !_impl->tracks[4]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-limiter FAILED (need Audio 5)\n");
			std::fflush (stderr);
			return false;
		}
		if (!insert_track_limiter (4) || !_snap.track_limiter_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-limiter FAILED (insert)\n");
			std::fflush (stderr);
			return false;
		}
		std::string verified;
		if (!find_ace_limiter_insert (_impl->tracks[4], &verified)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-limiter FAILED (not on route)\n");
			std::fflush (stderr);
			return false;
		}

		if (!set_track_limiter_bypassed (4, false) || _snap.track_limiter_bypassed) {
			std::fprintf (stderr, "airdour_win_shell: smoke-limiter FAILED (bypass clear)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_track_limiter_ceiling_db (4, -3.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-limiter FAILED (Ceiling)\n");
			std::fflush (stderr);
			return false;
		}
		if (_snap.track_limiter_ceiling_db < -3.5f || _snap.track_limiter_ceiling_db > -2.5f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-limiter FAILED (ceil=%.1f)\n",
			              _snap.track_limiter_ceiling_db);
			std::fflush (stderr);
			return false;
		}
		if (!set_track_limiter_release_ms (4, 200.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-limiter FAILED (Release)\n");
			std::fflush (stderr);
			return false;
		}
		if (_snap.track_limiter_release_ms < 180.f || _snap.track_limiter_release_ms > 220.f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-limiter FAILED (rel=%.1f)\n",
			              _snap.track_limiter_release_ms);
			std::fflush (stderr);
			return false;
		}

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-limiter ok (fx=%s ceil=%.1f rel=%.0fms)\n",
		              verified.c_str (), _snap.track_limiter_ceiling_db,
		              _snap.track_limiter_release_ms);
		std::fflush (stderr);
		_snap.detail = L"smoke-limiter ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-limiter exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}


void
EngineHost::refresh_track_gate_snap ()
{
	_snap.track_gate_ok = false;
	_snap.track_gate_bypassed = false;
	_snap.track_gate_threshold_db = _impl ? _impl->track_gate_threshold_db : -40.f;
	_snap.track_gate_hold_ms = _impl ? _impl->track_gate_hold_ms : 50.f;
	_snap.track_gate_threshold_norm = gate_threshold_to_norm (_snap.track_gate_threshold_db);
	_snap.track_gate_hold_norm = gate_hold_to_norm (_snap.track_gate_hold_ms);
	_snap.track_gate_name.clear ();
	if (!_impl || _impl->tracks.size () < 6 || !_impl->tracks[5]) {
		return;
	}
	std::string name;
	std::shared_ptr<PluginInsert> pi = _impl->track_gate.lock ();
	if (!pi) {
		pi = find_ace_gate_insert (_impl->tracks[5], &name);
		if (pi) {
			_impl->track_gate = pi;
		}
	} else {
		name = pi->name ().empty () ? "ACE Gate" : pi->name ();
	}
	if (!pi) {
		_impl->track_gate_ok = false;
		return;
	}
	_impl->track_gate_ok = true;
	_impl->track_gate_name = name.empty () ? "ACE Gate" : name;
	_impl->track_gate_bypassed = !pi->active ();
	try {
		std::shared_ptr<AutomationControl> thr = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceGateThresholdPort));
		std::shared_ptr<AutomationControl> hold = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceGateHoldPort));
		if (thr) {
			_impl->track_gate_threshold_db = (float)thr->get_value ();
		}
		if (hold) {
			_impl->track_gate_hold_ms = (float)hold->get_value ();
		}
	} catch (...) {
	}
	_snap.track_gate_ok = true;
	_snap.track_gate_bypassed = _impl->track_gate_bypassed;
	_snap.track_gate_threshold_db = _impl->track_gate_threshold_db;
	_snap.track_gate_hold_ms = _impl->track_gate_hold_ms;
	_snap.track_gate_threshold_norm = gate_threshold_to_norm (_impl->track_gate_threshold_db);
	_snap.track_gate_hold_norm = gate_hold_to_norm (_impl->track_gate_hold_ms);
	_snap.track_gate_name = utf8_to_wide (_impl->track_gate_name);
}

bool
EngineHost::insert_track_gate (int track)
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		_snap.detail = L"insert Gate: no audio track";
		return false;
	}
	std::shared_ptr<AudioTrack> at = _impl->tracks[track];
	std::string existing;
	std::shared_ptr<PluginInsert> already = find_ace_gate_insert (at, &existing);
	if (already) {
		_impl->track_gate = already;
		_impl->track_gate_ok = true;
		_impl->track_gate_name = existing;
		refresh_track_gate_snap ();
		return true;
	}

	ensure_plugins_scanned ();
	try {
		PluginInfoPtr info = find_ace_gate_info ();
		if (!info) {
			_snap.detail = L"ACE Gate not found (check ARDOUR_DLL_PATH/LV2)";
			std::fprintf (stderr, "airdour_win_shell: ACE Gate LV2 missing\n");
			std::fflush (stderr);
			return false;
		}

		PluginPtr plugin = info->load (*_impl->session);
		if (!plugin) {
			_snap.detail = L"ACE Gate load failed";
			return false;
		}

		std::shared_ptr<PluginInsert> pi (
		    new PluginInsert (*_impl->session, *at, plugin));
		pi->set_strict_io (true);
		if (at->add_processor (pi, PreFader) != 0) {
			_snap.detail = L"ACE Gate add_processor failed";
			return false;
		}

		_impl->track_gate = pi;
		_impl->track_gate_ok = true;
		_impl->track_gate_bypassed = !pi->active ();
		_impl->track_gate_name = info->name.empty () ? "ACE Gate" : info->name;
		try {
			std::shared_ptr<AutomationControl> thr = pi->automation_control (
			    Evoral::Parameter (PluginAutomation, 0, kAceGateThresholdPort));
			std::shared_ptr<AutomationControl> hold = pi->automation_control (
			    Evoral::Parameter (PluginAutomation, 0, kAceGateHoldPort));
			if (thr) {
				_impl->track_gate_threshold_db = (float)thr->get_value ();
			}
			if (hold) {
				_impl->track_gate_hold_ms = (float)hold->get_value ();
			}
		} catch (...) {
		}
		refresh_track_gate_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: Gate \"%s\" on Audio %d (pre-fader)\n",
		              _impl->track_gate_name.c_str (), track + 1);
		std::fflush (stderr);
		_snap.detail = L"ACE Gate on Audio 6";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: insert_track_gate: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_gate_bypassed (int track, bool bypassed)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_gate (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_gate.lock ();
	if (!pi) {
		pi = find_ace_gate_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_gate = pi;
	}
	try {
		if (bypassed) {
			pi->deactivate ();
		} else {
			pi->activate ();
		}
		_impl->track_gate_bypassed = !pi->active ();
		refresh_track_gate_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::toggle_track_gate_bypass (int track)
{
	refresh_track_gate_snap ();
	return set_track_gate_bypassed (track, !_snap.track_gate_bypassed);
}

bool
EngineHost::set_track_gate_threshold_db (int track, float db)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_gate (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_gate.lock ();
	if (!pi) {
		pi = find_ace_gate_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_gate = pi;
	}
	db = std::max (-60.f, std::min (0.f, db));
	try {
		std::shared_ptr<AutomationControl> ac = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceGateThresholdPort));
		if (!ac) {
			_snap.detail = L"ACE Gate Threshold missing";
			return false;
		}
		ac->set_value (db, Controllable::NoGroup);
		_impl->track_gate_threshold_db = (float)ac->get_value ();
		refresh_track_gate_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_gate_hold_ms (int track, float ms)
{
	if (!_impl || track < 0 || track >= (int)_impl->tracks.size () || !_impl->tracks[track]) {
		return false;
	}
	if (!insert_track_gate (track)) {
		return false;
	}
	std::shared_ptr<PluginInsert> pi = _impl->track_gate.lock ();
	if (!pi) {
		pi = find_ace_gate_insert (_impl->tracks[track]);
		if (!pi) {
			return false;
		}
		_impl->track_gate = pi;
	}
	ms = std::max (1.f, std::min (500.f, ms));
	try {
		std::shared_ptr<AutomationControl> ac = pi->automation_control (
		    Evoral::Parameter (PluginAutomation, 0, kAceGateHoldPort));
		if (!ac) {
			_snap.detail = L"ACE Gate Hold missing";
			return false;
		}
		ac->set_value (ms, Controllable::NoGroup);
		_impl->track_gate_hold_ms = (float)ac->get_value ();
		refresh_track_gate_snap ();
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_track_gate_threshold_norm (int track, float norm_0_1)
{
	return set_track_gate_threshold_db (track, gate_norm_to_threshold (norm_0_1));
}

bool
EngineHost::set_track_gate_hold_norm (int track, float norm_0_1)
{
	return set_track_gate_hold_ms (track, gate_norm_to_hold (norm_0_1));
}

bool
EngineHost::smoke_gate ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-gate failed — no Session";
		return false;
	}
	try {
		collect_tracks ();
		if (_impl->tracks.size () < 6 || !_impl->tracks[5]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-gate FAILED (need Audio 6)\n");
			std::fflush (stderr);
			return false;
		}
		if (!insert_track_gate (5) || !_snap.track_gate_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-gate FAILED (insert)\n");
			std::fflush (stderr);
			return false;
		}
		std::string verified;
		if (!find_ace_gate_insert (_impl->tracks[5], &verified)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-gate FAILED (not on route)\n");
			std::fflush (stderr);
			return false;
		}

		if (!set_track_gate_bypassed (5, false) || _snap.track_gate_bypassed) {
			std::fprintf (stderr, "airdour_win_shell: smoke-gate FAILED (bypass clear)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_track_gate_threshold_db (5, -35.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-gate FAILED (Threshold)\n");
			std::fflush (stderr);
			return false;
		}
		if (_snap.track_gate_threshold_db < -35.5f || _snap.track_gate_threshold_db > -34.5f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-gate FAILED (thr=%.1f)\n",
			              _snap.track_gate_threshold_db);
			std::fflush (stderr);
			return false;
		}
		if (!set_track_gate_hold_ms (5, 80.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-gate FAILED (Hold)\n");
			std::fflush (stderr);
			return false;
		}
		if (_snap.track_gate_hold_ms < 70.f || _snap.track_gate_hold_ms > 90.f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-gate FAILED (hold=%.1f)\n",
			              _snap.track_gate_hold_ms);
			std::fflush (stderr);
			return false;
		}

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-gate ok (fx=%s thr=%.1f hold=%.0fms)\n",
		              verified.c_str (), _snap.track_gate_threshold_db,
		              _snap.track_gate_hold_ms);
		std::fflush (stderr);
		_snap.detail = L"smoke-gate ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-gate exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}


/* UI-rate spectrum: fixed Goertzel bands over a 512-sample window. Preallocated. */
static constexpr uint32_t kSpecFftN = 512;

static void
spectrum_goertzel_bands (const float* samples, uint32_t n, float sample_rate,
                         float* out_bands, int n_bands)
{
	if (!samples || !out_bands || n_bands <= 0 || n < 8 || sample_rate < 100.f) {
		for (int i = 0; i < n_bands; ++i) {
			out_bands[i] = 0.f;
		}
		return;
	}
	const float nyq = sample_rate * 0.5f;
	const float f_lo = 40.f;
	const float f_hi = std::min (nyq * 0.92f, 16000.f);
	for (int b = 0; b < n_bands; ++b) {
		float t = (n_bands == 1) ? 0.f : (float)b / (float)(n_bands - 1);
		float freq = f_lo * std::pow (f_hi / f_lo, t);
		float k = freq * (float)n / sample_rate;
		float omega = 2.f * 3.14159265f * k / (float)n;
		float coeff = 2.f * std::cos (omega);
		float s0 = 0.f, s1 = 0.f, s2 = 0.f;
		for (uint32_t i = 0; i < n; ++i) {
			/* Hann window */
			float w = 0.5f - 0.5f * std::cos (2.f * 3.14159265f * (float)i / (float)(n - 1));
			float x = samples[i] * w;
			s0 = x + coeff * s1 - s2;
			s2 = s1;
			s1 = s0;
		}
		float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
		if (!(power > 0.f)) {
			power = 0.f;
		}
		float db = (power > 1.0e-20f) ? (10.f * std::log10 (power)) : -100.f;
		/* Map roughly −90..0 dB → 0..1 */
		float norm = (db + 90.f) / 90.f;
		if (norm < 0.f) {
			norm = 0.f;
		}
		if (norm > 1.f) {
			norm = 1.f;
		}
		out_bands[b] = norm;
	}
}

void
EngineHost::refresh_spectrum_snap ()
{
	if (!_snap.spectrum_enabled) {
		_snap.spectrum_ok = false;
		return;
	}
	if (!_impl || !_impl->session) {
		_snap.spectrum_ok = false;
		for (int i = 0; i < EngineSnapshot::kSpectrumBands; ++i) {
			_snap.spectrum_band[i] *= 0.85f;
		}
		return;
	}

	static float work[kSpecFftN];
	static float mixdown[kSpecFftN];
	static float gainbuf[kSpecFftN];
	std::memset (work, 0, sizeof (work));

	samplecnt_t sr = _impl->session->sample_rate ();
	if (sr <= 0) {
		sr = 48000;
	}
	samplepos_t pos = _impl->session->transport_sample ();
	if (pos < 0) {
		pos = 0;
	}

	bool got = false;
	try {
		collect_tracks ();
		if (_snap.spectrum_master) {
			_snap.spectrum_track = -1;
			wcsncpy_s (_snap.spectrum_label, L"Master mix", _TRUNCATE);
			float scale = 1.f;
			int ntr = (int)_impl->tracks.size ();
			if (ntr > 1) {
				scale = 1.f / std::sqrt ((float)ntr);
			}
			for (size_t t = 0; t < _impl->tracks.size (); ++t) {
				std::shared_ptr<AudioTrack> at = _impl->tracks[t];
				if (!at || !at->playlist ()) {
					continue;
				}
				std::shared_ptr<AudioPlaylist> apl =
				    std::dynamic_pointer_cast<AudioPlaylist> (at->playlist ());
				if (!apl) {
					continue;
				}
				static float dst[kSpecFftN];
				std::memset (dst, 0, sizeof (dst));
				std::memset (mixdown, 0, sizeof (mixdown));
				std::memset (gainbuf, 0, sizeof (gainbuf));
				apl->read (dst, mixdown, gainbuf,
				           timepos_t (pos), timecnt_t ((samplecnt_t)kSpecFftN), 0);
				for (uint32_t i = 0; i < kSpecFftN; ++i) {
					work[i] += dst[i] * scale;
				}
				got = true;
			}
		} else {
			int tr = _snap.selection_track;
			if (tr < 0 || tr >= (int)_impl->tracks.size () || !_impl->tracks[tr]) {
				tr = 0;
			}
			_snap.spectrum_track = tr;
			if (tr < EngineSnapshot::kMeters && _snap.track_name[tr][0]) {
				wcsncpy_s (_snap.spectrum_label, _snap.track_name[tr], _TRUNCATE);
			} else {
				wchar_t lab[32];
				std::swprintf (lab, 32, L"Audio %d", tr + 1);
				wcsncpy_s (_snap.spectrum_label, lab, _TRUNCATE);
			}
			if (tr < (int)_impl->tracks.size () && _impl->tracks[tr]
			    && _impl->tracks[tr]->playlist ()) {
				std::shared_ptr<AudioPlaylist> apl =
				    std::dynamic_pointer_cast<AudioPlaylist> (
				        _impl->tracks[tr]->playlist ());
				if (apl) {
					std::memset (mixdown, 0, sizeof (mixdown));
					std::memset (gainbuf, 0, sizeof (gainbuf));
					apl->read (work, mixdown, gainbuf,
					           timepos_t (pos), timecnt_t ((samplecnt_t)kSpecFftN), 0);
					got = true;
				}
			}
		}
	} catch (...) {
		got = false;
	}

	float bands[EngineSnapshot::kSpectrumBands];
	spectrum_goertzel_bands (work, kSpecFftN, (float)sr, bands,
	                         EngineSnapshot::kSpectrumBands);

	/* Soft decay when silent / no playlist data. */
	float energy = 0.f;
	for (int i = 0; i < EngineSnapshot::kSpectrumBands; ++i) {
		energy += bands[i];
	}
	if (!got || energy < 1.0e-4f) {
		for (int i = 0; i < EngineSnapshot::kSpectrumBands; ++i) {
			_snap.spectrum_band[i] *= 0.88f;
		}
		_snap.spectrum_ok = got;
		return;
	}
	for (int i = 0; i < EngineSnapshot::kSpectrumBands; ++i) {
		float v = bands[i];
		if (v >= _snap.spectrum_band[i]) {
			_snap.spectrum_band[i] = v;
		} else {
			_snap.spectrum_band[i] = _snap.spectrum_band[i] * 0.75f + v * 0.25f;
		}
	}
	_snap.spectrum_ok = true;
}

bool
EngineHost::set_spectrum_enabled (bool on)
{
	_snap.spectrum_enabled = on;
	if (!on) {
		_snap.spectrum_ok = false;
		for (int i = 0; i < EngineSnapshot::kSpectrumBands; ++i) {
			_snap.spectrum_band[i] = 0.f;
		}
		_snap.detail = L"spectrum off";
		return true;
	}
	refresh_spectrum_snap ();
	_snap.detail = L"spectrum on";
	return true;
}

bool
EngineHost::toggle_spectrum_enabled ()
{
	return set_spectrum_enabled (!_snap.spectrum_enabled);
}

bool
EngineHost::set_spectrum_master (bool master)
{
	_snap.spectrum_master = master;
	if (_snap.spectrum_enabled) {
		refresh_spectrum_snap ();
	}
	return true;
}

bool
EngineHost::toggle_spectrum_master ()
{
	return set_spectrum_master (!_snap.spectrum_master);
}

bool
EngineHost::smoke_spectrum ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-spectrum failed — no Session";
		return false;
	}
	try {
		if (!set_spectrum_enabled (true) || !_snap.spectrum_enabled) {
			std::fprintf (stderr, "airdour_win_shell: smoke-spectrum FAILED (enable)\n");
			std::fflush (stderr);
			return false;
		}

		/* Synthetic 440 Hz tone through the same Goertzel path. */
		static float tone[kSpecFftN];
		samplecnt_t sr = _impl->session->sample_rate ();
		if (sr <= 0) {
			sr = 48000;
		}
		for (uint32_t i = 0; i < kSpecFftN; ++i) {
			tone[i] = 0.35f * std::sin (2.f * 3.14159265f * 440.f * (float)i / (float)sr);
		}
		float bands[EngineSnapshot::kSpectrumBands];
		spectrum_goertzel_bands (tone, kSpecFftN, (float)sr, bands,
		                         EngineSnapshot::kSpectrumBands);
		float peak = 0.f;
		int peak_i = 0;
		for (int i = 0; i < EngineSnapshot::kSpectrumBands; ++i) {
			_snap.spectrum_band[i] = bands[i];
			if (bands[i] > peak) {
				peak = bands[i];
				peak_i = i;
			}
		}
		_snap.spectrum_ok = (peak > 0.05f);
		if (!_snap.spectrum_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-spectrum FAILED (peak=%.3f)\n",
			              peak);
			std::fflush (stderr);
			return false;
		}

		/* Live playlist path (may be silent — still must not throw). */
		set_spectrum_master (false);
		refresh_spectrum_snap ();
		set_spectrum_master (true);
		refresh_spectrum_snap ();
		set_spectrum_master (false);
		refresh_spectrum_snap ();

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-spectrum ok (bands=%d peak=%.2f@%d)\n",
		              EngineSnapshot::kSpectrumBands, peak, peak_i);
		std::fflush (stderr);
		_snap.detail = L"smoke-spectrum ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-spectrum exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}


static float
stereo_correlation (const float* L, const float* R, uint32_t n)
{
	if (!L || !R || n < 8) {
		return 0.f;
	}
	double sum_l = 0.0, sum_r = 0.0;
	for (uint32_t i = 0; i < n; ++i) {
		sum_l += (double)L[i];
		sum_r += (double)R[i];
	}
	const double mean_l = sum_l / (double)n;
	const double mean_r = sum_r / (double)n;
	double num = 0.0, den_l = 0.0, den_r = 0.0;
	for (uint32_t i = 0; i < n; ++i) {
		double dl = (double)L[i] - mean_l;
		double dr = (double)R[i] - mean_r;
		num += dl * dr;
		den_l += dl * dl;
		den_r += dr * dr;
	}
	double den = std::sqrt (den_l * den_r);
	if (den < 1.0e-20) {
		return 0.f;
	}
	double r = num / den;
	if (r > 1.0) {
		r = 1.0;
	}
	if (r < -1.0) {
		r = -1.0;
	}
	return (float)r;
}

void
EngineHost::refresh_correlation_snap ()
{
	if (!_snap.correlation_enabled) {
		_snap.correlation_ok = false;
		return;
	}
	if (!_impl || !_impl->session) {
		_snap.correlation_ok = false;
		_snap.correlation *= 0.85f;
		return;
	}

	static float left[kSpecFftN];
	static float right[kSpecFftN];
	static float mixdown[kSpecFftN];
	static float gainbuf[kSpecFftN];
	std::memset (left, 0, sizeof (left));
	std::memset (right, 0, sizeof (right));

	samplepos_t pos = _impl->session->transport_sample ();
	if (pos < 0) {
		pos = 0;
	}

	bool got = false;
	try {
		collect_tracks ();
		if (_snap.correlation_master) {
			_snap.correlation_track = -1;
			wcsncpy_s (_snap.correlation_label, L"Master L/R", _TRUNCATE);
			float scale = 1.f;
			int ntr = (int)_impl->tracks.size ();
			if (ntr > 1) {
				scale = 1.f / std::sqrt ((float)ntr);
			}
			for (size_t t = 0; t < _impl->tracks.size (); ++t) {
				std::shared_ptr<AudioTrack> at = _impl->tracks[t];
				if (!at || !at->playlist ()) {
					continue;
				}
				std::shared_ptr<AudioPlaylist> apl =
				    std::dynamic_pointer_cast<AudioPlaylist> (at->playlist ());
				if (!apl) {
					continue;
				}
				static float dst_l[kSpecFftN];
				static float dst_r[kSpecFftN];
				std::memset (dst_l, 0, sizeof (dst_l));
				std::memset (dst_r, 0, sizeof (dst_r));
				std::memset (mixdown, 0, sizeof (mixdown));
				std::memset (gainbuf, 0, sizeof (gainbuf));
				apl->read (dst_l, mixdown, gainbuf,
				           timepos_t (pos), timecnt_t ((samplecnt_t)kSpecFftN), 0);
				std::memset (mixdown, 0, sizeof (mixdown));
				std::memset (gainbuf, 0, sizeof (gainbuf));
				apl->read (dst_r, mixdown, gainbuf,
				           timepos_t (pos), timecnt_t ((samplecnt_t)kSpecFftN), 1);
				/* Mono playlist: chan1 empty → copy L to R. */
				float er = 0.f;
				for (uint32_t i = 0; i < kSpecFftN; ++i) {
					er += std::fabs (dst_r[i]);
				}
				if (er < 1.0e-6f) {
					std::memcpy (dst_r, dst_l, sizeof (dst_r));
				}
				for (uint32_t i = 0; i < kSpecFftN; ++i) {
					left[i] += dst_l[i] * scale;
					right[i] += dst_r[i] * scale;
				}
				got = true;
			}
		} else {
			int tr = _snap.selection_track;
			if (tr < 0 || tr >= (int)_impl->tracks.size () || !_impl->tracks[tr]) {
				tr = 0;
			}
			_snap.correlation_track = tr;
			if (tr < EngineSnapshot::kMeters && _snap.track_name[tr][0]) {
				wcsncpy_s (_snap.correlation_label, _snap.track_name[tr], _TRUNCATE);
			} else {
				wchar_t lab[32];
				std::swprintf (lab, 32, L"Audio %d", tr + 1);
				wcsncpy_s (_snap.correlation_label, lab, _TRUNCATE);
			}
			if (tr < (int)_impl->tracks.size () && _impl->tracks[tr]
			    && _impl->tracks[tr]->playlist ()) {
				std::shared_ptr<AudioPlaylist> apl =
				    std::dynamic_pointer_cast<AudioPlaylist> (
				        _impl->tracks[tr]->playlist ());
				if (apl) {
					std::memset (mixdown, 0, sizeof (mixdown));
					std::memset (gainbuf, 0, sizeof (gainbuf));
					apl->read (left, mixdown, gainbuf,
					           timepos_t (pos), timecnt_t ((samplecnt_t)kSpecFftN), 0);
					std::memset (mixdown, 0, sizeof (mixdown));
					std::memset (gainbuf, 0, sizeof (gainbuf));
					apl->read (right, mixdown, gainbuf,
					           timepos_t (pos), timecnt_t ((samplecnt_t)kSpecFftN), 1);
					float er = 0.f;
					for (uint32_t i = 0; i < kSpecFftN; ++i) {
						er += std::fabs (right[i]);
					}
					if (er < 1.0e-6f) {
						std::memcpy (right, left, sizeof (right));
					}
					got = true;
				}
			}
		}
	} catch (...) {
		got = false;
	}

	float r = stereo_correlation (left, right, kSpecFftN);
	float energy = 0.f;
	for (uint32_t i = 0; i < kSpecFftN; ++i) {
		energy += std::fabs (left[i]) + std::fabs (right[i]);
	}
	if (!got || energy < 1.0e-5f) {
		_snap.correlation *= 0.88f;
		_snap.correlation_ok = got;
		return;
	}
	_snap.correlation = _snap.correlation * 0.55f + r * 0.45f;

	/* Decimate for goniometer dots. */
	const uint32_t step = kSpecFftN / EngineSnapshot::kGonioPts;
	float peak = 1.0e-6f;
	for (uint32_t i = 0; i < kSpecFftN; ++i) {
		peak = std::max (peak, std::fabs (left[i]));
		peak = std::max (peak, std::fabs (right[i]));
	}
	const float inv = 1.f / peak;
	for (int i = 0; i < EngineSnapshot::kGonioPts; ++i) {
		uint32_t idx = (uint32_t)i * step;
		if (idx >= kSpecFftN) {
			idx = kSpecFftN - 1;
		}
		_snap.gonio_x[i] = std::max (-1.f, std::min (1.f, left[idx] * inv));
		_snap.gonio_y[i] = std::max (-1.f, std::min (1.f, right[idx] * inv));
	}
	_snap.correlation_ok = true;
}

bool
EngineHost::set_correlation_enabled (bool on)
{
	_snap.correlation_enabled = on;
	if (!on) {
		_snap.correlation_ok = false;
		_snap.correlation = 0.f;
		_snap.detail = L"phase meter off";
		return true;
	}
	refresh_correlation_snap ();
	_snap.detail = L"phase meter on";
	return true;
}

bool
EngineHost::toggle_correlation_enabled ()
{
	return set_correlation_enabled (!_snap.correlation_enabled);
}

bool
EngineHost::set_correlation_master (bool master)
{
	_snap.correlation_master = master;
	if (_snap.correlation_enabled) {
		refresh_correlation_snap ();
	}
	return true;
}

bool
EngineHost::toggle_correlation_master ()
{
	return set_correlation_master (!_snap.correlation_master);
}

bool
EngineHost::smoke_correlation ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-correlation failed — no Session";
		return false;
	}
	try {
		if (!set_correlation_enabled (true) || !_snap.correlation_enabled) {
			std::fprintf (stderr, "airdour_win_shell: smoke-correlation FAILED (enable)\n");
			std::fflush (stderr);
			return false;
		}

		static float L[kSpecFftN];
		static float R[kSpecFftN];
		for (uint32_t i = 0; i < kSpecFftN; ++i) {
			float s = 0.3f * std::sin (2.f * 3.14159265f * (float)i / 32.f);
			L[i] = s;
			R[i] = s;
		}
		float r_mono = stereo_correlation (L, R, kSpecFftN);
		if (r_mono < 0.95f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-correlation FAILED (mono=%.3f)\n",
			              r_mono);
			std::fflush (stderr);
			return false;
		}
		for (uint32_t i = 0; i < kSpecFftN; ++i) {
			R[i] = -L[i];
		}
		float r_anti = stereo_correlation (L, R, kSpecFftN);
		if (r_anti > -0.95f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-correlation FAILED (anti=%.3f)\n",
			              r_anti);
			std::fflush (stderr);
			return false;
		}

		_snap.correlation = r_mono;
		set_correlation_master (false);
		refresh_correlation_snap ();
		set_correlation_master (true);
		refresh_correlation_snap ();
		set_correlation_master (false);

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-correlation ok (mono=%.2f anti=%.2f)\n",
		              r_mono, r_anti);
		std::fflush (stderr);
		_snap.detail = L"smoke-correlation ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-correlation exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}


/* UI-rate approx LUFS: K-weight (BS.1770-ish) over ~400 ms window. Preallocated.
 * Not a certified meter — labeled ≈ in the UI.
 */
static constexpr uint32_t kLufsWin = 19200; /* 400 ms @ 48 kHz */

struct KWeightState {
	float hs_z1 = 0.f, hs_z2 = 0.f;
	float hp_z1 = 0.f, hp_z2 = 0.f;
};

static void
kweight_reset (KWeightState& s)
{
	s.hs_z1 = s.hs_z2 = s.hp_z1 = s.hp_z2 = 0.f;
}

/* ITU-R BS.1770 stage 1+2 coefficients at 48 kHz (applied for all rates as approx). */
static float
kweight_sample (KWeightState& s, float x)
{
	/* High shelf */
	const float hs_b0 = 1.53512485958697f;
	const float hs_b1 = -2.69169618940638f;
	const float hs_b2 = 1.19839281085285f;
	const float hs_a1 = -1.69065929318241f;
	const float hs_a2 = 0.73248077421585f;
	float hs_y = hs_b0 * x + s.hs_z1;
	s.hs_z1 = hs_b1 * x - hs_a1 * hs_y + s.hs_z2;
	s.hs_z2 = hs_b2 * x - hs_a2 * hs_y;

	/* Highpass */
	const float hp_b0 = 1.0f;
	const float hp_b1 = -2.0f;
	const float hp_b2 = 1.0f;
	const float hp_a1 = -1.99004745483398f;
	const float hp_a2 = 0.99007225036621f;
	float hp_y = hp_b0 * hs_y + s.hp_z1;
	s.hp_z1 = hp_b1 * hs_y - hp_a1 * hp_y + s.hp_z2;
	s.hp_z2 = hp_b2 * hs_y - hp_a2 * hp_y;
	return hp_y;
}

static float
mean_square_to_lufs (double mean_sq)
{
	if (!(mean_sq > 1.0e-20)) {
		return -120.f;
	}
	return (float)(-0.691 + 10.0 * std::log10 (mean_sq));
}

static float
kweight_stereo_lufs (const float* L, const float* R, uint32_t n)
{
	if (!L || !R || n < 64) {
		return -120.f;
	}
	KWeightState sl, sr;
	kweight_reset (sl);
	kweight_reset (sr);
	double acc = 0.0;
	for (uint32_t i = 0; i < n; ++i) {
		float yl = kweight_sample (sl, L[i]);
		float yr = kweight_sample (sr, R[i]);
		acc += (double)yl * (double)yl + (double)yr * (double)yr;
	}
	/* Stereo channel weighting G = 1.0 for L/R (BS.1770). */
	return mean_square_to_lufs (acc / (double)n);
}

void
EngineHost::refresh_loudness_snap ()
{
	if (!_snap.loudness_enabled) {
		_snap.loudness_ok = false;
		return;
	}
	if (!_impl || !_impl->session) {
		_snap.loudness_ok = false;
		_snap.loudness_momentary = _snap.loudness_momentary * 0.9f - 7.f * 0.1f;
		return;
	}

	static float left[kLufsWin];
	static float right[kLufsWin];
	static float mixdown[kLufsWin];
	static float gainbuf[kLufsWin];
	static float integ_blocks[64];
	static int   integ_n = 0;
	static int   integ_i = 0;
	static bool  integ_init = false;
	if (!integ_init) {
		for (int i = 0; i < 64; ++i) {
			integ_blocks[i] = -120.f;
		}
		integ_init = true;
	}

	samplecnt_t sr = _impl->session->sample_rate ();
	if (sr <= 0) {
		sr = 48000;
	}
	uint32_t n = (uint32_t)std::min ((samplecnt_t)kLufsWin,
	                                 (samplecnt_t)std::max ((samplecnt_t)2048, sr / 2));
	/* Prefer ~400 ms when rate known. */
	samplecnt_t want = (samplecnt_t)((double)sr * 0.4);
	if (want < 2048) {
		want = 2048;
	}
	if (want > (samplecnt_t)kLufsWin) {
		want = (samplecnt_t)kLufsWin;
	}
	n = (uint32_t)want;

	std::memset (left, 0, sizeof (float) * n);
	std::memset (right, 0, sizeof (float) * n);

	samplepos_t pos = _impl->session->transport_sample ();
	if (pos < (samplepos_t)n) {
		pos = 0;
	} else {
		pos = pos - (samplepos_t)n;
	}

	bool got = false;
	try {
		collect_tracks ();
		if (_snap.loudness_master) {
			_snap.loudness_track = -1;
			wcsncpy_s (_snap.loudness_label, L"Master ≈LUFS", _TRUNCATE);
			float scale = 1.f;
			int ntr = (int)_impl->tracks.size ();
			if (ntr > 1) {
				scale = 1.f / std::sqrt ((float)ntr);
			}
			for (size_t t = 0; t < _impl->tracks.size (); ++t) {
				std::shared_ptr<AudioTrack> at = _impl->tracks[t];
				if (!at || !at->playlist ()) {
					continue;
				}
				std::shared_ptr<AudioPlaylist> apl =
				    std::dynamic_pointer_cast<AudioPlaylist> (at->playlist ());
				if (!apl) {
					continue;
				}
				static float dst_l[kLufsWin];
				static float dst_r[kLufsWin];
				std::memset (dst_l, 0, sizeof (float) * n);
				std::memset (dst_r, 0, sizeof (float) * n);
				std::memset (mixdown, 0, sizeof (float) * n);
				std::memset (gainbuf, 0, sizeof (float) * n);
				apl->read (dst_l, mixdown, gainbuf,
				           timepos_t (pos), timecnt_t ((samplecnt_t)n), 0);
				std::memset (mixdown, 0, sizeof (float) * n);
				std::memset (gainbuf, 0, sizeof (float) * n);
				apl->read (dst_r, mixdown, gainbuf,
				           timepos_t (pos), timecnt_t ((samplecnt_t)n), 1);
				float er = 0.f;
				for (uint32_t i = 0; i < n; ++i) {
					er += std::fabs (dst_r[i]);
				}
				if (er < 1.0e-6f) {
					std::memcpy (dst_r, dst_l, sizeof (float) * n);
				}
				for (uint32_t i = 0; i < n; ++i) {
					left[i] += dst_l[i] * scale;
					right[i] += dst_r[i] * scale;
				}
				got = true;
			}
		} else {
			int tr = _snap.selection_track;
			if (tr < 0 || tr >= (int)_impl->tracks.size () || !_impl->tracks[tr]) {
				tr = 0;
			}
			_snap.loudness_track = tr;
			if (tr < EngineSnapshot::kMeters && _snap.track_name[tr][0]) {
				wcsncpy_s (_snap.loudness_label, _snap.track_name[tr], _TRUNCATE);
			} else {
				wchar_t lab[32];
				std::swprintf (lab, 32, L"Audio %d ≈", tr + 1);
				wcsncpy_s (_snap.loudness_label, lab, _TRUNCATE);
			}
			if (tr < (int)_impl->tracks.size () && _impl->tracks[tr]
			    && _impl->tracks[tr]->playlist ()) {
				std::shared_ptr<AudioPlaylist> apl =
				    std::dynamic_pointer_cast<AudioPlaylist> (
				        _impl->tracks[tr]->playlist ());
				if (apl) {
					std::memset (mixdown, 0, sizeof (float) * n);
					std::memset (gainbuf, 0, sizeof (float) * n);
					apl->read (left, mixdown, gainbuf,
					           timepos_t (pos), timecnt_t ((samplecnt_t)n), 0);
					std::memset (mixdown, 0, sizeof (float) * n);
					std::memset (gainbuf, 0, sizeof (float) * n);
					apl->read (right, mixdown, gainbuf,
					           timepos_t (pos), timecnt_t ((samplecnt_t)n), 1);
					float er = 0.f;
					for (uint32_t i = 0; i < n; ++i) {
						er += std::fabs (right[i]);
					}
					if (er < 1.0e-6f) {
						std::memcpy (right, left, sizeof (float) * n);
					}
					got = true;
				}
			}
		}
	} catch (...) {
		got = false;
	}

	float m = kweight_stereo_lufs (left, right, n);
	if (!got) {
		_snap.loudness_momentary = _snap.loudness_momentary * 0.9f - 7.f * 0.1f;
		_snap.loudness_ok = false;
		return;
	}
	_snap.loudness_momentary = _snap.loudness_momentary * 0.4f + m * 0.6f;

	/* Absolute gate −70 LUFS; keep recent gated blocks for integrated approx. */
	if (m > -70.f) {
		integ_blocks[integ_i] = m;
		integ_i = (integ_i + 1) % 64;
		if (integ_n < 64) {
			++integ_n;
		}
	}
	if (integ_n > 0) {
		double sum = 0.0;
		int used = 0;
		/* Relative gate: discard blocks below ungated mean − 10 LU. */
		double ungated = 0.0;
		for (int i = 0; i < integ_n; ++i) {
			ungated += (double)integ_blocks[i];
		}
		ungated /= (double)integ_n;
		const float rel = (float)ungated - 10.f;
		for (int i = 0; i < integ_n; ++i) {
			if (integ_blocks[i] >= rel) {
				/* Convert back via energy average of LUFS blocks (approx). */
				sum += std::pow (10.0, ((double)integ_blocks[i] + 0.691) / 10.0);
				++used;
			}
		}
		if (used > 0) {
			_snap.loudness_integrated = mean_square_to_lufs (sum / (double)used);
		}
	}
	_snap.loudness_ok = true;
}

bool
EngineHost::set_loudness_enabled (bool on)
{
	_snap.loudness_enabled = on;
	if (!on) {
		_snap.loudness_ok = false;
		_snap.loudness_momentary = -70.f;
		_snap.loudness_integrated = -70.f;
		_snap.detail = L"loudness off";
		return true;
	}
	refresh_loudness_snap ();
	_snap.detail = L"loudness on (approx LUFS)";
	return true;
}

bool
EngineHost::toggle_loudness_enabled ()
{
	return set_loudness_enabled (!_snap.loudness_enabled);
}

bool
EngineHost::set_loudness_master (bool master)
{
	_snap.loudness_master = master;
	if (_snap.loudness_enabled) {
		refresh_loudness_snap ();
	}
	return true;
}

bool
EngineHost::toggle_loudness_master ()
{
	return set_loudness_master (!_snap.loudness_master);
}

bool
EngineHost::smoke_loudness ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-loudness failed — no Session";
		return false;
	}
	try {
		if (!set_loudness_enabled (true) || !_snap.loudness_enabled) {
			std::fprintf (stderr, "airdour_win_shell: smoke-loudness FAILED (enable)\n");
			std::fflush (stderr);
			return false;
		}

		static float L[kLufsWin];
		static float R[kLufsWin];
		const uint32_t n = 4800; /* 100 ms @ 48 kHz */
		for (uint32_t i = 0; i < n; ++i) {
			/* −20 dBFS 1 kHz stereo sine → expect roughly −23 LUFS-ish. */
			float s = 0.1f * std::sin (2.f * 3.14159265f * 1000.f * (float)i / 48000.f);
			L[i] = s;
			R[i] = s;
		}
		float m = kweight_stereo_lufs (L, R, n);
		if (!(m > -40.f) || !(m < -10.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-loudness FAILED (M=%.2f)\n", m);
			std::fflush (stderr);
			return false;
		}
		_snap.loudness_momentary = m;
		_snap.loudness_integrated = m;
		set_loudness_master (false);
		refresh_loudness_snap ();
		set_loudness_master (true);
		refresh_loudness_snap ();
		set_loudness_master (false);

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loudness ok (M=%.1f approx K-weight)\n",
		              m);
		std::fflush (stderr);
		_snap.detail = L"smoke-loudness ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-loudness exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}


bool
EngineHost::smoke_perf (unsigned sample_secs)
{
	if (!_impl || !_impl->engine || !_started) {
		std::fprintf (stderr, "airdour_win_shell: smoke-perf FAILED (no engine)\n");
		std::fflush (stderr);
		return false;
	}
	if (sample_secs < 1) {
		sample_secs = 1;
	}
	if (sample_secs > 30) {
		sample_secs = 30;
	}

	/* Do not transport_play here — Debug Session roll during attach_engine has
	 * intermittent AVs. WASAPI shared still runs process cycles while stopped. */
	PROCESS_MEMORY_COUNTERS_EX pmc0;
	std::memset (&pmc0, 0, sizeof (pmc0));
	pmc0.cb = sizeof (pmc0);
	GetProcessMemoryInfo (GetCurrentProcess (), (PROCESS_MEMORY_COUNTERS*)&pmc0, sizeof (pmc0));

	const DWORD t0 = GetTickCount ();
	float dsp_sum = 0.f;
	float dsp_max = 0.f;
	unsigned dsp_n = 0;
	SIZE_T ws_peak = pmc0.WorkingSetSize;
	uint64_t xruns0 = 0;

	refresh_snapshot ();
	xruns0 = _snap.xrun_count;
	const uint32_t period0 = _snap.period_frames;
	const uint32_t sr0 = _snap.sample_rate;

	while ((GetTickCount () - t0) < sample_secs * 1000u) {
		MSG msg;
		while (PeekMessageW (&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage (&msg);
			DispatchMessageW (&msg);
		}
		Sleep (50);
		refresh_snapshot ();
		dsp_sum += _snap.dsp_load_pct;
		if (_snap.dsp_load_pct > dsp_max) {
			dsp_max = _snap.dsp_load_pct;
		}
		++dsp_n;
		PROCESS_MEMORY_COUNTERS_EX pmc;
		std::memset (&pmc, 0, sizeof (pmc));
		pmc.cb = sizeof (pmc);
		if (GetProcessMemoryInfo (GetCurrentProcess (), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof (pmc))) {
			if (pmc.WorkingSetSize > ws_peak) {
				ws_peak = pmc.WorkingSetSize;
			}
		}
	}

	refresh_snapshot ();

	const double period_ms = (sr0 > 0 && period0 > 0)
		? (1000.0 * (double)period0 / (double)sr0)
		: 0.0;
	const double period_us = period_ms * 1000.0;
	const float dsp_avg = (dsp_n > 0) ? (dsp_sum / (float)dsp_n) : 0.f;
	const uint64_t xruns = (_snap.xrun_count >= xruns0) ? (_snap.xrun_count - xruns0) : _snap.xrun_count;
	const double headroom = (period_us > 0.0 && _snap.callback_max_us > 0)
		? (100.0 * (1.0 - (double)_snap.callback_max_us / period_us))
		: (100.0 - (double)dsp_max);

	std::fprintf (stderr,
	              "airdour_win_shell: PERF backend=%ls sr=%u period=%u (%.2f ms) "
	              "cb_us last=%llu avg=%llu max=%llu / period_us=%.0f "
	              "dsp_pct avg=%.1f max=%.1f headroom_pct≈%.1f xruns=%llu "
	              "ws_peak_mb=%.1f cycles=%llu\n",
	              _snap.backend.c_str (),
	              (unsigned)sr0,
	              (unsigned)period0,
	              period_ms,
	              (unsigned long long)_snap.callback_last_us,
	              (unsigned long long)_snap.callback_avg_us,
	              (unsigned long long)_snap.callback_max_us,
	              period_us,
	              dsp_avg,
	              dsp_max,
	              headroom,
	              (unsigned long long)xruns,
	              (double)ws_peak / (1024.0 * 1024.0),
	              (unsigned long long)_snap.callback_count);
	std::fflush (stderr);
	_snap.detail = L"smoke-perf ok";
	return true;
}

bool
EngineHost::smoke_plugin_insert ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-plugin failed — no Session";
		return false;
	}
	try {
		if (_impl->tracks.empty ()) {
			collect_tracks ();
		}
		if (_impl->tracks.empty () || !_impl->tracks[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-plugin FAILED (no Audio 1)\n");
			std::fflush (stderr);
			return false;
		}
		if (!insert_track_fx (0) || !_snap.track_fx_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-plugin FAILED (insert)\n");
			std::fflush (stderr);
			return false;
		}
		std::string verified;
		if (!track_has_ace_reverb (_impl->tracks[0], &verified)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-plugin FAILED (not on route)\n");
			std::fflush (stderr);
			return false;
		}

		/* Bypass off → on → off */
		if (!set_track_fx_bypassed (0, false) || _snap.track_fx_bypassed) {
			std::fprintf (stderr, "airdour_win_shell: smoke-plugin FAILED (bypass clear)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_track_fx_bypassed (0, true) || !_snap.track_fx_bypassed) {
			std::fprintf (stderr, "airdour_win_shell: smoke-plugin FAILED (bypass set)\n");
			std::fflush (stderr);
			return false;
		}
		if (!set_track_fx_bypassed (0, false) || _snap.track_fx_bypassed) {
			std::fprintf (stderr, "airdour_win_shell: smoke-plugin FAILED (bypass restore)\n");
			std::fflush (stderr);
			return false;
		}

		/* Blend param write-through */
		const float want = 0.72f;
		if (!set_track_fx_param (0, want)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-plugin FAILED (param set)\n");
			std::fflush (stderr);
			return false;
		}
		float got = _snap.track_fx_param;
		if (got < want - 0.05f || got > want + 0.05f) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-plugin FAILED (param got=%.3f want=%.3f)\n",
			              got, want);
			std::fflush (stderr);
			return false;
		}

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-plugin ok (fx=%s on_route=1 bypass=0→1→0 "
		              "blend=%.2f)\n",
		              verified.c_str (), got);
		std::fflush (stderr);
		_snap.detail = L"smoke-plugin ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-plugin exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_vst3 ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"smoke-vst3 failed — no Session";
		return false;
	}
	try {
		if (!scan_vst3_fresh ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-vst3 FAILED (scan)\n");
			std::fflush (stderr);
			return false;
		}

		const size_t n = PluginManager::instance ().vst3_plugin_info ().size ();
		PluginInfoPtr info = find_smoke_vst3_info ();
		if (!info) {
			/* Honest success when no VST3 is installed — scan path compiled + ran. */
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3 ok (scan-only VST3=%zu; "
			              "set VST3_PATH or AIRDOUR_SMOKE_VST3 to insert)\n",
			              n);
			std::fflush (stderr);
			_snap.detail = L"smoke-vst3 scan-only";
			return true;
		}

		if (_impl->tracks.size () < 2 || !_impl->tracks[1]) {
			collect_tracks ();
		}
		if (_impl->tracks.size () < 2 || !_impl->tracks[1]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-vst3 FAILED (no Audio 2)\n");
			std::fflush (stderr);
			return false;
		}

		std::shared_ptr<AudioTrack> at = _impl->tracks[1];
		PluginPtr plugin = info->load (*_impl->session);
		if (!plugin) {
			std::fprintf (stderr, "airdour_win_shell: smoke-vst3 FAILED (load %s)\n",
			              info->name.c_str ());
			std::fflush (stderr);
			return false;
		}

		std::shared_ptr<PluginInsert> pi (
		    new PluginInsert (*_impl->session, *at, plugin));
		pi->set_strict_io (true);
		if (at->add_processor (pi, PreFader) != 0) {
			std::fprintf (stderr, "airdour_win_shell: smoke-vst3 FAILED (add_processor)\n");
			std::fflush (stderr);
			return false;
		}

		/* Confirm on route + bypass toggle (DSP-only; no VST3 UI). */
		bool on_route = false;
		at->foreach_processor ([&] (std::weak_ptr<Processor> wp) {
			std::shared_ptr<PluginInsert> p = std::dynamic_pointer_cast<PluginInsert> (wp.lock ());
			if (p && p == pi) {
				on_route = true;
			}
		});
		if (!on_route) {
			std::fprintf (stderr, "airdour_win_shell: smoke-vst3 FAILED (not on route)\n");
			std::fflush (stderr);
			return false;
		}

		pi->deactivate ();
		if (pi->active ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-vst3 FAILED (bypass)\n");
			std::fflush (stderr);
			return false;
		}
		pi->activate ();
		if (!pi->active ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-vst3 FAILED (activate)\n");
			std::fflush (stderr);
			return false;
		}

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3 ok (fx=%s on Audio 2, "
		              "scan=%zu, bypass ok, no GTK UI)\n",
		              info->name.c_str (), n);
		std::fflush (stderr);
		_snap.detail = L"smoke-vst3 ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vst3 exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vst3 FAILED (exception)\n");
		std::fflush (stderr);
		return false;
	}
}

bool
EngineHost::ensure_midi_track ()
{
	if (!_impl || !_impl->session) {
		return false;
	}
	if (_impl->midi_track) {
		attach_midi_synth ();
		refresh_midi_snap ();
		return true;
	}
	try {
		ensure_plugins_scanned ();
		PluginInfoPtr instrument = find_reasonable_synth_info ();

		ChanCount midi_io (DataType::MIDI, 1);
		/* With an instrument, request audio outs as well so routing works. */
		ChanCount outs = midi_io;
		if (instrument) {
			outs.set (DataType::AUDIO, 2);
		}
		std::list<std::shared_ptr<MidiTrack> > created = _impl->session->new_midi_track (
			midi_io,
			outs,
			true /*strict_io*/,
			instrument,
			(Plugin::PresetRecord*)0,
			std::shared_ptr<RouteGroup> (),
			1,
			"MIDI",
			PresentationInfo::max_order,
			Normal,
			true /*input_auto_connect*/,
			false);
		if (created.empty () || !created.front ()) {
			_snap.detail = L"MIDI track create failed";
			return false;
		}
		_impl->midi_track = created.front ();
		if (_impl->midi_track->the_instrument ()) {
			_impl->midi_synth_ok = true;
			_impl->midi_synth_name = _impl->midi_track->the_instrument ()->name ();
			_impl->midi_synth_to_master =
			    _impl->midi_track->output ()
			    && _impl->midi_track->output ()->n_ports ().n_audio () > 0;
			std::fprintf (stderr,
			              "airdour_win_shell: MIDI track+synth \"%s\" / \"%s\"\n",
			              _impl->midi_track->name ().c_str (),
			              _impl->midi_synth_name.c_str ());
		} else {
			std::fprintf (stderr, "airdour_win_shell: created MIDI track \"%s\" (no synth yet)\n",
			              _impl->midi_track->name ().c_str ());
			attach_midi_synth ();
		}
		std::fflush (stderr);
		/* Do not connect WinMME MIDI out here — SMF WriterLock vs reader deadlock
		 * during note edits. Soft-synth audio goes through Dummy/WASAPI master. */
		refresh_midi_snap ();
		_snap.detail = L"MIDI track ready";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: MIDI track exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"MIDI track exception";
		return false;
	} catch (...) {
		_snap.detail = L"MIDI track unknown failure";
		return false;
	}
}

bool
EngineHost::ensure_midi_region (float length_sec)
{
	if (!ensure_midi_track () || !_impl || !_impl->session || !_impl->midi_track) {
		return false;
	}
	if (_impl->midi_region) {
		refresh_midi_notes_snap ();
		return true;
	}
	try {
		std::shared_ptr<Playlist> playlist = _impl->midi_track->playlist ();
		if (!playlist) {
			_snap.detail = L"MIDI playlist missing";
			return false;
		}
		/* Reuse an existing region if playlist already has one. */
		playlist->foreach_region ([&] (std::shared_ptr<Region> r) {
			if (_impl->midi_region) {
				return;
			}
			std::shared_ptr<MidiRegion> mr = std::dynamic_pointer_cast<MidiRegion> (r);
			if (mr) {
				_impl->midi_region = mr;
			}
		});
		if (_impl->midi_region) {
			refresh_midi_notes_snap ();
			return true;
		}

		double sr = (double)_impl->engine->sample_rate ();
		if (sr <= 0.0) {
			sr = 48000.0;
		}
		if (length_sec < 1.f) {
			length_sec = 32.f;
		}
		samplepos_t start_sample = 0;
		samplepos_t end_sample = (samplepos_t)(length_sec * sr);
		if (end_sample <= start_sample) {
			end_sample = start_sample + (samplepos_t)sr;
		}

		std::shared_ptr<Track> track =
		    std::dynamic_pointer_cast<Track> (_impl->midi_track);
		std::shared_ptr<MidiSource> midi_src =
		    _impl->session->create_midi_source_by_stealing_name (track);
		if (!midi_src) {
			_snap.detail = L"MIDI source create failed";
			return false;
		}

		SourceList srcs;
		srcs.push_back (std::dynamic_pointer_cast<Source> (midi_src));
		const Temporal::timecnt_t source_start (Temporal::BeatTime);
		const Temporal::timepos_t start_pos (start_sample);
		const Temporal::timepos_t end_pos (end_sample);
		const Temporal::timecnt_t region_length = start_pos.distance (end_pos);

		PropertyList whole_file_props;
		whole_file_props.add (Properties::start, source_start);
		whole_file_props.add (Properties::length, region_length);
		whole_file_props.add (Properties::automatic, true);
		whole_file_props.add (Properties::whole_file, true);
		whole_file_props.add (Properties::name, midi_src->name ());
		whole_file_props.add (Properties::opaque,
		                      _impl->session->config.get_draw_opaque_midi_regions ());

		std::shared_ptr<Region> whole_file_region =
		    RegionFactory::create (srcs, whole_file_props);
		if (!whole_file_region) {
			_snap.detail = L"MIDI whole-file region failed";
			return false;
		}

		PropertyList playlist_region_props;
		playlist_region_props.add (Properties::name, whole_file_region->name ());
		std::shared_ptr<Region> region =
		    RegionFactory::create (whole_file_region, playlist_region_props);
		if (!region) {
			_snap.detail = L"MIDI playlist region failed";
			return false;
		}

		_impl->session->begin_reversible_command ("add midi region");
		playlist->clear_changes ();
		playlist->clear_owned_changes ();
		region->set_position (start_pos);
		playlist->add_region (region, start_pos, 1.0, false);
		playlist->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();

		_impl->midi_region = std::dynamic_pointer_cast<MidiRegion> (region);
		if (!_impl->midi_region) {
			_snap.detail = L"MIDI region cast failed";
			return false;
		}
		refresh_midi_notes_snap ();
		std::fprintf (stderr, "airdour_win_shell: MIDI region ready (%.1fs)\n", length_sec);
		std::fflush (stderr);
		_snap.detail = L"MIDI region ready";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: MIDI region exception: %s\n", e.what ());
		std::fflush (stderr);
		_snap.detail = L"MIDI region exception";
		return false;
	} catch (...) {
		_snap.detail = L"MIDI region unknown failure";
		return false;
	}
}

bool
EngineHost::add_midi_note (uint8_t pitch, float start_sec, float length_sec, uint8_t velocity)
{
	if (!ensure_midi_region () || !_impl || !_impl->session || !_impl->midi_region) {
		return false;
	}
	if (pitch > 127) {
		pitch = 127;
	}
	if (velocity < 1) {
		velocity = 100;
	}
	if (velocity > 127) {
		velocity = 127;
	}
	if (length_sec < 0.05f) {
		length_sec = 0.25f;
	}
	if (start_sec < 0.f) {
		start_sec = 0.f;
	}
	try {
		std::shared_ptr<MidiModel> model = _impl->midi_region->model ();
		if (!model) {
			return false;
		}
		/* Fixed 120 BPM: 1 beat = 0.5s. Avoids TempoMap during UI edits. */
		const double beats_per_sec = 120.0 / 60.0;
		Temporal::Beats start_beats =
		    Temporal::Beats::from_double ((double)start_sec * beats_per_sec);
		Temporal::Beats length_beats =
		    Temporal::Beats::from_double ((double)length_sec * beats_per_sec);
		if (length_beats < Temporal::Beats::one_tick ()) {
			length_beats = Temporal::Beats::from_double (0.5);
		}
		if (start_beats < Temporal::Beats ()) {
			start_beats = Temporal::Beats ();
		}

		std::shared_ptr<Evoral::Note<Temporal::Beats> > note (
		    new Evoral::Note<Temporal::Beats> (
		        0 /*channel*/,
		        start_beats,
		        length_beats,
		        pitch,
		        velocity));

		MidiModel::NoteDiffCommand* cmd =
		    model->new_note_diff_command ("add midi note");
		cmd->add (note);
		{
			/* Serialize vs process-thread Source::ReaderLock (avoids WriterLock hang). */
			PBD::Mutex::Lock lx (_impl->engine->process_lock ());
			model->apply_diff_command_as_commit (*_impl->session, cmd);
		}

		std::shared_ptr<Evoral::Note<Temporal::Beats> > inserted =
		    model->find_note (note->id ());
		if (!inserted) {
			/* Overlap policy may reject; still report failure. */
			_snap.detail = L"MIDI note rejected (overlap)";
			refresh_midi_notes_snap ();
			return false;
		}
		_impl->selected_midi_note_id = inserted->id ();
		_impl->midi_activity = 1.f;
		refresh_midi_notes_snap ();
		_snap.detail = L"MIDI note added";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: add_midi_note: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::select_midi_note_at (float time_sec, uint8_t pitch, float pitch_slop)
{
	(void)pitch_slop;
	refresh_midi_notes_snap ();
	if (!_impl) {
		return false;
	}
	int best = -1;
	float best_dist = 1e9f;
	for (int i = 0; i < _snap.midi_note_count; ++i) {
		const EngineMidiNoteSnap& n = _snap.midi_notes[i];
		if (n.pitch != pitch) {
			continue;
		}
		float t0 = n.start_sec;
		float t1 = n.start_sec + n.length_sec;
		if (time_sec < t0 - 0.02f || time_sec > t1 + 0.02f) {
			continue;
		}
		float mid = 0.5f * (t0 + t1);
		float d = std::fabs (time_sec - mid);
		if (d < best_dist) {
			best_dist = d;
			best = i;
		}
	}
	if (best < 0) {
		_impl->selected_midi_note_id = -1;
		refresh_midi_notes_snap ();
		return false;
	}
	_impl->selected_midi_note_id = _snap.midi_notes[best].id;
	refresh_midi_notes_snap ();
	return true;
}

void
EngineHost::clear_midi_note_selection ()
{
	if (_impl) {
		_impl->selected_midi_note_id = -1;
	}
	refresh_midi_notes_snap ();
}

bool
EngineHost::delete_selected_midi_note ()
{
	if (!_impl || !_impl->session || !_impl->midi_region
	    || _impl->selected_midi_note_id < 0) {
		return false;
	}
	try {
		/* Diff-command remove can WriterLock-deadlock against SMF readers on
		 * this Windows path. Rebuild the region from remaining notes instead. */
		refresh_midi_notes_snap ();
		struct Kept {
			uint8_t pitch;
			uint8_t velocity;
			float   start_sec;
			float   length_sec;
		};
		std::vector<Kept> keep;
		keep.reserve ((size_t)_snap.midi_note_count);
		const int32_t doomed = _impl->selected_midi_note_id;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			if (n.id == doomed) {
				continue;
			}
			Kept k;
			k.pitch = n.pitch;
			k.velocity = n.velocity;
			k.start_sec = n.start_sec;
			k.length_sec = n.length_sec;
			keep.push_back (k);
		}

		std::shared_ptr<Playlist> playlist = _impl->midi_track
		                                        ? _impl->midi_track->playlist ()
		                                        : std::shared_ptr<Playlist> ();
		if (!playlist) {
			return false;
		}
		_impl->session->begin_reversible_command ("delete midi note");
		playlist->clear_changes ();
		playlist->clear_owned_changes ();
		playlist->remove_region (_impl->midi_region);
		playlist->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();

		_impl->midi_region.reset ();
		_impl->selected_midi_note_id = -1;
		if (!ensure_midi_region (32.f)) {
			_snap.detail = L"MIDI note delete: region rebuild failed";
			return false;
		}
		for (size_t i = 0; i < keep.size (); ++i) {
			if (!add_midi_note (keep[i].pitch, keep[i].start_sec,
			                    keep[i].length_sec, keep[i].velocity)) {
				std::fprintf (stderr, "airdour_win_shell: re-add note %zu failed\n", i);
				std::fflush (stderr);
			}
		}
		_impl->selected_midi_note_id = -1;
		refresh_midi_notes_snap ();
		_snap.detail = L"MIDI note deleted";
		std::fprintf (stderr, "airdour_win_shell: delete note via rebuild (kept=%zu)\n",
		              keep.size ());
		std::fflush (stderr);
		return true;
	} catch (...) {
		std::fprintf (stderr, "airdour_win_shell: delete note exception\n");
		std::fflush (stderr);
		return false;
	}
}

bool
EngineHost::smoke_midi ()
{
	if (!_impl || !_impl->session || !_impl->engine) {
		_snap.detail = L"smoke-midi failed — no Session";
		return false;
	}
	try {
		refresh_midi_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi option=%ls devices=%d midi_ok=%d\n",
		              _snap.midi_option.c_str (),
		              _snap.midi_device_count,
		              _snap.midi_ok ? 1 : 0);
		for (int i = 0; i < _snap.midi_device_count; ++i) {
			std::fprintf (stderr, "airdour_win_shell:   midi[%d] %ls (%s)\n",
			              i, _snap.midi_devices[i].name,
			              _snap.midi_devices[i].input ? "in?" : "out?");
		}
		std::fflush (stderr);

		if (!ensure_midi_track () || !_snap.midi_track_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi FAILED (track)\n");
			std::fflush (stderr);
			return false;
		}
		connect_midi_output (); /* best-effort; ok if none */
		refresh_midi_snap ();

		/* Dummy/WinMME path is ok even with 0 hardware devices (enumerate worked). */
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi ok (track=1 option=%ls devices=%d out=%ls)\n",
		              _snap.midi_option.c_str (), _snap.midi_device_count,
		              _snap.midi_out_connected ? _snap.midi_out_name.c_str () : L"(none)");
		std::fflush (stderr);
		_snap.detail = L"smoke-midi ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_midi_edit ()
{
	if (!_impl || !_impl->session || !_impl->engine) {
		_snap.detail = L"smoke-midi-edit failed — no Session";
		return false;
	}
	try {
		/* Enumerate + track only — do NOT connect MIDI out until after edits
		 * (SMF WriterLock vs process ReaderLock deadlock under Dummy/WinMME). */
		refresh_midi_snap ();
		if (!ensure_midi_track () || !_snap.midi_track_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-edit FAILED (track)\n");
			std::fflush (stderr);
			return false;
		}
		if (!ensure_midi_region (16.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-edit FAILED (region)\n");
			std::fflush (stderr);
			return false;
		}
		/* Clear any leftover selection; add a small motif. */
		clear_midi_note_selection ();
		const int before = _snap.midi_note_count;
		bool a = add_midi_note (60, 0.0f, 0.5f, 100); /* C4 */
		bool b = add_midi_note (64, 0.5f, 0.5f, 100); /* E4 */
		bool c = add_midi_note (67, 1.0f, 0.75f, 110); /* G4 */
		refresh_midi_notes_snap ();
		const int after_add = _snap.midi_note_count;
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-edit notes before=%d after_add=%d "
		              "(a=%d b=%d c=%d) region=%d out=%d\n",
		              before, after_add, a ? 1 : 0, b ? 1 : 0, c ? 1 : 0,
		              _snap.midi_region_ok ? 1 : 0,
		              _snap.midi_out_connected ? 1 : 0);
		std::fflush (stderr);
		if (after_add < before + 2) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-edit FAILED (add)\n");
			std::fflush (stderr);
			return false;
		}
		/* Select middle note and delete it. */
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-edit selecting…\n");
		std::fflush (stderr);
		if (!select_midi_note_at (0.75f, 64)) {
			/* Fallback: select first snap note. */
			if (_snap.midi_note_count > 0) {
				_impl->selected_midi_note_id = _snap.midi_notes[0].id;
				refresh_midi_notes_snap ();
			}
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-edit selected id=%d\n",
		              (int)_impl->selected_midi_note_id);
		std::fflush (stderr);
		const int before_del = _snap.midi_note_count;
		bool deleted = delete_selected_midi_note ();
		refresh_midi_notes_snap ();
		const int after_del = _snap.midi_note_count;
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-edit delete=%d count %d→%d\n",
		              deleted ? 1 : 0, before_del, after_del);
		std::fflush (stderr);
		if (!deleted || after_del != before_del - 1 || after_del < 1) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-edit FAILED (delete)\n");
			std::fflush (stderr);
			return false;
		}
		/* Soft-synth must be in-graph for Dummy/WASAPI audible path (no MIDI HW). */
		if (!attach_midi_synth () || !_snap.midi_synth_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-edit FAILED (synth)\n");
			std::fflush (stderr);
			return false;
		}
		/* Optional WinMME physical out — not required for hearing via soft-synth. */
		connect_midi_output ();
		refresh_midi_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-edit ok (notes=%d synth=%ls master=%d out=%ls)\n",
		              after_del,
		              _snap.midi_synth_name.empty () ? L"(none)" : _snap.midi_synth_name.c_str (),
		              _snap.midi_synth_to_master ? 1 : 0,
		              _snap.midi_out_connected ? _snap.midi_out_name.c_str () : L"(none)");
		std::fflush (stderr);
		_snap.detail = L"smoke-midi-edit ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-edit exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}


bool
EngineHost::quantize_midi_notes ()
{
	if (!_impl || !_impl->session) {
		_snap.detail = L"quantize: no Session";
		return false;
	}
	if (!ensure_midi_region (32.f) || !_impl->midi_region) {
		_snap.detail = L"quantize: no MIDI region";
		return false;
	}
	try {
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count <= 0) {
			_snap.detail = L"quantize: no notes";
			return false;
		}

		bool have_sel = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].selected) {
				have_sel = true;
				break;
			}
		}

		struct Kept {
			uint8_t pitch;
			uint8_t velocity;
			float   start_sec;
			float   length_sec;
		};
		std::vector<Kept> keep;
		keep.reserve ((size_t)_snap.midi_note_count);

		const bool was_snap = _snap.snap_enabled;
		_snap.snap_enabled = true;

		int moved = 0;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			Kept k;
			k.pitch = n.pitch;
			k.velocity = n.velocity;
			k.length_sec = n.length_sec;
			const bool do_q = !have_sel || n.selected;
			if (do_q) {
				float q = snap_time (n.start_sec);
				if (q < 0.f) {
					q = 0.f;
				}
				if (std::fabs (q - n.start_sec) > 1.0e-4f) {
					++moved;
				}
				k.start_sec = q;
			} else {
				k.start_sec = n.start_sec;
			}
			keep.push_back (k);
		}
		_snap.snap_enabled = was_snap;

		std::shared_ptr<Playlist> playlist = _impl->midi_track
		                                        ? _impl->midi_track->playlist ()
		                                        : std::shared_ptr<Playlist> ();
		if (!playlist) {
			_snap.detail = L"quantize: no playlist";
			return false;
		}
		_impl->session->begin_reversible_command ("quantize midi");
		playlist->clear_changes ();
		playlist->clear_owned_changes ();
		playlist->remove_region (_impl->midi_region);
		playlist->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();

		_impl->midi_region.reset ();
		_impl->selected_midi_note_id = -1;
		if (!ensure_midi_region (32.f)) {
			_snap.detail = L"quantize: region rebuild failed";
			return false;
		}
		int added = 0;
		for (size_t i = 0; i < keep.size (); ++i) {
			if (add_midi_note (keep[i].pitch, keep[i].start_sec,
			                   keep[i].length_sec, keep[i].velocity)) {
				++added;
			} else {
				std::fprintf (stderr,
				              "airdour_win_shell: quantize re-add note %zu failed "
				              "(p=%u t=%.3f)\n",
				              i, (unsigned)keep[i].pitch, keep[i].start_sec);
				std::fflush (stderr);
			}
		}
		_impl->selected_midi_note_id = -1;
		refresh_midi_notes_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: quantize ok (notes=%d moved=%d added=%d dens=%d)\n",
		              _snap.midi_note_count, moved, added, _snap.snap_density);
		std::fflush (stderr);
		_snap.detail = L"MIDI quantized to snap grid";
		return added > 0 || keep.empty ();
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: quantize exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_midi_quantize ()
{
	if (!_impl || !_impl->session || !_impl->engine) {
		_snap.detail = L"smoke-midi-quantize failed — no Session";
		return false;
	}
	try {
		refresh_midi_snap ();
		if (!ensure_midi_track () || !_snap.midi_track_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-quantize FAILED (track)\n");
			std::fflush (stderr);
			return false;
		}
		if (!ensure_midi_region (16.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-quantize FAILED (region)\n");
			std::fflush (stderr);
			return false;
		}
		clear_midi_note_selection ();
		/* Wipe existing notes by rebuild empty. */
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count > 0) {
			std::shared_ptr<Playlist> playlist = _impl->midi_track
			                                        ? _impl->midi_track->playlist ()
			                                        : std::shared_ptr<Playlist> ();
			if (playlist && _impl->midi_region) {
				_impl->session->begin_reversible_command ("clear midi for quantize smoke");
				playlist->clear_changes ();
				playlist->clear_owned_changes ();
				playlist->remove_region (_impl->midi_region);
				playlist->rdiff_and_add_command (_impl->session);
				_impl->session->commit_reversible_command ();
				_impl->midi_region.reset ();
				_impl->selected_midi_note_id = -1;
				if (!ensure_midi_region (16.f)) {
					std::fprintf (stderr, "airdour_win_shell: smoke-midi-quantize FAILED (clear)\n");
					std::fflush (stderr);
					return false;
				}
			}
		}

		set_snap_density (1); /* beat */
		set_snap_enabled (true);

		/* Off-grid starts at fixed 120 BPM (0.5s/beat): 0.13→0.0, 0.63→0.5, 1.12→1.0 */
		bool a = add_midi_note (60, 0.13f, 0.25f, 100);
		bool b = add_midi_note (64, 0.63f, 0.25f, 100);
		bool c = add_midi_note (67, 1.12f, 0.25f, 100);
		refresh_midi_notes_snap ();
		if (!a || !b || !c || _snap.midi_note_count < 3) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-midi-quantize FAILED (add count=%d)\n",
			              _snap.midi_note_count);
			std::fflush (stderr);
			return false;
		}

		clear_midi_note_selection (); /* quantize ALL */
		if (!quantize_midi_notes ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-quantize FAILED (quantize)\n");
			std::fflush (stderr);
			return false;
		}
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count < 3) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-midi-quantize FAILED (after count=%d)\n",
			              _snap.midi_note_count);
			std::fflush (stderr);
			return false;
		}

		bool ok60 = false, ok64 = false, ok67 = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			if (n.pitch == 60 && std::fabs (n.start_sec - 0.0f) < 0.06f) {
				ok60 = true;
			}
			if (n.pitch == 64 && std::fabs (n.start_sec - 0.5f) < 0.06f) {
				ok64 = true;
			}
			if (n.pitch == 67 && std::fabs (n.start_sec - 1.0f) < 0.06f) {
				ok67 = true;
			}
		}
		if (!ok60 || !ok64 || !ok67) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-quantize FAILED (grid)\n");
			for (int i = 0; i < _snap.midi_note_count; ++i) {
				std::fprintf (stderr, "  note p=%u t=%.3f\n",
				              (unsigned)_snap.midi_notes[i].pitch,
				              _snap.midi_notes[i].start_sec);
			}
			std::fflush (stderr);
			return false;
		}

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-quantize ok (notes=%d dens=beat)\n",
		              _snap.midi_note_count);
		std::fflush (stderr);
		_snap.detail = L"smoke-midi-quantize ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-quantize exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}



bool
EngineHost::transpose_midi_notes (int semitones)
{
	if (semitones == 0) {
		_snap.detail = L"transpose: 0 st";
		return true;
	}
	if (!_impl || !_impl->session) {
		_snap.detail = L"transpose: no Session";
		return false;
	}
	if (!ensure_midi_region (32.f) || !_impl->midi_region) {
		_snap.detail = L"transpose: no MIDI region";
		return false;
	}
	try {
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count <= 0) {
			_snap.detail = L"transpose: no notes";
			return false;
		}

		bool have_sel = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].selected) {
				have_sel = true;
				break;
			}
		}

		struct Kept {
			uint8_t pitch;
			uint8_t velocity;
			float   start_sec;
			float   length_sec;
		};
		std::vector<Kept> keep;
		keep.reserve ((size_t)_snap.midi_note_count);

		int moved = 0;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			Kept k;
			k.velocity = n.velocity;
			k.start_sec = n.start_sec;
			k.length_sec = n.length_sec;
			const bool do_t = !have_sel || n.selected;
			if (do_t) {
				int p = (int)n.pitch + semitones;
				if (p < 0) {
					p = 0;
				}
				if (p > 127) {
					p = 127;
				}
				if (p != (int)n.pitch) {
					++moved;
				}
				k.pitch = (uint8_t)p;
			} else {
				k.pitch = n.pitch;
			}
			keep.push_back (k);
		}

		std::shared_ptr<Playlist> playlist = _impl->midi_track
		                                        ? _impl->midi_track->playlist ()
		                                        : std::shared_ptr<Playlist> ();
		if (!playlist) {
			_snap.detail = L"transpose: no playlist";
			return false;
		}
		_impl->session->begin_reversible_command ("transpose midi");
		playlist->clear_changes ();
		playlist->clear_owned_changes ();
		playlist->remove_region (_impl->midi_region);
		playlist->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();

		_impl->midi_region.reset ();
		_impl->selected_midi_note_id = -1;
		if (!ensure_midi_region (32.f)) {
			_snap.detail = L"transpose: region rebuild failed";
			return false;
		}
		int added = 0;
		for (size_t i = 0; i < keep.size (); ++i) {
			if (add_midi_note (keep[i].pitch, keep[i].start_sec,
			                   keep[i].length_sec, keep[i].velocity)) {
				++added;
			} else {
				std::fprintf (stderr,
				              "airdour_win_shell: transpose re-add note %zu failed "
				              "(p=%u t=%.3f)\n",
				              i, (unsigned)keep[i].pitch, keep[i].start_sec);
				std::fflush (stderr);
			}
		}
		_impl->selected_midi_note_id = -1;
		refresh_midi_notes_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: transpose ok (notes=%d moved=%d added=%d st=%+d)\n",
		              _snap.midi_note_count, moved, added, semitones);
		std::fflush (stderr);
		_snap.detail = L"MIDI transposed";
		return added > 0 || keep.empty ();
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: transpose exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_midi_transpose ()
{
	if (!_impl || !_impl->session || !_impl->engine) {
		_snap.detail = L"smoke-midi-transpose failed — no Session";
		return false;
	}
	try {
		refresh_midi_snap ();
		if (!ensure_midi_track () || !_snap.midi_track_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-transpose FAILED (track)\n");
			std::fflush (stderr);
			return false;
		}
		if (!ensure_midi_region (16.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-transpose FAILED (region)\n");
			std::fflush (stderr);
			return false;
		}
		clear_midi_note_selection ();
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count > 0) {
			std::shared_ptr<Playlist> playlist = _impl->midi_track
			                                        ? _impl->midi_track->playlist ()
			                                        : std::shared_ptr<Playlist> ();
			if (playlist && _impl->midi_region) {
				_impl->session->begin_reversible_command ("clear midi for transpose smoke");
				playlist->clear_changes ();
				playlist->clear_owned_changes ();
				playlist->remove_region (_impl->midi_region);
				playlist->rdiff_and_add_command (_impl->session);
				_impl->session->commit_reversible_command ();
				_impl->midi_region.reset ();
				_impl->selected_midi_note_id = -1;
				if (!ensure_midi_region (16.f)) {
					std::fprintf (stderr, "airdour_win_shell: smoke-midi-transpose FAILED (clear)\n");
					std::fflush (stderr);
					return false;
				}
			}
		}

		bool a = add_midi_note (60, 0.0f, 0.4f, 100); /* C4 */
		bool b = add_midi_note (64, 0.5f, 0.4f, 100); /* E4 */
		refresh_midi_notes_snap ();
		if (!a || !b || _snap.midi_note_count < 2) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-transpose FAILED (add)\n");
			std::fflush (stderr);
			return false;
		}

		clear_midi_note_selection ();
		if (!transpose_midi_notes (+1)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-transpose FAILED (+1)\n");
			std::fflush (stderr);
			return false;
		}
		refresh_midi_notes_snap ();
		bool got61 = false, got65 = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].pitch == 61) {
				got61 = true;
			}
			if (_snap.midi_notes[i].pitch == 65) {
				got65 = true;
			}
		}
		if (!got61 || !got65) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-transpose FAILED (+1 verify)\n");
			for (int i = 0; i < _snap.midi_note_count; ++i) {
				std::fprintf (stderr, "  note p=%u\n", (unsigned)_snap.midi_notes[i].pitch);
			}
			std::fflush (stderr);
			return false;
		}

		if (!transpose_midi_notes (-12)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-transpose FAILED (−12)\n");
			std::fflush (stderr);
			return false;
		}
		refresh_midi_notes_snap ();
		bool got49 = false, got53 = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].pitch == 49) {
				got49 = true;
			}
			if (_snap.midi_notes[i].pitch == 53) {
				got53 = true;
			}
		}
		if (!got49 || !got53) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-transpose FAILED (−12 verify)\n");
			for (int i = 0; i < _snap.midi_note_count; ++i) {
				std::fprintf (stderr, "  note p=%u\n", (unsigned)_snap.midi_notes[i].pitch);
			}
			std::fflush (stderr);
			return false;
		}

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-transpose ok (notes=%d +1→−12)\n",
		              _snap.midi_note_count);
		std::fflush (stderr);
		_snap.detail = L"smoke-midi-transpose ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-transpose exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}



bool
EngineHost::nudge_midi_velocity (int delta)
{
	if (delta == 0) {
		return true;
	}
	if (!_impl || !_impl->session) {
		_snap.detail = L"velocity: no Session";
		return false;
	}
	if (!ensure_midi_region (32.f) || !_impl->midi_region) {
		_snap.detail = L"velocity: no MIDI region";
		return false;
	}
	try {
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count <= 0) {
			_snap.detail = L"velocity: no notes";
			return false;
		}

		bool have_sel = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].selected) {
				have_sel = true;
				break;
			}
		}

		struct Kept {
			uint8_t pitch;
			uint8_t velocity;
			float   start_sec;
			float   length_sec;
		};
		std::vector<Kept> keep;
		keep.reserve ((size_t)_snap.midi_note_count);
		int moved = 0;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			Kept k;
			k.pitch = n.pitch;
			k.start_sec = n.start_sec;
			k.length_sec = n.length_sec;
			const bool do_v = !have_sel || n.selected;
			if (do_v) {
				int v = (int)n.velocity + delta;
				if (v < 1) {
					v = 1;
				}
				if (v > 127) {
					v = 127;
				}
				if (v != (int)n.velocity) {
					++moved;
				}
				k.velocity = (uint8_t)v;
			} else {
				k.velocity = n.velocity;
			}
			keep.push_back (k);
		}

		std::shared_ptr<Playlist> playlist = _impl->midi_track
		                                        ? _impl->midi_track->playlist ()
		                                        : std::shared_ptr<Playlist> ();
		if (!playlist) {
			_snap.detail = L"velocity: no playlist";
			return false;
		}
		_impl->session->begin_reversible_command ("midi velocity");
		playlist->clear_changes ();
		playlist->clear_owned_changes ();
		playlist->remove_region (_impl->midi_region);
		playlist->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();

		_impl->midi_region.reset ();
		_impl->selected_midi_note_id = -1;
		if (!ensure_midi_region (32.f)) {
			_snap.detail = L"velocity: region rebuild failed";
			return false;
		}
		int added = 0;
		int32_t first_id = -1;
		for (size_t i = 0; i < keep.size (); ++i) {
			if (add_midi_note (keep[i].pitch, keep[i].start_sec,
			                   keep[i].length_sec, keep[i].velocity)) {
				++added;
				if (first_id < 0 && _impl->selected_midi_note_id >= 0) {
					first_id = _impl->selected_midi_note_id;
				}
			}
		}
		/* Prefer re-select first note that was in the edited set. */
		_impl->selected_midi_note_id = first_id;
		refresh_midi_notes_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: velocity ok (notes=%d moved=%d added=%d d=%+d)\n",
		              _snap.midi_note_count, moved, added, delta);
		std::fflush (stderr);
		_snap.detail = L"MIDI velocity nudged";
		return added > 0 || keep.empty ();
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: velocity exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_midi_velocity (int velocity)
{
	if (velocity < 1) {
		velocity = 1;
	}
	if (velocity > 127) {
		velocity = 127;
	}
	if (!_impl || !_impl->session) {
		_snap.detail = L"velocity: no Session";
		return false;
	}
	if (!ensure_midi_region (32.f) || !_impl->midi_region) {
		_snap.detail = L"velocity: no MIDI region";
		return false;
	}
	try {
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count <= 0) {
			_snap.detail = L"velocity: no notes";
			return false;
		}

		bool have_sel = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].selected) {
				have_sel = true;
				break;
			}
		}

		struct Kept {
			uint8_t pitch;
			uint8_t velocity;
			float   start_sec;
			float   length_sec;
		};
		std::vector<Kept> keep;
		keep.reserve ((size_t)_snap.midi_note_count);
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			Kept k;
			k.pitch = n.pitch;
			k.start_sec = n.start_sec;
			k.length_sec = n.length_sec;
			const bool do_v = !have_sel || n.selected;
			k.velocity = do_v ? (uint8_t)velocity : n.velocity;
			keep.push_back (k);
		}

		std::shared_ptr<Playlist> playlist = _impl->midi_track
		                                        ? _impl->midi_track->playlist ()
		                                        : std::shared_ptr<Playlist> ();
		if (!playlist) {
			return false;
		}
		_impl->session->begin_reversible_command ("set midi velocity");
		playlist->clear_changes ();
		playlist->clear_owned_changes ();
		playlist->remove_region (_impl->midi_region);
		playlist->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();

		_impl->midi_region.reset ();
		_impl->selected_midi_note_id = -1;
		if (!ensure_midi_region (32.f)) {
			return false;
		}
		int32_t first_id = -1;
		for (size_t i = 0; i < keep.size (); ++i) {
			if (add_midi_note (keep[i].pitch, keep[i].start_sec,
			                   keep[i].length_sec, keep[i].velocity)) {
				if (first_id < 0 && _impl->selected_midi_note_id >= 0) {
					first_id = _impl->selected_midi_note_id;
				}
			}
		}
		_impl->selected_midi_note_id = first_id;
		refresh_midi_notes_snap ();
		_snap.detail = L"MIDI velocity set";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_midi_velocity ()
{
	if (!_impl || !_impl->session || !_impl->engine) {
		_snap.detail = L"smoke-midi-velocity failed — no Session";
		return false;
	}
	try {
		refresh_midi_snap ();
		if (!ensure_midi_track () || !_snap.midi_track_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-velocity FAILED (track)\n");
			std::fflush (stderr);
			return false;
		}
		if (!ensure_midi_region (16.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-velocity FAILED (region)\n");
			std::fflush (stderr);
			return false;
		}
		clear_midi_note_selection ();
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count > 0) {
			std::shared_ptr<Playlist> playlist = _impl->midi_track
			                                        ? _impl->midi_track->playlist ()
			                                        : std::shared_ptr<Playlist> ();
			if (playlist && _impl->midi_region) {
				_impl->session->begin_reversible_command ("clear midi for velocity smoke");
				playlist->clear_changes ();
				playlist->clear_owned_changes ();
				playlist->remove_region (_impl->midi_region);
				playlist->rdiff_and_add_command (_impl->session);
				_impl->session->commit_reversible_command ();
				_impl->midi_region.reset ();
				_impl->selected_midi_note_id = -1;
				if (!ensure_midi_region (16.f)) {
					std::fprintf (stderr, "airdour_win_shell: smoke-midi-velocity FAILED (clear)\n");
					std::fflush (stderr);
					return false;
				}
			}
		}

		if (!add_midi_note (60, 0.0f, 0.5f, 100) || _snap.midi_note_count < 1) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-velocity FAILED (add)\n");
			std::fflush (stderr);
			return false;
		}
		/* Select the note we just added. */
		if (_snap.midi_note_count > 0) {
			_impl->selected_midi_note_id = _snap.midi_notes[0].id;
			refresh_midi_notes_snap ();
		}

		if (!nudge_midi_velocity (-20)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-velocity FAILED (nudge)\n");
			std::fflush (stderr);
			return false;
		}
		refresh_midi_notes_snap ();
		bool ok80 = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].pitch == 60
			    && _snap.midi_notes[i].velocity >= 78
			    && _snap.midi_notes[i].velocity <= 82) {
				ok80 = true;
			}
		}
		if (!ok80) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-velocity FAILED (after nudge)\n");
			for (int i = 0; i < _snap.midi_note_count; ++i) {
				std::fprintf (stderr, "  note p=%u v=%u\n",
				              (unsigned)_snap.midi_notes[i].pitch,
				              (unsigned)_snap.midi_notes[i].velocity);
			}
			std::fflush (stderr);
			return false;
		}

		if (!set_midi_velocity (64)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-velocity FAILED (set)\n");
			std::fflush (stderr);
			return false;
		}
		refresh_midi_notes_snap ();
		bool ok64 = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].pitch == 60 && _snap.midi_notes[i].velocity == 64) {
				ok64 = true;
			}
		}
		if (!ok64) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-velocity FAILED (after set)\n");
			for (int i = 0; i < _snap.midi_note_count; ++i) {
				std::fprintf (stderr, "  note p=%u v=%u\n",
				              (unsigned)_snap.midi_notes[i].pitch,
				              (unsigned)_snap.midi_notes[i].velocity);
			}
			std::fflush (stderr);
			return false;
		}

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-velocity ok (v=64 notes=%d)\n",
		              _snap.midi_note_count);
		std::fflush (stderr);
		_snap.detail = L"smoke-midi-velocity ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-velocity exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

namespace {
constexpr float kMidiMinLengthSec = 0.05f;
} // namespace

bool
EngineHost::nudge_midi_length (float delta_sec)
{
	if (delta_sec == 0.f) {
		return true;
	}
	if (!_impl || !_impl->session) {
		_snap.detail = L"length: no Session";
		return false;
	}
	if (!ensure_midi_region (32.f) || !_impl->midi_region) {
		_snap.detail = L"length: no MIDI region";
		return false;
	}
	try {
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count <= 0) {
			_snap.detail = L"length: no notes";
			return false;
		}

		bool have_sel = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].selected) {
				have_sel = true;
				break;
			}
		}

		struct Kept {
			uint8_t pitch;
			uint8_t velocity;
			float   start_sec;
			float   length_sec;
		};
		std::vector<Kept> keep;
		keep.reserve ((size_t)_snap.midi_note_count);
		int moved = 0;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			Kept k;
			k.pitch = n.pitch;
			k.velocity = n.velocity;
			k.start_sec = n.start_sec;
			const bool do_l = !have_sel || n.selected;
			if (do_l) {
				float len = n.length_sec + delta_sec;
				if (len < kMidiMinLengthSec) {
					len = kMidiMinLengthSec;
				}
				if (_snap.snap_enabled) {
					float end = snap_time (k.start_sec + len);
					len = end - k.start_sec;
					if (len < kMidiMinLengthSec) {
						len = kMidiMinLengthSec;
					}
				}
				if (std::fabs (len - n.length_sec) > 1e-4f) {
					++moved;
				}
				k.length_sec = len;
			} else {
				k.length_sec = n.length_sec;
			}
			keep.push_back (k);
		}

		std::shared_ptr<Playlist> playlist = _impl->midi_track
		                                        ? _impl->midi_track->playlist ()
		                                        : std::shared_ptr<Playlist> ();
		if (!playlist) {
			_snap.detail = L"length: no playlist";
			return false;
		}
		_impl->session->begin_reversible_command ("midi length");
		playlist->clear_changes ();
		playlist->clear_owned_changes ();
		playlist->remove_region (_impl->midi_region);
		playlist->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();

		_impl->midi_region.reset ();
		_impl->selected_midi_note_id = -1;
		if (!ensure_midi_region (32.f)) {
			_snap.detail = L"length: region rebuild failed";
			return false;
		}
		int added = 0;
		int32_t first_id = -1;
		for (size_t i = 0; i < keep.size (); ++i) {
			if (add_midi_note (keep[i].pitch, keep[i].start_sec,
			                   keep[i].length_sec, keep[i].velocity)) {
				++added;
				if (first_id < 0 && _impl->selected_midi_note_id >= 0) {
					first_id = _impl->selected_midi_note_id;
				}
			}
		}
		_impl->selected_midi_note_id = first_id;
		refresh_midi_notes_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: length ok (notes=%d moved=%d added=%d d=%+.3f)\n",
		              _snap.midi_note_count, moved, added, delta_sec);
		std::fflush (stderr);
		_snap.detail = L"MIDI length nudged";
		return added > 0 || keep.empty ();
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: length exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_midi_note_start (float start_sec)
{
	if (start_sec < 0.f) {
		start_sec = 0.f;
	}
	if (_snap.snap_enabled) {
		start_sec = snap_time (start_sec);
	}
	if (!_impl || !_impl->session) {
		_snap.detail = L"length: no Session";
		return false;
	}
	if (!ensure_midi_region (32.f) || !_impl->midi_region) {
		_snap.detail = L"length: no MIDI region";
		return false;
	}
	try {
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count <= 0) {
			_snap.detail = L"length: no notes";
			return false;
		}

		bool have_sel = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].selected) {
				have_sel = true;
				break;
			}
		}
		if (!have_sel) {
			_snap.detail = L"length: select a note to trim";
			return false;
		}

		struct Kept {
			uint8_t pitch;
			uint8_t velocity;
			float   start_sec;
			float   length_sec;
		};
		std::vector<Kept> keep;
		keep.reserve ((size_t)_snap.midi_note_count);
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			Kept k;
			k.pitch = n.pitch;
			k.velocity = n.velocity;
			if (n.selected) {
				float end = n.start_sec + n.length_sec;
				float s = start_sec;
				if (s > end - kMidiMinLengthSec) {
					s = end - kMidiMinLengthSec;
				}
				if (s < 0.f) {
					s = 0.f;
				}
				k.start_sec = s;
				k.length_sec = end - s;
				if (k.length_sec < kMidiMinLengthSec) {
					k.length_sec = kMidiMinLengthSec;
				}
			} else {
				k.start_sec = n.start_sec;
				k.length_sec = n.length_sec;
			}
			keep.push_back (k);
		}

		std::shared_ptr<Playlist> playlist = _impl->midi_track
		                                        ? _impl->midi_track->playlist ()
		                                        : std::shared_ptr<Playlist> ();
		if (!playlist) {
			return false;
		}
		_impl->session->begin_reversible_command ("midi trim start");
		playlist->clear_changes ();
		playlist->clear_owned_changes ();
		playlist->remove_region (_impl->midi_region);
		playlist->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();

		_impl->midi_region.reset ();
		_impl->selected_midi_note_id = -1;
		if (!ensure_midi_region (32.f)) {
			return false;
		}
		int32_t first_id = -1;
		for (size_t i = 0; i < keep.size (); ++i) {
			if (add_midi_note (keep[i].pitch, keep[i].start_sec,
			                   keep[i].length_sec, keep[i].velocity)) {
				if (first_id < 0 && _impl->selected_midi_note_id >= 0) {
					first_id = _impl->selected_midi_note_id;
				}
			}
		}
		_impl->selected_midi_note_id = first_id;
		refresh_midi_notes_snap ();
		_snap.detail = L"MIDI note start trimmed";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::set_midi_note_end (float end_sec)
{
	if (end_sec < 0.f) {
		end_sec = 0.f;
	}
	if (_snap.snap_enabled) {
		end_sec = snap_time (end_sec);
	}
	if (!_impl || !_impl->session) {
		_snap.detail = L"length: no Session";
		return false;
	}
	if (!ensure_midi_region (32.f) || !_impl->midi_region) {
		_snap.detail = L"length: no MIDI region";
		return false;
	}
	try {
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count <= 0) {
			_snap.detail = L"length: no notes";
			return false;
		}

		bool have_sel = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].selected) {
				have_sel = true;
				break;
			}
		}
		if (!have_sel) {
			_snap.detail = L"length: select a note to trim";
			return false;
		}

		struct Kept {
			uint8_t pitch;
			uint8_t velocity;
			float   start_sec;
			float   length_sec;
		};
		std::vector<Kept> keep;
		keep.reserve ((size_t)_snap.midi_note_count);
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			Kept k;
			k.pitch = n.pitch;
			k.velocity = n.velocity;
			k.start_sec = n.start_sec;
			if (n.selected) {
				float len = end_sec - n.start_sec;
				if (len < kMidiMinLengthSec) {
					len = kMidiMinLengthSec;
				}
				k.length_sec = len;
			} else {
				k.length_sec = n.length_sec;
			}
			keep.push_back (k);
		}

		std::shared_ptr<Playlist> playlist = _impl->midi_track
		                                        ? _impl->midi_track->playlist ()
		                                        : std::shared_ptr<Playlist> ();
		if (!playlist) {
			return false;
		}
		_impl->session->begin_reversible_command ("midi trim end");
		playlist->clear_changes ();
		playlist->clear_owned_changes ();
		playlist->remove_region (_impl->midi_region);
		playlist->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();

		_impl->midi_region.reset ();
		_impl->selected_midi_note_id = -1;
		if (!ensure_midi_region (32.f)) {
			return false;
		}
		int32_t first_id = -1;
		for (size_t i = 0; i < keep.size (); ++i) {
			if (add_midi_note (keep[i].pitch, keep[i].start_sec,
			                   keep[i].length_sec, keep[i].velocity)) {
				if (first_id < 0 && _impl->selected_midi_note_id >= 0) {
					first_id = _impl->selected_midi_note_id;
				}
			}
		}
		_impl->selected_midi_note_id = first_id;
		refresh_midi_notes_snap ();
		_snap.detail = L"MIDI note end trimmed";
		return true;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_midi_length ()
{
	if (!_impl || !_impl->session || !_impl->engine) {
		_snap.detail = L"smoke-midi-length failed — no Session";
		return false;
	}
	try {
		refresh_midi_snap ();
		if (!ensure_midi_track () || !_snap.midi_track_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-length FAILED (track)\n");
			std::fflush (stderr);
			return false;
		}
		if (!ensure_midi_region (16.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-length FAILED (region)\n");
			std::fflush (stderr);
			return false;
		}
		clear_midi_note_selection ();
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count > 0) {
			std::shared_ptr<Playlist> playlist = _impl->midi_track
			                                        ? _impl->midi_track->playlist ()
			                                        : std::shared_ptr<Playlist> ();
			if (playlist && _impl->midi_region) {
				_impl->session->begin_reversible_command ("clear midi for length smoke");
				playlist->clear_changes ();
				playlist->clear_owned_changes ();
				playlist->remove_region (_impl->midi_region);
				playlist->rdiff_and_add_command (_impl->session);
				_impl->session->commit_reversible_command ();
				_impl->midi_region.reset ();
				_impl->selected_midi_note_id = -1;
				if (!ensure_midi_region (16.f)) {
					std::fprintf (stderr, "airdour_win_shell: smoke-midi-length FAILED (clear)\n");
					std::fflush (stderr);
					return false;
				}
			}
		}

		const bool snap_was = _snap.snap_enabled;
		set_snap_enabled (false);

		if (!add_midi_note (60, 0.0f, 1.0f, 100) || _snap.midi_note_count < 1) {
			set_snap_enabled (snap_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-length FAILED (add)\n");
			std::fflush (stderr);
			return false;
		}
		if (_snap.midi_note_count > 0) {
			_impl->selected_midi_note_id = _snap.midi_notes[0].id;
			refresh_midi_notes_snap ();
		}

		if (!set_midi_note_end (0.5f)) {
			set_snap_enabled (snap_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-length FAILED (trim end)\n");
			std::fflush (stderr);
			return false;
		}
		refresh_midi_notes_snap ();
		bool ok05 = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].pitch == 60
			    && _snap.midi_notes[i].length_sec >= 0.45f
			    && _snap.midi_notes[i].length_sec <= 0.55f) {
				ok05 = true;
			}
		}
		if (!ok05) {
			set_snap_enabled (snap_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-length FAILED (after trim)\n");
			for (int i = 0; i < _snap.midi_note_count; ++i) {
				std::fprintf (stderr, "  note p=%u len=%.3f\n",
				              (unsigned)_snap.midi_notes[i].pitch,
				              _snap.midi_notes[i].length_sec);
			}
			std::fflush (stderr);
			return false;
		}

		if (!nudge_midi_length (0.25f)) {
			set_snap_enabled (snap_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-length FAILED (nudge)\n");
			std::fflush (stderr);
			return false;
		}
		refresh_midi_notes_snap ();
		bool ok075 = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].pitch == 60
			    && _snap.midi_notes[i].length_sec >= 0.70f
			    && _snap.midi_notes[i].length_sec <= 0.80f) {
				ok075 = true;
			}
		}
		set_snap_enabled (snap_was);
		if (!ok075) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-length FAILED (after nudge)\n");
			for (int i = 0; i < _snap.midi_note_count; ++i) {
				std::fprintf (stderr, "  note p=%u len=%.3f\n",
				              (unsigned)_snap.midi_notes[i].pitch,
				              _snap.midi_notes[i].length_sec);
			}
			std::fflush (stderr);
			return false;
		}

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-length ok (len~0.75 notes=%d)\n",
		              _snap.midi_note_count);
		std::fflush (stderr);
		_snap.detail = L"smoke-midi-length ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-length exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::has_midi_clipboard () const
{
	return !_midi_clipboard.empty ();
}

bool
EngineHost::copy_selected_midi_notes ()
{
	refresh_midi_notes_snap ();
	_midi_clipboard.clear ();
	if (_snap.midi_note_count <= 0) {
		_snap.detail = L"MIDI copy: no notes";
		return false;
	}
	bool have_sel = false;
	float t0 = 1.0e9f;
	for (int i = 0; i < _snap.midi_note_count; ++i) {
		const EngineMidiNoteSnap& n = _snap.midi_notes[i];
		if (!n.selected) {
			continue;
		}
		have_sel = true;
		if (n.start_sec < t0) {
			t0 = n.start_sec;
		}
	}
	if (!have_sel) {
		_snap.detail = L"MIDI copy: nothing selected";
		return false;
	}
	for (int i = 0; i < _snap.midi_note_count; ++i) {
		const EngineMidiNoteSnap& n = _snap.midi_notes[i];
		if (!n.selected) {
			continue;
		}
		MidiClipNote c;
		c.pitch = n.pitch;
		c.velocity = n.velocity;
		c.start_rel_sec = n.start_sec - t0;
		c.length_sec = n.length_sec;
		if (c.length_sec < 0.05f) {
			c.length_sec = 0.05f;
		}
		_midi_clipboard.push_back (c);
	}
	std::fprintf (stderr, "airdour_win_shell: MIDI copy (%zu notes)\n",
	              _midi_clipboard.size ());
	std::fflush (stderr);
	_snap.detail = L"MIDI notes copied";
	return !_midi_clipboard.empty ();
}

bool
EngineHost::cut_selected_midi_notes ()
{
	if (!copy_selected_midi_notes ()) {
		return false;
	}
	if (!_impl || !_impl->session) {
		_snap.detail = L"MIDI cut: no Session";
		return false;
	}
	if (!ensure_midi_region (32.f) || !_impl->midi_region) {
		_snap.detail = L"MIDI cut: no MIDI region";
		return false;
	}
	try {
		refresh_midi_notes_snap ();
		struct Kept {
			uint8_t pitch;
			uint8_t velocity;
			float   start_sec;
			float   length_sec;
		};
		std::vector<Kept> keep;
		keep.reserve ((size_t)_snap.midi_note_count);
		int removed = 0;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			if (n.selected) {
				++removed;
				continue;
			}
			Kept k;
			k.pitch = n.pitch;
			k.velocity = n.velocity;
			k.start_sec = n.start_sec;
			k.length_sec = n.length_sec;
			keep.push_back (k);
		}
		if (removed <= 0) {
			return false;
		}

		std::shared_ptr<Playlist> playlist = _impl->midi_track
		                                        ? _impl->midi_track->playlist ()
		                                        : std::shared_ptr<Playlist> ();
		if (!playlist) {
			return false;
		}
		_impl->session->begin_reversible_command ("cut midi notes");
		playlist->clear_changes ();
		playlist->clear_owned_changes ();
		playlist->remove_region (_impl->midi_region);
		playlist->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();

		_impl->midi_region.reset ();
		_impl->selected_midi_note_id = -1;
		if (!ensure_midi_region (32.f)) {
			return false;
		}
		for (size_t i = 0; i < keep.size (); ++i) {
			add_midi_note (keep[i].pitch, keep[i].start_sec,
			               keep[i].length_sec, keep[i].velocity);
		}
		_impl->selected_midi_note_id = -1;
		refresh_midi_notes_snap ();
		std::fprintf (stderr, "airdour_win_shell: MIDI cut (removed=%d kept=%zu clip=%zu)\n",
		              removed, keep.size (), _midi_clipboard.size ());
		std::fflush (stderr);
		_snap.detail = L"MIDI notes cut";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: MIDI cut exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::paste_midi_notes (float at_sec)
{
	if (_midi_clipboard.empty ()) {
		_snap.detail = L"MIDI paste: clipboard empty";
		return false;
	}
	if (!_impl || !_impl->session) {
		_snap.detail = L"MIDI paste: no Session";
		return false;
	}
	if (at_sec < 0.f) {
		at_sec = _snap.transport_sec;
	}
	if (at_sec < 0.f) {
		at_sec = 0.f;
	}
	if (_snap.snap_enabled) {
		at_sec = snap_time (at_sec);
	}
	if (!ensure_midi_region (32.f) || !_impl->midi_region) {
		_snap.detail = L"MIDI paste: no MIDI region";
		return false;
	}
	try {
		int added = 0;
		int32_t first_id = -1;
		for (size_t i = 0; i < _midi_clipboard.size (); ++i) {
			const MidiClipNote& c = _midi_clipboard[i];
			float start = at_sec + c.start_rel_sec;
			if (start < 0.f) {
				start = 0.f;
			}
			if (add_midi_note (c.pitch, start, c.length_sec, c.velocity)) {
				++added;
				if (first_id < 0 && _impl->selected_midi_note_id >= 0) {
					first_id = _impl->selected_midi_note_id;
				}
			}
		}
		_impl->selected_midi_note_id = first_id;
		refresh_midi_notes_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: MIDI paste (added=%d at=%.3f clip=%zu)\n",
		              added, at_sec, _midi_clipboard.size ());
		std::fflush (stderr);
		_snap.detail = L"MIDI notes pasted";
		return added > 0;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: MIDI paste exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_midi_clipboard ()
{
	if (!_impl || !_impl->session || !_impl->engine) {
		_snap.detail = L"smoke-midi-clipboard failed — no Session";
		return false;
	}
	try {
		refresh_midi_snap ();
		if (!ensure_midi_track () || !_snap.midi_track_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-clipboard FAILED (track)\n");
			std::fflush (stderr);
			return false;
		}
		if (!ensure_midi_region (16.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-clipboard FAILED (region)\n");
			std::fflush (stderr);
			return false;
		}
		clear_midi_note_selection ();
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count > 0) {
			std::shared_ptr<Playlist> playlist = _impl->midi_track
			                                        ? _impl->midi_track->playlist ()
			                                        : std::shared_ptr<Playlist> ();
			if (playlist && _impl->midi_region) {
				_impl->session->begin_reversible_command ("clear midi for clipboard smoke");
				playlist->clear_changes ();
				playlist->clear_owned_changes ();
				playlist->remove_region (_impl->midi_region);
				playlist->rdiff_and_add_command (_impl->session);
				_impl->session->commit_reversible_command ();
				_impl->midi_region.reset ();
				_impl->selected_midi_note_id = -1;
				if (!ensure_midi_region (16.f)) {
					std::fprintf (stderr, "airdour_win_shell: smoke-midi-clipboard FAILED (clear)\n");
					std::fflush (stderr);
					return false;
				}
			}
		}

		const bool snap_was = _snap.snap_enabled;
		set_snap_enabled (false);

		if (!add_midi_note (60, 0.0f, 0.4f, 100) || _snap.midi_note_count < 1) {
			set_snap_enabled (snap_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-clipboard FAILED (add)\n");
			std::fflush (stderr);
			return false;
		}
		_impl->selected_midi_note_id = _snap.midi_notes[0].id;
		refresh_midi_notes_snap ();

		if (!copy_selected_midi_notes () || !has_midi_clipboard ()) {
			set_snap_enabled (snap_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-clipboard FAILED (copy)\n");
			std::fflush (stderr);
			return false;
		}

		if (!paste_midi_notes (1.0f)) {
			set_snap_enabled (snap_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-clipboard FAILED (paste)\n");
			std::fflush (stderr);
			return false;
		}
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count < 2) {
			set_snap_enabled (snap_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-clipboard FAILED (count after paste=%d)\n",
			              _snap.midi_note_count);
			std::fflush (stderr);
			return false;
		}
		bool got_paste = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].pitch == 60
			    && _snap.midi_notes[i].start_sec >= 0.95f
			    && _snap.midi_notes[i].start_sec <= 1.05f) {
				got_paste = true;
				_impl->selected_midi_note_id = _snap.midi_notes[i].id;
			}
		}
		if (!got_paste) {
			set_snap_enabled (snap_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-clipboard FAILED (paste locate)\n");
			for (int i = 0; i < _snap.midi_note_count; ++i) {
				std::fprintf (stderr, "  note p=%u t=%.3f\n",
				              (unsigned)_snap.midi_notes[i].pitch,
				              _snap.midi_notes[i].start_sec);
			}
			std::fflush (stderr);
			return false;
		}
		refresh_midi_notes_snap ();

		if (!cut_selected_midi_notes ()) {
			set_snap_enabled (snap_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-clipboard FAILED (cut)\n");
			std::fflush (stderr);
			return false;
		}
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count != 1) {
			set_snap_enabled (snap_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-clipboard FAILED (after cut count=%d)\n",
			              _snap.midi_note_count);
			std::fflush (stderr);
			return false;
		}

		if (!paste_midi_notes (2.0f)) {
			set_snap_enabled (snap_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-clipboard FAILED (repaste)\n");
			std::fflush (stderr);
			return false;
		}
		refresh_midi_notes_snap ();
		bool got2 = false;
		bool got0 = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].pitch != 60) {
				continue;
			}
			if (_snap.midi_notes[i].start_sec >= -0.05f
			    && _snap.midi_notes[i].start_sec <= 0.05f) {
				got0 = true;
			}
			if (_snap.midi_notes[i].start_sec >= 1.95f
			    && _snap.midi_notes[i].start_sec <= 2.05f) {
				got2 = true;
			}
		}
		set_snap_enabled (snap_was);
		if (!got0 || !got2 || _snap.midi_note_count < 2) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-clipboard FAILED (final)\n");
			for (int i = 0; i < _snap.midi_note_count; ++i) {
				std::fprintf (stderr, "  note p=%u t=%.3f\n",
				              (unsigned)_snap.midi_notes[i].pitch,
				              _snap.midi_notes[i].start_sec);
			}
			std::fflush (stderr);
			return false;
		}

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-clipboard ok (notes=%d copy/paste/cut)\n",
		              _snap.midi_note_count);
		std::fflush (stderr);
		_snap.detail = L"smoke-midi-clipboard ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-clipboard exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::humanize_midi_notes (float strength, uint32_t seed)
{
	if (strength < 0.1f) {
		strength = 0.1f;
	}
	if (strength > 2.f) {
		strength = 2.f;
	}
	if (!_impl || !_impl->session) {
		_snap.detail = L"humanize: no Session";
		return false;
	}
	if (!ensure_midi_region (32.f) || !_impl->midi_region) {
		_snap.detail = L"humanize: no MIDI region";
		return false;
	}
	try {
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count <= 0) {
			_snap.detail = L"humanize: no notes";
			return false;
		}

		bool have_sel = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].selected) {
				have_sel = true;
				break;
			}
		}

		uint32_t use_seed = seed;
		if (use_seed == 0) {
			use_seed = (uint32_t)std::chrono::steady_clock::now ().time_since_epoch ().count ();
			use_seed ^= (uint32_t)_snap.midi_note_count * 2654435761u;
		}
		std::mt19937 rng (use_seed);
		/* strength 1 → ±20 ms timing, ±8 velocity. */
		const float tmax = 0.020f * strength;
		const float vmax = 8.f * strength;
		std::uniform_real_distribution<float> tdist (-tmax, tmax);
		std::uniform_real_distribution<float> vdist (-vmax, vmax);

		struct Kept {
			uint8_t pitch;
			uint8_t velocity;
			float   start_sec;
			float   length_sec;
		};
		std::vector<Kept> keep;
		keep.reserve ((size_t)_snap.midi_note_count);
		int moved = 0;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			Kept k;
			k.pitch = n.pitch;
			k.length_sec = n.length_sec;
			if (k.length_sec < 0.05f) {
				k.length_sec = 0.05f;
			}
			const bool do_h = !have_sel || n.selected;
			if (do_h) {
				float t = n.start_sec + tdist (rng);
				if (t < 0.f) {
					t = 0.f;
				}
				int v = (int)std::lround ((float)n.velocity + vdist (rng));
				if (v < 1) {
					v = 1;
				}
				if (v > 127) {
					v = 127;
				}
				if (std::fabs (t - n.start_sec) > 1e-5f || v != (int)n.velocity) {
					++moved;
				}
				k.start_sec = t;
				k.velocity = (uint8_t)v;
			} else {
				k.start_sec = n.start_sec;
				k.velocity = n.velocity;
			}
			keep.push_back (k);
		}

		std::shared_ptr<Playlist> playlist = _impl->midi_track
		                                        ? _impl->midi_track->playlist ()
		                                        : std::shared_ptr<Playlist> ();
		if (!playlist) {
			_snap.detail = L"humanize: no playlist";
			return false;
		}
		_impl->session->begin_reversible_command ("humanize midi");
		playlist->clear_changes ();
		playlist->clear_owned_changes ();
		playlist->remove_region (_impl->midi_region);
		playlist->rdiff_and_add_command (_impl->session);
		_impl->session->commit_reversible_command ();

		_impl->midi_region.reset ();
		_impl->selected_midi_note_id = -1;
		if (!ensure_midi_region (32.f)) {
			_snap.detail = L"humanize: region rebuild failed";
			return false;
		}
		int added = 0;
		int32_t first_id = -1;
		for (size_t i = 0; i < keep.size (); ++i) {
			if (add_midi_note (keep[i].pitch, keep[i].start_sec,
			                   keep[i].length_sec, keep[i].velocity)) {
				++added;
				if (first_id < 0 && _impl->selected_midi_note_id >= 0) {
					first_id = _impl->selected_midi_note_id;
				}
			}
		}
		_impl->selected_midi_note_id = first_id;
		refresh_midi_notes_snap ();
		std::fprintf (stderr,
		              "airdour_win_shell: humanize ok (notes=%d moved=%d added=%d str=%.2f seed=%u)\n",
		              _snap.midi_note_count, moved, added, strength, use_seed);
		std::fflush (stderr);
		_snap.detail = L"MIDI humanized";
		return added > 0 || keep.empty ();
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: humanize exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_midi_humanize ()
{
	if (!_impl || !_impl->session || !_impl->engine) {
		_snap.detail = L"smoke-midi-humanize failed — no Session";
		return false;
	}
	try {
		refresh_midi_snap ();
		if (!ensure_midi_track () || !_snap.midi_track_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-humanize FAILED (track)\n");
			std::fflush (stderr);
			return false;
		}
		if (!ensure_midi_region (16.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-humanize FAILED (region)\n");
			std::fflush (stderr);
			return false;
		}
		clear_midi_note_selection ();
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count > 0) {
			std::shared_ptr<Playlist> playlist = _impl->midi_track
			                                        ? _impl->midi_track->playlist ()
			                                        : std::shared_ptr<Playlist> ();
			if (playlist && _impl->midi_region) {
				_impl->session->begin_reversible_command ("clear midi for humanize smoke");
				playlist->clear_changes ();
				playlist->clear_owned_changes ();
				playlist->remove_region (_impl->midi_region);
				playlist->rdiff_and_add_command (_impl->session);
				_impl->session->commit_reversible_command ();
				_impl->midi_region.reset ();
				_impl->selected_midi_note_id = -1;
				if (!ensure_midi_region (16.f)) {
					std::fprintf (stderr, "airdour_win_shell: smoke-midi-humanize FAILED (clear)\n");
					std::fflush (stderr);
					return false;
				}
			}
		}

		const bool snap_was = _snap.snap_enabled;
		set_snap_enabled (false);

		bool a = add_midi_note (60, 1.0f, 0.4f, 100);
		bool b = add_midi_note (64, 1.5f, 0.4f, 100);
		refresh_midi_notes_snap ();
		if (!a || !b || _snap.midi_note_count < 2) {
			set_snap_enabled (snap_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-humanize FAILED (add)\n");
			std::fflush (stderr);
			return false;
		}

		struct Orig {
			uint8_t pitch;
			uint8_t velocity;
			float   start_sec;
		};
		Orig orig[8];
		int norig = 0;
		for (int i = 0; i < _snap.midi_note_count && norig < 8; ++i) {
			orig[norig].pitch = _snap.midi_notes[i].pitch;
			orig[norig].velocity = _snap.midi_notes[i].velocity;
			orig[norig].start_sec = _snap.midi_notes[i].start_sec;
			++norig;
		}

		clear_midi_note_selection (); /* humanize ALL */
		if (!humanize_midi_notes (1.f, 42u)) {
			set_snap_enabled (snap_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-humanize FAILED (humanize)\n");
			std::fflush (stderr);
			return false;
		}
		refresh_midi_notes_snap ();

		const float tmax = 0.020f + 0.002f; /* slack */
		const int vmax = 8 + 1;
		bool any_changed = false;
		bool bounds_ok = true;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			const Orig* o = nullptr;
			for (int j = 0; j < norig; ++j) {
				if (orig[j].pitch == n.pitch) {
					o = &orig[j];
					break;
				}
			}
			if (!o) {
				continue;
			}
			float dt = std::fabs (n.start_sec - o->start_sec);
			int dv = std::abs ((int)n.velocity - (int)o->velocity);
			if (dt > 1e-4f || dv > 0) {
				any_changed = true;
			}
			if (dt > tmax || dv > vmax || n.velocity < 1 || n.velocity > 127
			    || n.start_sec < 0.f) {
				bounds_ok = false;
			}
		}
		set_snap_enabled (snap_was);
		if (!any_changed || !bounds_ok) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-midi-humanize FAILED (changed=%d bounds=%d)\n",
			              any_changed ? 1 : 0, bounds_ok ? 1 : 0);
			for (int i = 0; i < _snap.midi_note_count; ++i) {
				std::fprintf (stderr, "  note p=%u t=%.4f v=%u\n",
				              (unsigned)_snap.midi_notes[i].pitch,
				              _snap.midi_notes[i].start_sec,
				              (unsigned)_snap.midi_notes[i].velocity);
			}
			std::fflush (stderr);
			return false;
		}

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-humanize ok (notes=%d seed=42)\n",
		              _snap.midi_note_count);
		std::fflush (stderr);
		_snap.detail = L"smoke-midi-humanize ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-humanize exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

void
EngineHost::set_midi_step_enabled (bool on)
{
	_snap.midi_step_enabled = on;
	_snap.detail = on ? L"MIDI step-record on" : L"MIDI step-record off";
}

bool
EngineHost::midi_step_enabled () const
{
	return _snap.midi_step_enabled;
}

bool
EngineHost::toggle_midi_step_enabled ()
{
	set_midi_step_enabled (!_snap.midi_step_enabled);
	return _snap.midi_step_enabled;
}

void
EngineHost::set_midi_chord_quality (int quality)
{
	if (quality < 0) {
		quality = 0;
	}
	if (quality > 2) {
		quality = 2;
	}
	_snap.midi_chord_quality = quality;
	if (quality == 1) {
		_snap.detail = L"MIDI chord Maj";
	} else if (quality == 2) {
		_snap.detail = L"MIDI chord Min";
	} else {
		_snap.detail = L"MIDI chord Off";
	}
}

int
EngineHost::midi_chord_quality () const
{
	return _snap.midi_chord_quality;
}

int
EngineHost::cycle_midi_chord_quality ()
{
	int q = _snap.midi_chord_quality + 1;
	if (q > 2) {
		q = 0;
	}
	set_midi_chord_quality (q);
	return _snap.midi_chord_quality;
}

bool
EngineHost::step_advance (int steps)
{
	if (steps == 0) {
		return true;
	}
	float unit = snap_unit_sec ();
	if (unit < 0.05f) {
		unit = 0.05f;
	}
	float t = _snap.transport_sec + unit * (float)steps;
	if (t < 0.f) {
		t = 0.f;
	}
	/* Force grid snap for the destination even if Snap UI is off. */
	const bool snap_was = _snap.snap_enabled;
	set_snap_enabled (true);
	bool ok = locate_to (t, false, 0.f);
	set_snap_enabled (snap_was);
	return ok;
}

void
EngineHost::set_midi_scale_lock (bool on)
{
	_snap.midi_scale_lock = on;
	_snap.detail = on ? L"MIDI scale lock on" : L"MIDI scale lock off";
}

bool
EngineHost::midi_scale_lock () const
{
	return _snap.midi_scale_lock;
}

bool
EngineHost::toggle_midi_scale_lock ()
{
	set_midi_scale_lock (!_snap.midi_scale_lock);
	return _snap.midi_scale_lock;
}

void
EngineHost::set_midi_scale_kind (int kind)
{
	if (kind < 0) {
		kind = 0;
	}
	if (kind > 1) {
		kind = 1;
	}
	_snap.midi_scale_kind = kind;
	_snap.detail = (kind == 1) ? L"MIDI scale minor" : L"MIDI scale major";
}

int
EngineHost::midi_scale_kind () const
{
	return _snap.midi_scale_kind;
}

int
EngineHost::cycle_midi_scale_kind ()
{
	set_midi_scale_kind (_snap.midi_scale_kind == 0 ? 1 : 0);
	return _snap.midi_scale_kind;
}

void
EngineHost::set_midi_scale_root (int root)
{
	if (root < 0) {
		root = 0;
	}
	if (root > 11) {
		root = 11;
	}
	_snap.midi_scale_root = root;
	_snap.detail = L"MIDI scale root";
}

int
EngineHost::midi_scale_root () const
{
	return _snap.midi_scale_root;
}

int
EngineHost::cycle_midi_scale_root ()
{
	int r = _snap.midi_scale_root + 1;
	if (r > 11) {
		r = 0;
	}
	set_midi_scale_root (r);
	return _snap.midi_scale_root;
}

uint8_t
EngineHost::snap_pitch_to_scale (uint8_t pitch) const
{
	if (!_snap.midi_scale_lock) {
		return pitch > 127 ? 127 : pitch;
	}
	if (pitch > 127) {
		pitch = 127;
	}
	static const int kMajor[7] = { 0, 2, 4, 5, 7, 9, 11 };
	static const int kMinor[7] = { 0, 2, 3, 5, 7, 8, 10 }; /* natural */
	const int* deg = (_snap.midi_scale_kind == 1) ? kMinor : kMajor;
	const int root = _snap.midi_scale_root;
	const int pc = (int)pitch % 12;
	int best_pc = (root + deg[0]) % 12;
	int best_dist = 99;
	for (int i = 0; i < 7; ++i) {
		int dpc = (root + deg[i]) % 12;
		int dist = std::abs (dpc - pc);
		if (dist > 6) {
			dist = 12 - dist; /* wrap */
		}
		/* Prefer upward on ties so C#→D in C major. */
		bool better = dist < best_dist;
		if (!better && dist == best_dist) {
			int up = (dpc - pc + 12) % 12;
			int up_best = (best_pc - pc + 12) % 12;
			better = up < up_best;
		}
		if (better) {
			best_dist = dist;
			best_pc = dpc;
		}
	}
	int octave = (int)pitch / 12;
	int out = octave * 12 + best_pc;
	/* Keep in same octave neighborhood if wrap jumped across. */
	if (out - (int)pitch > 6) {
		out -= 12;
	} else if ((int)pitch - out > 6) {
		out += 12;
	}
	if (out < 0) {
		out = 0;
	}
	if (out > 127) {
		out = 127;
	}
	return (uint8_t)out;
}

bool
EngineHost::step_input_midi_note (uint8_t pitch, uint8_t velocity, bool advance)
{
	if (pitch > 127) {
		pitch = 127;
	}
	if (velocity < 1) {
		velocity = 1;
	}
	if (velocity > 127) {
		velocity = 127;
	}
	/* Scale lock snaps the input root before chord expansion. */
	pitch = snap_pitch_to_scale (pitch);
	if (!_impl || !_impl->session) {
		_snap.detail = L"step: no Session";
		return false;
	}
	float unit = snap_unit_sec ();
	if (unit < 0.05f) {
		unit = 0.05f;
	}
	float start = _snap.transport_sec;
	if (start < 0.f) {
		start = 0.f;
	}
	/* Snap placement origin when Snap UI is on. */
	if (_snap.snap_enabled) {
		start = snap_time (start);
	}
	float len = unit * 0.9f;
	if (len < 0.05f) {
		len = 0.05f;
	}
	if (!ensure_midi_region (32.f)) {
		_snap.detail = L"step: no MIDI region";
		return false;
	}

	/* Expand root → triad when chord quality is Maj/Min. */
	uint8_t pitches[3];
	int n = 1;
	pitches[0] = pitch;
	const int q = _snap.midi_chord_quality;
	if (q == 1) {
		if ((int)pitch + 4 <= 127) {
			pitches[n++] = (uint8_t)(pitch + 4);
		}
		if ((int)pitch + 7 <= 127) {
			pitches[n++] = (uint8_t)(pitch + 7);
		}
	} else if (q == 2) {
		if ((int)pitch + 3 <= 127) {
			pitches[n++] = (uint8_t)(pitch + 3);
		}
		if ((int)pitch + 7 <= 127) {
			pitches[n++] = (uint8_t)(pitch + 7);
		}
	}

	for (int i = 0; i < n; ++i) {
		if (!add_midi_note (pitches[i], start, len, velocity)) {
			_snap.detail = L"step: add note failed";
			return false;
		}
	}
	refresh_midi_notes_snap ();

	if (advance) {
		float next = start + unit;
		const bool snap_was = _snap.snap_enabled;
		set_snap_enabled (true);
		locate_to (next, false, 0.f);
		set_snap_enabled (snap_was);
	}
	std::fprintf (stderr,
	              "airdour_win_shell: step note root=%u n=%d q=%d t=%.3f len=%.3f adv=%d → %.3f\n",
	              (unsigned)pitch, n, q, start, len, advance ? 1 : 0,
	              _snap.transport_sec);
	std::fflush (stderr);
	_snap.detail = (n > 1) ? L"MIDI step chord" : L"MIDI step note";
	return true;
}

bool
EngineHost::smoke_midi_step ()
{
	if (!_impl || !_impl->session || !_impl->engine) {
		_snap.detail = L"smoke-midi-step failed — no Session";
		return false;
	}
	try {
		refresh_midi_snap ();
		if (!ensure_midi_track () || !_snap.midi_track_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-step FAILED (track)\n");
			std::fflush (stderr);
			return false;
		}
		if (!ensure_midi_region (16.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-step FAILED (region)\n");
			std::fflush (stderr);
			return false;
		}
		clear_midi_note_selection ();
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count > 0) {
			std::shared_ptr<Playlist> playlist = _impl->midi_track
			                                        ? _impl->midi_track->playlist ()
			                                        : std::shared_ptr<Playlist> ();
			if (playlist && _impl->midi_region) {
				_impl->session->begin_reversible_command ("clear midi for step smoke");
				playlist->clear_changes ();
				playlist->clear_owned_changes ();
				playlist->remove_region (_impl->midi_region);
				playlist->rdiff_and_add_command (_impl->session);
				_impl->session->commit_reversible_command ();
				_impl->midi_region.reset ();
				_impl->selected_midi_note_id = -1;
				if (!ensure_midi_region (16.f)) {
					std::fprintf (stderr, "airdour_win_shell: smoke-midi-step FAILED (clear)\n");
					std::fflush (stderr);
					return false;
				}
			}
		}

		const bool snap_was = _snap.snap_enabled;
		const int dens_was = _snap.snap_density;
		set_snap_density (1); /* beat = 0.5s @ 120 */
		set_snap_enabled (false);
		set_midi_step_enabled (true);
		if (!midi_step_enabled ()) {
			set_snap_enabled (snap_was);
			set_snap_density (dens_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-step FAILED (enable)\n");
			std::fflush (stderr);
			return false;
		}

		if (!locate_to (0.f, false, 0.f)) {
			set_midi_step_enabled (false);
			set_snap_enabled (snap_was);
			set_snap_density (dens_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-step FAILED (locate 0)\n");
			std::fflush (stderr);
			return false;
		}

		float unit = snap_unit_sec ();
		if (!step_input_midi_note (60, 100)) {
			set_midi_step_enabled (false);
			set_snap_enabled (snap_was);
			set_snap_density (dens_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-step FAILED (note 60)\n");
			std::fflush (stderr);
			return false;
		}
		if (std::fabs (_snap.transport_sec - unit) > 0.08f) {
			set_midi_step_enabled (false);
			set_snap_enabled (snap_was);
			set_snap_density (dens_was);
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-midi-step FAILED (advance1 t=%.3f want=%.3f)\n",
			              _snap.transport_sec, unit);
			std::fflush (stderr);
			return false;
		}

		if (!step_input_midi_note (64, 100)) {
			set_midi_step_enabled (false);
			set_snap_enabled (snap_was);
			set_snap_density (dens_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-step FAILED (note 64)\n");
			std::fflush (stderr);
			return false;
		}
		refresh_midi_notes_snap ();
		bool got60 = false, got64 = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].pitch == 60
			    && _snap.midi_notes[i].start_sec >= -0.05f
			    && _snap.midi_notes[i].start_sec <= 0.08f) {
				got60 = true;
			}
			if (_snap.midi_notes[i].pitch == 64
			    && _snap.midi_notes[i].start_sec >= unit - 0.08f
			    && _snap.midi_notes[i].start_sec <= unit + 0.08f) {
				got64 = true;
			}
		}
		if (!got60 || !got64 || _snap.midi_note_count < 2) {
			set_midi_step_enabled (false);
			set_snap_enabled (snap_was);
			set_snap_density (dens_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-step FAILED (verify notes)\n");
			for (int i = 0; i < _snap.midi_note_count; ++i) {
				std::fprintf (stderr, "  note p=%u t=%.3f\n",
				              (unsigned)_snap.midi_notes[i].pitch,
				              _snap.midi_notes[i].start_sec);
			}
			std::fflush (stderr);
			return false;
		}

		if (!step_advance (1)) {
			set_midi_step_enabled (false);
			set_snap_enabled (snap_was);
			set_snap_density (dens_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-step FAILED (rest advance)\n");
			std::fflush (stderr);
			return false;
		}
		float want3 = unit * 3.f;
		if (std::fabs (_snap.transport_sec - want3) > 0.12f) {
			set_midi_step_enabled (false);
			set_snap_enabled (snap_was);
			set_snap_density (dens_was);
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-midi-step FAILED (rest t=%.3f want=%.3f)\n",
			              _snap.transport_sec, want3);
			std::fflush (stderr);
			return false;
		}

		set_midi_step_enabled (false);
		set_snap_enabled (snap_was);
		set_snap_density (dens_was);

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-step ok (notes=%d unit=%.3f)\n",
		              _snap.midi_note_count, unit);
		std::fflush (stderr);
		_snap.detail = L"smoke-midi-step ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-step exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_midi_chord ()
{
	if (!_impl || !_impl->session || !_impl->engine) {
		_snap.detail = L"smoke-midi-chord failed — no Session";
		return false;
	}
	try {
		refresh_midi_snap ();
		if (!ensure_midi_track () || !_snap.midi_track_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-chord FAILED (track)\n");
			std::fflush (stderr);
			return false;
		}
		if (!ensure_midi_region (16.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-chord FAILED (region)\n");
			std::fflush (stderr);
			return false;
		}
		clear_midi_note_selection ();
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count > 0) {
			std::shared_ptr<Playlist> playlist = _impl->midi_track
			                                        ? _impl->midi_track->playlist ()
			                                        : std::shared_ptr<Playlist> ();
			if (playlist && _impl->midi_region) {
				_impl->session->begin_reversible_command ("clear midi for chord smoke");
				playlist->clear_changes ();
				playlist->clear_owned_changes ();
				playlist->remove_region (_impl->midi_region);
				playlist->rdiff_and_add_command (_impl->session);
				_impl->session->commit_reversible_command ();
				_impl->midi_region.reset ();
				_impl->selected_midi_note_id = -1;
				if (!ensure_midi_region (16.f)) {
					std::fprintf (stderr, "airdour_win_shell: smoke-midi-chord FAILED (clear)\n");
					std::fflush (stderr);
					return false;
				}
			}
		}

		const bool snap_was = _snap.snap_enabled;
		const int dens_was = _snap.snap_density;
		const int chord_was = _snap.midi_chord_quality;
		set_snap_density (1);
		set_snap_enabled (false);
		set_midi_step_enabled (true);

		auto fail = [&] (const char* why) {
			set_midi_chord_quality (chord_was);
			set_midi_step_enabled (false);
			set_snap_enabled (snap_was);
			set_snap_density (dens_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-chord FAILED (%s)\n", why);
			std::fflush (stderr);
			return false;
		};

		if (!locate_to (0.f, false, 0.f)) {
			return fail ("locate 0");
		}
		float unit = snap_unit_sec ();

		/* Maj triad at t=0 from root 60 → 60,64,67; advance once. */
		set_midi_chord_quality (0);
		if (cycle_midi_chord_quality () != 1) {
			return fail ("cycle→maj");
		}
		if (midi_chord_quality () != 1) {
			return fail ("set maj");
		}
		if (!step_input_midi_note (60, 100, true)) {
			return fail ("maj place");
		}
		refresh_midi_notes_snap ();
		bool got60 = false, got64 = false, got67 = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			if (n.start_sec < -0.05f || n.start_sec > 0.08f) {
				continue;
			}
			if (n.pitch == 60) {
				got60 = true;
			}
			if (n.pitch == 64) {
				got64 = true;
			}
			if (n.pitch == 67) {
				got67 = true;
			}
		}
		if (!got60 || !got64 || !got67) {
			return fail ("maj verify");
		}
		if (std::fabs (_snap.transport_sec - unit) > 0.08f) {
			return fail ("maj advance");
		}

		/* Min triad at beat from root 60 → 60,63,67. */
		set_midi_chord_quality (2);
		if (!step_input_midi_note (60, 100, true)) {
			return fail ("min place");
		}
		refresh_midi_notes_snap ();
		bool m60 = false, m63 = false, m67 = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			if (n.start_sec < unit - 0.08f || n.start_sec > unit + 0.08f) {
				continue;
			}
			if (n.pitch == 60) {
				m60 = true;
			}
			if (n.pitch == 63) {
				m63 = true;
			}
			if (n.pitch == 67) {
				m67 = true;
			}
		}
		if (!m60 || !m63 || !m67) {
			return fail ("min verify");
		}

		/* Off + multi-key latch: two pitches at same start, advance once after. */
		set_midi_chord_quality (0);
		float multi_t = _snap.transport_sec;
		if (!step_input_midi_note (62, 100, false)) {
			return fail ("multi D");
		}
		if (!step_input_midi_note (65, 100, false)) {
			return fail ("multi F");
		}
		if (std::fabs (_snap.transport_sec - multi_t) > 0.05f) {
			return fail ("multi no-advance");
		}
		if (!step_advance (1)) {
			return fail ("multi advance");
		}
		refresh_midi_notes_snap ();
		bool d_ok = false, f_ok = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			if (n.start_sec < multi_t - 0.08f || n.start_sec > multi_t + 0.08f) {
				continue;
			}
			if (n.pitch == 62) {
				d_ok = true;
			}
			if (n.pitch == 65) {
				f_ok = true;
			}
		}
		if (!d_ok || !f_ok) {
			return fail ("multi verify");
		}

		set_midi_chord_quality (chord_was);
		set_midi_step_enabled (false);
		set_snap_enabled (snap_was);
		set_snap_density (dens_was);

		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-chord ok (notes=%d unit=%.3f)\n",
		              _snap.midi_note_count, unit);
		std::fflush (stderr);
		_snap.detail = L"smoke-midi-chord ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-chord exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

void
EngineHost::set_midi_arp_enabled (bool on)
{
	if (!on && _arp_enabled) {
		arp_silence ();
	}
	_arp_enabled = on;
	_snap.midi_arp_enabled = on;
	if (on) {
		_arp_step = 0;
		_arp_last_grid = -1;
		rebuild_arp_pitches ();
		_snap.detail = L"MIDI arp on";
	} else {
		_snap.detail = L"MIDI arp off";
	}
}

bool
EngineHost::midi_arp_enabled () const
{
	return _arp_enabled;
}

bool
EngineHost::toggle_midi_arp_enabled ()
{
	set_midi_arp_enabled (!_arp_enabled);
	return _arp_enabled;
}

void
EngineHost::set_midi_arp_mode (int mode)
{
	if (mode < 0) {
		mode = 0;
	}
	if (mode > 2) {
		mode = 2;
	}
	_arp_mode = mode;
	_snap.midi_arp_mode = mode;
	_arp_step = 0;
	_arp_last_grid = -1;
	if (mode == 1) {
		_snap.detail = L"MIDI arp Down";
	} else if (mode == 2) {
		_snap.detail = L"MIDI arp UpDown";
	} else {
		_snap.detail = L"MIDI arp Up";
	}
}

int
EngineHost::midi_arp_mode () const
{
	return _arp_mode;
}

int
EngineHost::cycle_midi_arp_mode ()
{
	int m = _arp_mode + 1;
	if (m > 2) {
		m = 0;
	}
	set_midi_arp_mode (m);
	return _arp_mode;
}

void
EngineHost::set_midi_arp_held_pitches (const uint8_t* pitches, int count)
{
	_arp_held_count = 0;
	if (!pitches || count <= 0) {
		return;
	}
	if (count > kMaxArpPitches) {
		count = kMaxArpPitches;
	}
	for (int i = 0; i < count; ++i) {
		uint8_t p = pitches[i];
		if (p > 127) {
			p = 127;
		}
		bool dup = false;
		for (int j = 0; j < _arp_held_count; ++j) {
			if (_arp_held[j] == p) {
				dup = true;
				break;
			}
		}
		if (!dup) {
			_arp_held[_arp_held_count++] = p;
		}
	}
	std::sort (_arp_held, _arp_held + _arp_held_count);
}

void
EngineHost::clear_midi_arp_held_pitches ()
{
	_arp_held_count = 0;
}

void
EngineHost::arp_silence ()
{
	if (_arp_sounding >= 0) {
		midi_immediate_note (0x80, (uint8_t)_arp_sounding, 0);
		_arp_sounding = -1;
		_snap.midi_arp_pitch = -1;
	}
}

bool
EngineHost::midi_immediate_note (uint8_t status, uint8_t pitch, uint8_t velocity)
{
	if (!_impl || !_impl->midi_track) {
		return false;
	}
	if (pitch > 127) {
		pitch = 127;
	}
	if (velocity > 127) {
		velocity = 127;
	}
	uint8_t ev[3] = { (uint8_t)(status & 0xf0), pitch, velocity };
	try {
		return _impl->midi_track->write_immediate_event (Evoral::MIDI_EVENT, 3, ev);
	} catch (...) {
		return false;
	}
}

int
EngineHost::arp_pitch_at_step (int step) const
{
	const int n = _arp_pitch_count;
	if (n <= 0) {
		return -1;
	}
	if (n == 1) {
		return (int)_arp_pitches[0];
	}
	if (_arp_mode == 0) {
		return (int)_arp_pitches[step % n];
	}
	if (_arp_mode == 1) {
		return (int)_arp_pitches[(n - 1) - (step % n)];
	}
	/* Up-down (exclude duplicate ends). */
	const int period = 2 * (n - 1);
	int k = step % period;
	if (k < 0) {
		k += period;
	}
	if (k < n) {
		return (int)_arp_pitches[k];
	}
	return (int)_arp_pitches[period - k];
}

void
EngineHost::rebuild_arp_pitches ()
{
	_arp_pitch_count = 0;
	auto push_unique = [&] (uint8_t p) {
		if (p > 127) {
			p = 127;
		}
		for (int i = 0; i < _arp_pitch_count; ++i) {
			if (_arp_pitches[i] == p) {
				return;
			}
		}
		if (_arp_pitch_count < kMaxArpPitches) {
			_arp_pitches[_arp_pitch_count++] = p;
		}
	};

	/* 1) Shell-held computer-key chord. */
	if (_arp_held_count > 0) {
		for (int i = 0; i < _arp_held_count; ++i) {
			push_unique (_arp_held[i]);
		}
	}

	/* 2) Chord cluster around selected note start. */
	if (_arp_pitch_count == 0 && _snap.midi_note_count > 0) {
		int sel = -1;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].selected) {
				sel = i;
				break;
			}
		}
		if (sel >= 0) {
			const float t0 = _snap.midi_notes[sel].start_sec;
			for (int i = 0; i < _snap.midi_note_count; ++i) {
				if (std::fabs (_snap.midi_notes[i].start_sec - t0) <= 0.04f) {
					push_unique (_snap.midi_notes[i].pitch);
				}
			}
		}
	}

	/* 3) Notes under playhead. */
	if (_arp_pitch_count == 0 && _snap.midi_note_count > 0) {
		const float t = _snap.transport_sec;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			if (t >= n.start_sec - 0.01f && t < n.start_sec + n.length_sec + 0.01f) {
				push_unique (n.pitch);
			}
		}
	}

	/* 4) All unique region pitches (capped). */
	if (_arp_pitch_count == 0 && _snap.midi_note_count > 0) {
		for (int i = 0; i < _snap.midi_note_count && _arp_pitch_count < 8; ++i) {
			push_unique (_snap.midi_notes[i].pitch);
		}
	}

	/* 5) Chord-quality expand single root, else default Cmaj. */
	if (_arp_pitch_count == 1 && _snap.midi_chord_quality != 0) {
		const uint8_t root = _arp_pitches[0];
		_arp_pitch_count = 0;
		push_unique (root);
		if (_snap.midi_chord_quality == 1) {
			if ((int)root + 4 <= 127) {
				push_unique ((uint8_t)(root + 4));
			}
			if ((int)root + 7 <= 127) {
				push_unique ((uint8_t)(root + 7));
			}
		} else if (_snap.midi_chord_quality == 2) {
			if ((int)root + 3 <= 127) {
				push_unique ((uint8_t)(root + 3));
			}
			if ((int)root + 7 <= 127) {
				push_unique ((uint8_t)(root + 7));
			}
		}
	}
	if (_arp_pitch_count == 0) {
		push_unique (60);
		push_unique (64);
		push_unique (67);
	}
	std::sort (_arp_pitches, _arp_pitches + _arp_pitch_count);
}

bool
EngineHost::tick_arpeggiator (bool force)
{
	_snap.midi_arp_enabled = _arp_enabled;
	_snap.midi_arp_mode = _arp_mode;

	if (!_arp_enabled) {
		arp_silence ();
		return false;
	}
	if (!force && !_snap.transport_rolling) {
		arp_silence ();
		_arp_last_grid = -1;
		return false;
	}

	rebuild_arp_pitches ();
	if (_arp_pitch_count <= 0) {
		arp_silence ();
		return false;
	}

	float unit = snap_unit_sec ();
	if (unit < 0.05f) {
		unit = 0.05f;
	}

	int grid = _arp_last_grid;
	if (force) {
		grid = _arp_last_grid + 1;
		if (_arp_last_grid < 0) {
			grid = 0;
		}
	} else {
		float t = _snap.transport_sec;
		if (t < 0.f) {
			t = 0.f;
		}
		grid = (int)std::floor ((double)t / (double)unit + 1e-6);
	}
	if (grid == _arp_last_grid && !force) {
		_snap.midi_arp_pitch = _arp_sounding;
		return true;
	}
	_arp_last_grid = grid;
	if (!force) {
		_arp_step = grid;
	} else {
		_arp_step = grid;
	}

	const int pitch = arp_pitch_at_step (_arp_step);
	if (pitch < 0) {
		arp_silence ();
		return false;
	}

	if (_arp_sounding >= 0 && _arp_sounding != pitch) {
		midi_immediate_note (0x80, (uint8_t)_arp_sounding, 0);
	} else if (_arp_sounding >= 0 && _arp_sounding == pitch) {
		/* Retrigger same pitch for rhythmic pulse. */
		midi_immediate_note (0x80, (uint8_t)_arp_sounding, 0);
	}
	const bool ok = midi_immediate_note (0x90, (uint8_t)pitch, 100);
	_arp_sounding = pitch;
	_snap.midi_arp_pitch = pitch;
	if (_impl) {
		_impl->midi_activity = 1.f;
	}
	return ok;
}

bool
EngineHost::smoke_midi_arp ()
{
	if (!_impl || !_impl->session || !_impl->engine) {
		_snap.detail = L"smoke-midi-arp failed — no Session";
		return false;
	}
	try {
		refresh_midi_snap ();
		if (!ensure_midi_track () || !_snap.midi_track_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-arp FAILED (track)\n");
			std::fflush (stderr);
			return false;
		}
		if (!ensure_midi_region (16.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-arp FAILED (region)\n");
			std::fflush (stderr);
			return false;
		}
		clear_midi_note_selection ();
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count > 0) {
			std::shared_ptr<Playlist> playlist = _impl->midi_track
			                                        ? _impl->midi_track->playlist ()
			                                        : std::shared_ptr<Playlist> ();
			if (playlist && _impl->midi_region) {
				_impl->session->begin_reversible_command ("clear midi for arp smoke");
				playlist->clear_changes ();
				playlist->clear_owned_changes ();
				playlist->remove_region (_impl->midi_region);
				playlist->rdiff_and_add_command (_impl->session);
				_impl->session->commit_reversible_command ();
				_impl->midi_region.reset ();
				_impl->selected_midi_note_id = -1;
				if (!ensure_midi_region (16.f)) {
					std::fprintf (stderr, "airdour_win_shell: smoke-midi-arp FAILED (clear)\n");
					std::fflush (stderr);
					return false;
				}
			}
		}

		attach_midi_synth ();
		set_snap_density (1);
		set_snap_enabled (false);
		clear_midi_arp_held_pitches ();

		if (!add_midi_note (60, 0.0f, 2.0f, 100)
		    || !add_midi_note (64, 0.0f, 2.0f, 100)
		    || !add_midi_note (67, 0.0f, 2.0f, 100)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-arp FAILED (add chord)\n");
			std::fflush (stderr);
			return false;
		}
		refresh_midi_notes_snap ();
		/* Select middle note so cluster at t=0 is the arp source. */
		if (!select_midi_note_at (0.1f, 64)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-arp FAILED (select)\n");
			std::fflush (stderr);
			return false;
		}

		set_midi_arp_mode (0);
		set_midi_arp_enabled (true);
		if (!midi_arp_enabled ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-arp FAILED (enable)\n");
			std::fflush (stderr);
			return false;
		}

		int got_up[3] = { -1, -1, -1 };
		for (int i = 0; i < 3; ++i) {
			if (!tick_arpeggiator (true)) {
				set_midi_arp_enabled (false);
				std::fprintf (stderr, "airdour_win_shell: smoke-midi-arp FAILED (up tick %d)\n", i);
				std::fflush (stderr);
				return false;
			}
			got_up[i] = _snap.midi_arp_pitch;
		}
		if (got_up[0] != 60 || got_up[1] != 64 || got_up[2] != 67) {
			set_midi_arp_enabled (false);
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-midi-arp FAILED (up got %d %d %d)\n",
			              got_up[0], got_up[1], got_up[2]);
			std::fflush (stderr);
			return false;
		}

		set_midi_arp_mode (1);
		int got_dn[3] = { -1, -1, -1 };
		for (int i = 0; i < 3; ++i) {
			if (!tick_arpeggiator (true)) {
				set_midi_arp_enabled (false);
				std::fprintf (stderr, "airdour_win_shell: smoke-midi-arp FAILED (dn tick %d)\n", i);
				std::fflush (stderr);
				return false;
			}
			got_dn[i] = _snap.midi_arp_pitch;
		}
		if (got_dn[0] != 67 || got_dn[1] != 64 || got_dn[2] != 60) {
			set_midi_arp_enabled (false);
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-midi-arp FAILED (dn got %d %d %d)\n",
			              got_dn[0], got_dn[1], got_dn[2]);
			std::fflush (stderr);
			return false;
		}

		/* UpDown: 60,64,67,64 */
		set_midi_arp_mode (2);
		int got_ud[4] = { -1, -1, -1, -1 };
		for (int i = 0; i < 4; ++i) {
			if (!tick_arpeggiator (true)) {
				set_midi_arp_enabled (false);
				std::fprintf (stderr, "airdour_win_shell: smoke-midi-arp FAILED (ud tick %d)\n", i);
				std::fflush (stderr);
				return false;
			}
			got_ud[i] = _snap.midi_arp_pitch;
		}
		if (got_ud[0] != 60 || got_ud[1] != 64 || got_ud[2] != 67 || got_ud[3] != 64) {
			set_midi_arp_enabled (false);
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-midi-arp FAILED (ud got %d %d %d %d)\n",
			              got_ud[0], got_ud[1], got_ud[2], got_ud[3]);
			std::fflush (stderr);
			return false;
		}

		set_midi_arp_enabled (false);
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-arp ok (up/dn/ud pitches verified)\n");
		std::fflush (stderr);
		_snap.detail = L"smoke-midi-arp ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-arp exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}

bool
EngineHost::smoke_midi_scale ()
{
	if (!_impl || !_impl->session || !_impl->engine) {
		_snap.detail = L"smoke-midi-scale failed — no Session";
		return false;
	}
	try {
		refresh_midi_snap ();
		if (!ensure_midi_track () || !_snap.midi_track_ok) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-scale FAILED (track)\n");
			std::fflush (stderr);
			return false;
		}
		if (!ensure_midi_region (16.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-scale FAILED (region)\n");
			std::fflush (stderr);
			return false;
		}
		clear_midi_note_selection ();
		refresh_midi_notes_snap ();
		if (_snap.midi_note_count > 0) {
			std::shared_ptr<Playlist> playlist = _impl->midi_track
			                                        ? _impl->midi_track->playlist ()
			                                        : std::shared_ptr<Playlist> ();
			if (playlist && _impl->midi_region) {
				_impl->session->begin_reversible_command ("clear midi for scale smoke");
				playlist->clear_changes ();
				playlist->clear_owned_changes ();
				playlist->remove_region (_impl->midi_region);
				playlist->rdiff_and_add_command (_impl->session);
				_impl->session->commit_reversible_command ();
				_impl->midi_region.reset ();
				_impl->selected_midi_note_id = -1;
				if (!ensure_midi_region (16.f)) {
					std::fprintf (stderr, "airdour_win_shell: smoke-midi-scale FAILED (clear)\n");
					std::fflush (stderr);
					return false;
				}
			}
		}

		const bool snap_was = _snap.snap_enabled;
		const int dens_was = _snap.snap_density;
		const int chord_was = _snap.midi_chord_quality;
		const bool lock_was = _snap.midi_scale_lock;
		const int kind_was = _snap.midi_scale_kind;
		const int root_was = _snap.midi_scale_root;

		auto fail = [&] (const char* why) {
			set_midi_chord_quality (chord_was);
			set_midi_scale_lock (lock_was);
			set_midi_scale_kind (kind_was);
			set_midi_scale_root (root_was);
			set_snap_enabled (snap_was);
			set_snap_density (dens_was);
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-scale FAILED (%s)\n", why);
			std::fflush (stderr);
			return false;
		};

		set_snap_density (1);
		set_snap_enabled (false);
		set_midi_chord_quality (0);
		set_midi_scale_root (0); /* C */
		set_midi_scale_kind (0); /* major */
		set_midi_scale_lock (true);
		if (!midi_scale_lock () || snap_pitch_to_scale (61) != 62) {
			return fail ("C maj C#→D");
		}
		if (snap_pitch_to_scale (60) != 60) {
			return fail ("C stays");
		}

		if (!locate_to (0.f, false, 0.f)) {
			return fail ("locate 0");
		}
		/* C# root → D; place single note. */
		if (!step_input_midi_note (61, 100, true)) {
			return fail ("step C#");
		}
		refresh_midi_notes_snap ();
		bool got_d = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].pitch == 62
			    && _snap.midi_notes[i].start_sec >= -0.05f
			    && _snap.midi_notes[i].start_sec <= 0.08f) {
				got_d = true;
			}
		}
		if (!got_d) {
			return fail ("verify D");
		}

		/* Chord maj from snapped C#→D: D F# A = 62,66,69 */
		set_midi_chord_quality (1);
		float unit = snap_unit_sec ();
		if (!step_input_midi_note (61, 100, true)) {
			return fail ("chord step");
		}
		refresh_midi_notes_snap ();
		bool d2 = false, fs = false, a = false;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _snap.midi_notes[i];
			if (n.start_sec < unit - 0.08f || n.start_sec > unit + 0.08f) {
				continue;
			}
			if (n.pitch == 62) {
				d2 = true;
			}
			if (n.pitch == 66) {
				fs = true;
			}
			if (n.pitch == 69) {
				a = true;
			}
		}
		if (!d2 || !fs || !a) {
			return fail ("chord tones leave scale");
		}

		/* Lock off: C# stays C#. */
		set_midi_chord_quality (0);
		set_midi_scale_lock (false);
		if (snap_pitch_to_scale (61) != 61) {
			return fail ("lock off");
		}
		if (!step_input_midi_note (61, 100, false)) {
			return fail ("passthrough");
		}
		refresh_midi_notes_snap ();
		bool got_cs = false;
		float t = _snap.transport_sec;
		for (int i = 0; i < _snap.midi_note_count; ++i) {
			if (_snap.midi_notes[i].pitch == 61
			    && std::fabs (_snap.midi_notes[i].start_sec - t) < 0.08f) {
				got_cs = true;
			}
		}
		if (!got_cs) {
			return fail ("verify C#");
		}

		/* Natural minor A: kind cycle + root. */
		set_midi_scale_root (9); /* A */
		set_midi_scale_kind (0);
		if (cycle_midi_scale_kind () != 1) {
			return fail ("cycle→min");
		}
		set_midi_scale_lock (true);
		/* Bb (70) nearest in A natural minor {9,11,0,2,4,5,7} → A(69) or B(71); tie→up B */
		if (snap_pitch_to_scale (70) != 71) {
			return fail ("A min Bb→B");
		}

		set_midi_chord_quality (chord_was);
		set_midi_scale_lock (lock_was);
		set_midi_scale_kind (kind_was);
		set_midi_scale_root (root_was);
		set_snap_enabled (snap_was);
		set_snap_density (dens_was);

		std::fprintf (stderr, "airdour_win_shell: smoke-midi-scale ok\n");
		std::fflush (stderr);
		_snap.detail = L"smoke-midi-scale ok";
		return true;
	} catch (std::exception& e) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-scale exception: %s\n", e.what ());
		std::fflush (stderr);
		return false;
	} catch (...) {
		return false;
	}
}


void
EngineHost::refresh_snapshot ()
{
	_snap.gain = _gain;
	if (!_impl || !_impl->engine || !_started) {
		return;
	}
	try {
		_snap.available = true;
		_snap.running = _impl->engine->running ();
		_snap.backend = utf8_to_wide (_impl->engine->current_backend_name ());
		_snap.sample_rate = (uint32_t)_impl->engine->sample_rate ();
		_snap.period_frames = (uint32_t)_impl->engine->samples_per_cycle ();
		_snap.dsp_load_pct = _impl->engine->get_dsp_load ();
		_snap.callback_last_us = 0;
		_snap.callback_max_us = 0;
		_snap.callback_avg_us = 0;
		_snap.callback_count = 0;
		_snap.xrun_count = 0;
		if (std::shared_ptr<AudioBackend> be = _impl->engine->current_backend ()) {
			uint64_t be_xruns = 0;
			be->process_callback_stats (_snap.callback_last_us, _snap.callback_max_us,
			                            _snap.callback_avg_us, _snap.callback_count,
			                            be_xruns);
			_snap.xrun_count = be_xruns;
		}
		if (_impl->session) {
			_snap.xrun_count += (uint64_t)_impl->session->get_xrun_count ();
		}
		_snap.session_ok = (_impl->session != nullptr);

		if (_impl->session) {
			_snap.transport_rolling = _impl->session->transport_rolling ();
			samplecnt_t sr = _impl->session->sample_rate ();
			if (sr > 0) {
				_snap.transport_sec = (float)_impl->session->transport_sample () / (float)sr;
			}
			read_meters ();
			_snap.waves_from_session = true;
			refresh_region_flags ();
			refresh_record_flags ();
			refresh_mute_solo_flags ();
			refresh_pan_flags ();
			refresh_send_flags ();
			refresh_group_flags ();
			refresh_monitor_flags ();
			refresh_history_flags ();
			refresh_selection_snap ();
			refresh_locations_snap ();
			refresh_tempo_snap ();
			refresh_clock_snap ();
			refresh_midi_snap ();
			refresh_track_fx_snap ();
			refresh_track_comp_snap ();
			refresh_track_eq_snap ();
			refresh_track_delay_snap ();
			refresh_track_limiter_snap ();
			refresh_track_gate_snap ();
			refresh_spectrum_snap ();
			refresh_correlation_snap ();
			refresh_loudness_snap ();
		} else {
			_snap.waves_from_session = false;
			_snap.session_record_armed = false;
			_snap.session_recording = false;
			_snap.undo_depth = 0;
			_snap.redo_depth = 0;
			_snap.has_selection = false;
			_snap.selection_track = -1;
			_snap.loop_active = false;
			_snap.loop_playing = false;
			_snap.marker_count = 0;
			for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
				_snap.track_has_regions[i] = false;
				_snap.track_armed[i] = false;
				_snap.track_frozen[i] = false;
			}
			/* Closest honest engine path without Session: AudioEngine sample clock. */
			_snap.transport_rolling = _impl->transport_playing;
			samplecnt_t sr = (samplecnt_t)_impl->engine->sample_rate ();
			if (_impl->transport_playing && sr > 0) {
				samplecnt_t now = _impl->engine->processed_samples ();
				if (now < _impl->play_origin_samples) {
					_impl->play_origin_samples = now;
				}
				_snap.transport_sec = (float)(now - _impl->play_origin_samples) / (float)sr;
			} else if (!_impl->transport_playing) {
				_snap.transport_sec = 0.f;
			}
		}
	} catch (...) {
		/* RT/UI races must not take down the shell. */
	}
}

void
EngineHost::poll ()
{
	refresh_snapshot ();
	tick_arpeggiator (false);
}

EngineSnapshot
EngineHost::snapshot () const
{
	return _snap;
}

#else /* !AIRDOUR_WIN_SHELL_WITH_ENGINE */

EngineHost::EngineHost ()
	: _started (false)
	, _backend_name ()
	, _period_frames (0)
	, _pref_backend ("Dummy")
	, _pref_period_frames (0)
	, _pref_snap_default (false)
	, _pref_click_default (true)
	, _autosave_enabled (false)
	, _last_autosave_ms (0)
	, _recovery_saved_ft (0)
	, _recovery_last_open_ft (0)
	, _recovery_dirty (false)
	, _gain (0.85f)
	, _arp_enabled (false)
	, _arp_mode (0)
	, _arp_step (0)
	, _arp_last_grid (-1)
	, _arp_sounding (-1)
	, _arp_pitch_count (0)
	, _arp_held_count (0)
{
	for (int i = 0; i < kMaxArpPitches; ++i) {
		_arp_pitches[i] = 0;
		_arp_held[i] = 0;
	}
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_track_gain[i] = 0.85f;
		_snap.meter_peak[i] = 0.f;
		_snap.meter_clip[i] = false;
		_snap.track_has_regions[i] = false;
		_snap.track_armed[i] = false;
		_snap.track_frozen[i] = false;
		_snap.track_send[i] = 0.f;
		_snap.track_send_ok[i] = false;
		_snap.track_gain[i] = _track_gain[i];
		_snap.track_name[i][0] = L'\0';
		_snap.track_in_group[i] = false;
	}
	_snap.midi_track_name[0] = L'\0';
	_snap.group_member_count = 0;
	_snap.group_active = false;
	_snap.group_shares_mute = false;
	_snap.group_shares_solo = false;
	_snap.group_shares_gain = false;
	_snap.group_name[0] = L'\0';
	_snap.monitor_ok = false;
	_snap.monitor_dim = false;
	_snap.monitor_mono = false;
	_snap.monitor_cut = false;
	_snap.transport_speed = 0.f;
	_snap.bbt_bars = 1;
	_snap.bbt_beats = 1;
	_snap.bbt_ticks = 0;
	_snap.tc_hours = 0;
	_snap.tc_minutes = 0;
	_snap.tc_seconds = 0;
	_snap.tc_frames = 0;
	_snap.pref_backend = L"Dummy";
	_snap.pref_period_frames = 0;
	_snap.pref_snap_default = false;
	_snap.pref_click_default = true;
	_snap.vst3_path_hint[0] = L'\0';
	_snap.recent_count = 0;
	for (int i = 0; i < EngineSnapshot::kMaxRecent; ++i) {
		_snap.recent_label[i][0] = L'\0';
		_snap.recent_path[i][0] = L'\0';
	}
	_snap.aux_bus_ok = false;
	_snap.midi_arp_enabled = false;
	_snap.midi_arp_mode = 0;
	_snap.midi_arp_pitch = -1;
	_snap.midi_scale_lock = false;
	_snap.midi_scale_kind = 0;
	_snap.midi_scale_root = 0;
	_snap.available = false;
	_snap.running = false;
	_snap.session_ok = false;
	_snap.transport_rolling = false;
	_snap.meters_real = false;
	_snap.waves_from_session = false;
	_snap.session_record_armed = false;
	_snap.session_recording = false;
	_snap.session_saved = false;
	_snap.export_ok = false;
	_snap.undo_depth = 0;
	_snap.redo_depth = 0;
	_snap.undo_list_count = 0;
	_snap.next_undo_name[0] = L'\0';
	_snap.next_redo_name[0] = L'\0';
	for (int i = 0; i < EngineSnapshot::kMaxHistory; ++i) {
		_snap.undo_list_name[i][0] = L'\0';
	}
	_snap.has_selection = false;
	_snap.selection_track = -1;
	_snap.selection_start_sec = 0.f;
	_snap.selection_end_sec = 0.f;
	_snap.selection_fade_in_sec = 0.f;
	_snap.selection_fade_out_sec = 0.f;
	_snap.snap_enabled = false;
	_snap.midi_step_enabled = false;
	_snap.midi_chord_quality = 0;
	_snap.midi_arp_enabled = false;
	_snap.midi_arp_mode = 0;
	_snap.midi_arp_pitch = -1;
	_snap.midi_scale_lock = false;
	_snap.midi_scale_kind = 0;
	_snap.midi_scale_root = 0;
	_snap.snap_density = 1;
	_snap.grid_line_count = 0;
	for (int i = 0; i < EngineSnapshot::kMaxGridLines; ++i) {
		_snap.grid_line_sec[i] = 0.f;
		_snap.grid_line_bar[i] = false;
	}
	_snap.loop_active = false;
	_snap.loop_playing = false;
	_snap.loop_start_sec = 0.f;
	_snap.loop_end_sec = 0.f;
	_snap.punch_range_ok = false;
	_snap.punch_enabled = false;
	_snap.punch_start_sec = 0.f;
	_snap.punch_end_sec = 0.f;
	_snap.tempo_bpm = 120.f;
	_snap.count_in_enabled = false;
	_snap.click_enabled = false;
	_snap.audio_track_count = 0;
	_snap.midi_track_count = 0;
	_snap.marker_count = 0;
	for (int i = 0; i < EngineSnapshot::kMaxMarkers; ++i) {
		_snap.markers[i].sec = 0.f;
		_snap.markers[i].name[0] = L'\0';
	}
	_snap.midi_ok = false;
	_snap.midi_track_ok = false;
	_snap.midi_region_ok = false;
	_snap.midi_synth_ok = false;
	_snap.midi_synth_to_master = false;
	_snap.midi_out_connected = false;
	_snap.track_fx_ok = false;
	_snap.track_fx_bypassed = false;
	_snap.track_fx_param = 0.3f;
	_snap.track_comp_ok = false;
	_snap.track_comp_bypassed = false;
	_snap.track_comp_threshold_db = 0.f;
	_snap.track_comp_ratio = 4.f;
	_snap.track_comp_threshold_norm = 1.f;
	_snap.track_comp_ratio_norm = (4.f - 1.f) / 19.f;
	_snap.track_comp_sidechain = false;
	_snap.track_comp_sidechain_src = -1;
	_snap.track_eq_ok = false;
	_snap.track_eq_bypassed = false;
	_snap.track_eq_gain_l_db = 0.f;
	_snap.track_eq_gain_m_db = 0.f;
	_snap.track_eq_gain_h_db = 0.f;
	_snap.track_eq_gain_l_norm = 0.5f;
	_snap.track_eq_gain_m_norm = 0.5f;
	_snap.track_eq_gain_h_norm = 0.5f;
	_snap.track_delay_ok = false;
	_snap.track_delay_bypassed = false;
	_snap.track_delay_time_ms = 160.f;
	_snap.track_delay_feedback_pct = 20.f;
	_snap.track_delay_time_norm = std::log (160.f) / std::log (8000.f);
	_snap.track_delay_feedback_norm = 0.2f;
	_snap.track_limiter_ok = false;
	_snap.track_limiter_bypassed = false;
	_snap.track_limiter_ceiling_db = -0.3f;
	_snap.track_limiter_release_ms = 100.f;
	_snap.track_limiter_ceiling_norm = (-0.3f - (-20.f)) / 20.f;
	_snap.track_limiter_release_norm = (100.f - 1.f) / 1999.f;
	_snap.track_gate_ok = false;
	_snap.track_gate_bypassed = false;
	_snap.track_gate_threshold_db = -40.f;
	_snap.track_gate_hold_ms = 50.f;
	_snap.track_gate_threshold_norm = (-40.f - (-60.f)) / 60.f;
	_snap.track_gate_hold_norm = (50.f - 1.f) / 499.f;
	_snap.spectrum_enabled = false;
	_snap.spectrum_ok = false;
	_snap.spectrum_master = false;
	_snap.spectrum_track = 0;
	for (int si = 0; si < EngineSnapshot::kSpectrumBands; ++si) {
		_snap.spectrum_band[si] = 0.f;
	}
	_snap.spectrum_label[0] = L'\0';
	_snap.correlation_enabled = false;
	_snap.correlation_ok = false;
	_snap.correlation_master = false;
	_snap.correlation_track = 0;
	_snap.correlation = 0.f;
	for (int gi = 0; gi < EngineSnapshot::kGonioPts; ++gi) {
		_snap.gonio_x[gi] = 0.f;
		_snap.gonio_y[gi] = 0.f;
	}
	_snap.correlation_label[0] = L'\0';
	_snap.loudness_enabled = false;
	_snap.loudness_ok = false;
	_snap.loudness_master = false;
	_snap.loudness_track = 0;
	_snap.loudness_momentary = -70.f;
	_snap.loudness_integrated = -70.f;
	_snap.loudness_label[0] = L'\0';
	_snap.midi_activity = 0.f;
	_snap.dsp_load_pct = 0.f;
	_snap.callback_last_us = 0;
	_snap.callback_max_us = 0;
	_snap.callback_avg_us = 0;
	_snap.callback_count = 0;
	_snap.xrun_count = 0;
	_snap.midi_device_count = 0;
	for (int i = 0; i < EngineSnapshot::kMaxMidiDevices; ++i) {
		_snap.midi_devices[i].name[0] = L'\0';
		_snap.midi_devices[i].input = false;
	}
	_snap.midi_note_count = 0;
	for (int i = 0; i < EngineSnapshot::kMaxMidiNotes; ++i) {
		_snap.midi_notes[i].id = -1;
		_snap.midi_notes[i].start_sec = 0.f;
		_snap.midi_notes[i].length_sec = 0.f;
		_snap.midi_notes[i].pitch = 0;
		_snap.midi_notes[i].velocity = 0;
		_snap.midi_notes[i].selected = false;
	}
	_snap.midi_selected_note_id = -1;
	_snap.midi_option.clear ();
	_snap.midi_synth_name.clear ();
	_snap.midi_out_name.clear ();
	_snap.track_fx_name.clear ();
	_snap.track_fx_param_name = L"Blend";
	_snap.track_comp_name.clear ();
	_snap.track_eq_name.clear ();
	_snap.track_delay_name.clear ();
	_snap.track_limiter_name.clear ();
	_snap.track_gate_name.clear ();
	_snap.sample_rate = 0;
	_snap.period_frames = 0;
	_snap.gain = _gain;
	_snap.transport_sec = 0.f;
	_snap.backend = L"(not linked)";
	_snap.session_path.clear ();
	_snap.export_path.clear ();
	_snap.detail = L"build with AIRDOUR_WIN_SHELL_WITH_ENGINE + waf libs";
	_last_export_path.clear ();
}

EngineHost::~EngineHost () = default;

bool
EngineHost::is_linked ()
{
	return false;
}

void
EngineHost::prepare (const std::string& backend_name)
{
	if (!backend_name.empty ()) {
		_backend_name = backend_name;
	}
	_snap.detail = L"engine support not compiled into this binary";
}

void
EngineHost::set_period_frames (uint32_t frames)
{
	_period_frames = frames;
}

void
EngineHost::refresh_prefs_snap ()
{
	_snap.pref_backend = (_pref_backend == "WASAPI") ? L"WASAPI" : L"Dummy";
	_snap.pref_period_frames = _pref_period_frames;
	_snap.pref_snap_default = _pref_snap_default;
	_snap.pref_click_default = _pref_click_default;
}

void
EngineHost::load_shell_prefs ()
{
	refresh_prefs_snap ();
}

bool
EngineHost::save_shell_prefs ()
{
	return false;
}

void
EngineHost::set_pref_backend (const std::string& name)
{
	if (name == "WASAPI" || name == "wasapi") {
		_pref_backend = "WASAPI";
	} else {
		_pref_backend = "Dummy";
	}
	refresh_prefs_snap ();
}

void
EngineHost::set_pref_period_frames (uint32_t frames)
{
	_pref_period_frames = frames;
	refresh_prefs_snap ();
}

void
EngineHost::set_pref_snap_default (bool on)
{
	_pref_snap_default = on;
	refresh_prefs_snap ();
}

bool
EngineHost::set_pref_click_default (bool on)
{
	_pref_click_default = on;
	refresh_prefs_snap ();
	return false;
}

bool
EngineHost::smoke_prefs ()
{
	return false;
}

void
EngineHost::refresh_recent_snap ()
{
	_snap.recent_count = 0;
}

void
EngineHost::load_recent_sessions ()
{
	refresh_recent_snap ();
}

bool
EngineHost::save_recent_sessions ()
{
	return false;
}

void
EngineHost::remember_recent_session (const std::string& session_dir,
                                     const std::string& snapshot,
                                     bool persist)
{
	(void)session_dir;
	(void)snapshot;
	(void)persist;
}

void
EngineHost::clear_recent_sessions (bool persist)
{
	(void)persist;
	_recent.clear ();
	refresh_recent_snap ();
}

int
EngineHost::recent_count () const
{
	return 0;
}

bool
EngineHost::recent_entry (int index, std::string* path, std::string* snapshot) const
{
	(void)index;
	(void)path;
	(void)snapshot;
	return false;
}

bool
EngineHost::open_recent (int index)
{
	(void)index;
	return false;
}

bool
EngineHost::smoke_recent ()
{
	return false;
}

void
EngineHost::set_autosave_enabled (bool on)
{
	_autosave_enabled = on;
}

void
EngineHost::autosave_tick (uint32_t now_ms)
{
	(void)now_ms;
}

bool
EngineHost::autosave_now ()
{
	return false;
}

bool
EngineHost::recovery_offer_available () const
{
	return false;
}

bool
EngineHost::recovery_offer_path (std::string* path, std::string* snapshot) const
{
	(void)path;
	(void)snapshot;
	return false;
}

bool
EngineHost::restore_recovery_session ()
{
	return false;
}

bool
EngineHost::discard_recovery_offer ()
{
	return false;
}

bool
EngineHost::mark_recovery_clean_open ()
{
	return false;
}

bool
EngineHost::smoke_recovery ()
{
	return false;
}

void
EngineHost::load_recovery_marker ()
{
}

bool
EngineHost::save_recovery_marker ()
{
	return false;
}

std::string
EngineHost::recovery_copy_path () const
{
	return std::string ();
}

bool
EngineHost::copy_snapshot_to_recovery ()
{
	return false;
}

uint64_t
EngineHost::snapshot_filetime (const std::string& session_dir, const std::string& snap) const
{
	(void)session_dir;
	(void)snap;
	return 0;
}

bool
EngineHost::start (const std::string& backend_name)
{
	prepare (backend_name);
	return start ();
}

bool
EngineHost::start ()
{
	_snap.detail = L"engine support not compiled into this binary";
	return false;
}

void
EngineHost::transport_play ()
{
}

void
EngineHost::transport_stop ()
{
}

void
EngineHost::stop ()
{
	_started = false;
	_snap.running = false;
}

void
EngineHost::set_gain (float linear)
{
	set_track_gain (0, linear);
}

void
EngineHost::set_track_gain (int track, float linear)
{
	linear = std::max (0.f, std::min (1.f, linear));
	if (track >= 0 && track < EngineSnapshot::kMeters) {
		_track_gain[track] = linear;
	}
	if (track == 0) {
		_gain = linear;
		_snap.gain = linear;
	}
}

float
EngineHost::gain () const
{
	return _gain;
}

void
EngineHost::refresh_snapshot ()
{
	_snap.gain = _gain;
}

void
EngineHost::poll ()
{
}

EngineSnapshot
EngineHost::snapshot () const
{
	return _snap;
}

bool
EngineHost::ensure_session ()
{
	return false;
}

void
EngineHost::destroy_session ()
{
}

void
EngineHost::read_meters ()
{
}

void
EngineHost::refresh_region_flags ()
{
}

void
EngineHost::refresh_freeze_flags ()
{
}

void
EngineHost::refresh_record_flags ()
{
}

void
EngineHost::refresh_mute_solo_flags ()
{
}

bool
EngineHost::ensure_freeze_session_range (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::set_midi_gain (float linear)
{
	(void)linear;
	return false;
}

bool
EngineHost::set_track_armed (int track, bool armed)
{
	(void)track;
	(void)armed;
	return false;
}

bool
EngineHost::toggle_track_armed (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::set_track_muted (int track, bool muted)
{
	(void)track;
	(void)muted;
	return false;
}

bool
EngineHost::toggle_track_muted (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::set_track_soloed (int track, bool soloed)
{
	(void)track;
	(void)soloed;
	return false;
}

bool
EngineHost::toggle_track_soloed (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::set_midi_muted (bool muted)
{
	(void)muted;
	return false;
}

bool
EngineHost::toggle_midi_muted ()
{
	return false;
}

bool
EngineHost::set_midi_soloed (bool soloed)
{
	(void)soloed;
	return false;
}

bool
EngineHost::toggle_midi_soloed ()
{
	return false;
}

bool
EngineHost::smoke_mute_solo ()
{
	return false;
}

void
EngineHost::refresh_pan_flags ()
{
}

bool
EngineHost::set_track_pan (int track, float pan_0_1)
{
	(void)track;
	(void)pan_0_1;
	return false;
}

float
EngineHost::track_pan (int track) const
{
	(void)track;
	return 0.5f;
}

bool
EngineHost::smoke_pan ()
{
	return false;
}

void
EngineHost::refresh_send_flags ()
{
}

bool
EngineHost::ensure_aux_sends ()
{
	return false;
}

bool
EngineHost::set_track_send (int track, float send_0_1)
{
	(void)track;
	(void)send_0_1;
	return false;
}

float
EngineHost::track_send (int track) const
{
	(void)track;
	return 0.f;
}

bool
EngineHost::smoke_send ()
{
	return false;
}

bool
EngineHost::ensure_track_gain_automation (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::write_track_gain_automation (int track, float time_sec, float value_0_1)
{
	(void)track;
	(void)time_sec;
	(void)value_0_1;
	return false;
}

bool
EngineHost::clear_track_gain_automation (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::fill_gain_automation_curve (int track,
                                        float view_start_sec,
                                        float view_span_sec,
                                        float* y_0_1,
                                        size_t n)
{
	(void)track;
	(void)view_start_sec;
	(void)view_span_sec;
	if (y_0_1) {
		for (size_t i = 0; i < n; ++i) {
			y_0_1[i] = 0.5f;
		}
	}
	return false;
}

int
EngineHost::fill_gain_automation_points (int track,
                                         float view_start_sec,
                                         float view_span_sec,
                                         float* x_norm,
                                         float* y_0_1,
                                         int max_n)
{
	(void)track;
	(void)view_start_sec;
	(void)view_span_sec;
	(void)x_norm;
	(void)y_0_1;
	(void)max_n;
	return 0;
}

bool
EngineHost::smoke_automation ()
{
	return false;
}

bool
EngineHost::ensure_midi_cc_automation (int cc, int channel)
{
	(void)cc;
	(void)channel;
	return false;
}

bool
EngineHost::write_midi_cc_automation (float time_sec, float value_0_1, int cc, int channel)
{
	(void)time_sec;
	(void)value_0_1;
	(void)cc;
	(void)channel;
	return false;
}

bool
EngineHost::clear_midi_cc_automation (int cc, int channel)
{
	(void)cc;
	(void)channel;
	return false;
}

bool
EngineHost::fill_midi_cc_automation_curve (float view_start_sec,
                                          float view_span_sec,
                                          float* y_0_1,
                                          size_t n,
                                          int cc,
                                          int channel)
{
	(void)view_start_sec;
	(void)view_span_sec;
	(void)cc;
	(void)channel;
	const float def = 100.f / 127.f;
	if (y_0_1) {
		for (size_t i = 0; i < n; ++i) {
			y_0_1[i] = def;
		}
	}
	return false;
}

int
EngineHost::fill_midi_cc_automation_points (float view_start_sec,
                                           float view_span_sec,
                                           float* x_norm,
                                           float* y_0_1,
                                           int max_n,
                                           int cc,
                                           int channel)
{
	(void)view_start_sec;
	(void)view_span_sec;
	(void)x_norm;
	(void)y_0_1;
	(void)max_n;
	(void)cc;
	(void)channel;
	return 0;
}

bool
EngineHost::smoke_midi_cc ()
{
	return false;
}

bool
EngineHost::ensure_midi_bend_automation (int channel)
{
	(void)channel;
	return false;
}

bool
EngineHost::write_midi_bend_automation (float time_sec, float value_0_1, int channel)
{
	(void)time_sec;
	(void)value_0_1;
	(void)channel;
	return false;
}

bool
EngineHost::clear_midi_bend_automation (int channel)
{
	(void)channel;
	return false;
}

bool
EngineHost::fill_midi_bend_automation_curve (float view_start_sec,
                                            float view_span_sec,
                                            float* y_0_1,
                                            size_t n,
                                            int channel)
{
	(void)view_start_sec;
	(void)view_span_sec;
	(void)channel;
	if (y_0_1) {
		for (size_t i = 0; i < n; ++i) {
			y_0_1[i] = 0.5f;
		}
	}
	return false;
}

int
EngineHost::fill_midi_bend_automation_points (float view_start_sec,
                                             float view_span_sec,
                                             float* x_norm,
                                             float* y_0_1,
                                             int max_n,
                                             int channel)
{
	(void)view_start_sec;
	(void)view_span_sec;
	(void)x_norm;
	(void)y_0_1;
	(void)max_n;
	(void)channel;
	return 0;
}

bool
EngineHost::smoke_midi_bend ()
{
	return false;
}

bool
EngineHost::ensure_midi_pressure_automation (int channel)
{
	(void)channel;
	return false;
}

bool
EngineHost::write_midi_pressure_automation (float time_sec, float value_0_1, int channel)
{
	(void)time_sec;
	(void)value_0_1;
	(void)channel;
	return false;
}

bool
EngineHost::clear_midi_pressure_automation (int channel)
{
	(void)channel;
	return false;
}

bool
EngineHost::fill_midi_pressure_automation_curve (float view_start_sec,
                                                float view_span_sec,
                                                float* y_0_1,
                                                size_t n,
                                                int channel)
{
	(void)view_start_sec;
	(void)view_span_sec;
	(void)channel;
	if (y_0_1) {
		for (size_t i = 0; i < n; ++i) {
			y_0_1[i] = 0.f;
		}
	}
	return false;
}

int
EngineHost::fill_midi_pressure_automation_points (float view_start_sec,
                                                 float view_span_sec,
                                                 float* x_norm,
                                                 float* y_0_1,
                                                 int max_n,
                                                 int channel)
{
	(void)view_start_sec;
	(void)view_span_sec;
	(void)x_norm;
	(void)y_0_1;
	(void)max_n;
	(void)channel;
	return 0;
}

bool
EngineHost::smoke_midi_pressure ()
{
	return false;
}

bool
EngineHost::clear_track_meter_clip (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::set_session_record (bool enable)
{
	(void)enable;
	return false;
}

bool
EngineHost::toggle_session_record ()
{
	return false;
}

bool
EngineHost::import_audio_file (int track, const std::string& path)
{
	(void)track;
	(void)path;
	return false;
}

bool
EngineHost::import_test_tone (int track, float freq_hz, float seconds)
{
	(void)track;
	(void)freq_hz;
	(void)seconds;
	return false;
}

bool
EngineHost::save_session ()
{
	return false;
}

bool
EngineHost::open_session (const std::string& session_dir, const std::string& snapshot_name)
{
	(void)session_dir;
	(void)snapshot_name;
	return false;
}

bool
EngineHost::reload_session ()
{
	return false;
}

bool
EngineHost::export_session_wav (const std::string& outfile, int64_t start_sample, int64_t end_sample)
{
	(void)outfile;
	(void)start_sample;
	(void)end_sample;
	return false;
}

bool
EngineHost::smoke_export_wav (std::string* out_path)
{
	(void)out_path;
	return false;
}

bool
EngineHost::undo (uint32_t n)
{
	(void)n;
	return false;
}

bool
EngineHost::undo_to_index (int index)
{
	(void)index;
	return false;
}

bool
EngineHost::redo (uint32_t n)
{
	(void)n;
	return false;
}

bool
EngineHost::smoke_history ()
{
	return false;
}

bool
EngineHost::select_region_at (int track, float time_sec)
{
	(void)track;
	(void)time_sec;
	return false;
}

void
EngineHost::clear_region_selection ()
{
}

bool
EngineHost::delete_selected_region ()
{
	return false;
}

bool
EngineHost::split_at (float time_sec, int track)
{
	(void)time_sec;
	(void)track;
	return false;
}

bool
EngineHost::split_at_playhead (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::set_selected_fade_in_sec (float sec)
{
	(void)sec;
	return false;
}

bool
EngineHost::set_selected_fade_out_sec (float sec)
{
	(void)sec;
	return false;
}

bool
EngineHost::apply_selected_crossfade (float length_sec)
{
	(void)length_sec;
	return false;
}

bool
EngineHost::trim_selected_front_to (float time_sec)
{
	(void)time_sec;
	return false;
}

bool
EngineHost::trim_selected_end_to (float time_sec)
{
	(void)time_sec;
	return false;
}

bool
EngineHost::move_selected_to (float time_sec)
{
	(void)time_sec;
	return false;
}

bool
EngineHost::stretch_selected (float time_ratio)
{
	(void)time_ratio;
	return false;
}

bool
EngineHost::pitch_shift_selected (float semitones)
{
	(void)semitones;
	return false;
}

bool
EngineHost::reverse_selected ()
{
	return false;
}

bool
EngineHost::normalize_selected (float target_dbfs)
{
	(void)target_dbfs;
	return false;
}

bool
EngineHost::strip_silence_selected (float threshold_dbfs,
                                    float min_silence_sec,
                                    float fade_sec)
{
	(void)threshold_dbfs;
	(void)min_silence_sec;
	(void)fade_sec;
	return false;
}

bool
EngineHost::consolidate_range (int track, float start_sec, float end_sec)
{
	(void)track;
	(void)start_sec;
	(void)end_sec;
	return false;
}

bool
EngineHost::consolidate_selected ()
{
	return false;
}

bool
EngineHost::freeze_track (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::unfreeze_track (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::toggle_freeze_track (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::track_is_frozen (int track) const
{
	(void)track;
	return false;
}

void
EngineHost::set_snap_enabled (bool on)
{
	_snap.snap_enabled = on;
}

bool
EngineHost::snap_enabled () const
{
	return _snap.snap_enabled;
}

void
EngineHost::toggle_snap ()
{
	_snap.snap_enabled = !_snap.snap_enabled;
}

void
EngineHost::cycle_snap_density ()
{
	_snap.snap_density = (_snap.snap_density + 1) % 4;
}

void
EngineHost::set_snap_density (int density_0_to_3)
{
	if (density_0_to_3 < 0) {
		density_0_to_3 = 0;
	}
	if (density_0_to_3 > 3) {
		density_0_to_3 = 3;
	}
	_snap.snap_density = density_0_to_3;
}

int
EngineHost::snap_density () const
{
	return _snap.snap_density;
}

float
EngineHost::snap_time (float time_sec) const
{
	return time_sec;
}

void
EngineHost::refresh_grid_lines (float view_start_sec, float view_span_sec)
{
	(void)view_start_sec;
	(void)view_span_sec;
	_snap.grid_line_count = 0;
}

int
EngineHost::track_region_count (int track) const
{
	(void)track;
	return 0;
}

bool
EngineHost::smoke_edit ()
{
	return false;
}

bool
EngineHost::smoke_stretch ()
{
	return false;
}

bool
EngineHost::smoke_pitch ()
{
	return false;
}

bool
EngineHost::smoke_reverse ()
{
	return false;
}

bool
EngineHost::smoke_normalize ()
{
	return false;
}

bool
EngineHost::smoke_strip_silence ()
{
	return false;
}

bool
EngineHost::smoke_consolidate ()
{
	return false;
}

bool
EngineHost::smoke_freeze ()
{
	return false;
}

bool
EngineHost::smoke_snap ()
{
	return false;
}

bool
EngineHost::smoke_undo ()
{
	return false;
}

bool
EngineHost::set_loop_range (float start_sec, float end_sec)
{
	(void)start_sec;
	(void)end_sec;
	return false;
}

bool
EngineHost::set_loop_start (float start_sec)
{
	(void)start_sec;
	return false;
}

bool
EngineHost::set_loop_end (float end_sec)
{
	(void)end_sec;
	return false;
}

bool
EngineHost::set_loop_play (bool enable)
{
	(void)enable;
	return false;
}

bool
EngineHost::toggle_loop_play ()
{
	return false;
}

bool
EngineHost::set_punch_range (float start_sec, float end_sec)
{
	(void)start_sec;
	(void)end_sec;
	return false;
}

bool
EngineHost::set_punch_start (float start_sec)
{
	(void)start_sec;
	return false;
}

bool
EngineHost::set_punch_end (float end_sec)
{
	(void)end_sec;
	return false;
}

bool
EngineHost::copy_loop_to_punch ()
{
	return false;
}

bool
EngineHost::set_punch_enabled (bool enable)
{
	(void)enable;
	return false;
}

bool
EngineHost::toggle_punch_enabled ()
{
	return false;
}

bool
EngineHost::smoke_punch ()
{
	return false;
}

bool
EngineHost::smoke_punch_toggle ()
{
	return false;
}

bool
EngineHost::set_tempo_bpm (float bpm)
{
	(void)bpm;
	return false;
}

bool
EngineHost::nudge_tempo_bpm (float delta)
{
	(void)delta;
	return false;
}

bool
EngineHost::set_count_in (bool enable)
{
	(void)enable;
	return false;
}

bool
EngineHost::toggle_count_in ()
{
	return false;
}

bool
EngineHost::smoke_tempo ()
{
	return false;
}

bool
EngineHost::set_clicking (bool enable)
{
	(void)enable;
	return false;
}

bool
EngineHost::toggle_clicking ()
{
	return false;
}

bool
EngineHost::smoke_click ()
{
	return false;
}

bool
EngineHost::add_audio_track ()
{
	return false;
}

bool
EngineHost::add_midi_track ()
{
	return false;
}

bool
EngineHost::remove_audio_track (int index, bool require_empty_or_selected)
{
	(void)index;
	(void)require_empty_or_selected;
	return false;
}

bool
EngineHost::remove_selected_or_empty_audio_track ()
{
	return false;
}

bool
EngineHost::remove_midi_track ()
{
	return false;
}

bool
EngineHost::smoke_tracks ()
{
	return false;
}

bool
EngineHost::set_track_name (int track, const std::string& name)
{
	(void)track;
	(void)name;
	return false;
}

bool
EngineHost::set_midi_track_name (const std::string& name)
{
	(void)name;
	return false;
}

bool
EngineHost::move_audio_track (int index, int delta)
{
	(void)index;
	(void)delta;
	return false;
}

bool
EngineHost::smoke_rename ()
{
	return false;
}

void
EngineHost::refresh_group_flags ()
{
}

bool
EngineHost::create_route_group (const int* indices, int n)
{
	(void)indices;
	(void)n;
	return false;
}

bool
EngineHost::clear_route_group ()
{
	return false;
}

bool
EngineHost::set_route_group_active (bool enable)
{
	(void)enable;
	return false;
}

bool
EngineHost::toggle_route_group_active ()
{
	return false;
}

bool
EngineHost::smoke_group ()
{
	return false;
}

void
EngineHost::refresh_monitor_flags ()
{
}

bool
EngineHost::ensure_monitor_section ()
{
	return false;
}

bool
EngineHost::set_monitor_dim (bool enable)
{
	(void)enable;
	return false;
}

bool
EngineHost::toggle_monitor_dim ()
{
	return false;
}

bool
EngineHost::set_monitor_mono (bool enable)
{
	(void)enable;
	return false;
}

bool
EngineHost::toggle_monitor_mono ()
{
	return false;
}

bool
EngineHost::set_monitor_cut (bool enable)
{
	(void)enable;
	return false;
}

bool
EngineHost::toggle_monitor_cut ()
{
	return false;
}

bool
EngineHost::smoke_monitor ()
{
	return false;
}

bool
EngineHost::add_marker_at (float time_sec, const std::string& name)
{
	(void)time_sec;
	(void)name;
	return false;
}

bool
EngineHost::remove_marker_near (float time_sec, float slop_sec)
{
	(void)time_sec;
	(void)slop_sec;
	return false;
}

bool
EngineHost::locate_to (float time_sec, bool snap_to_marker, float slop_sec)
{
	(void)time_sec;
	(void)snap_to_marker;
	(void)slop_sec;
	return false;
}

bool
EngineHost::scrub_to (float time_sec)
{
	(void)time_sec;
	return false;
}

void
EngineHost::refresh_clock_snap ()
{
}

bool
EngineHost::set_transport_speed (double speed)
{
	(void)speed;
	return false;
}

bool
EngineHost::stop_shuttle ()
{
	return false;
}

bool
EngineHost::smoke_locate ()
{
	return false;
}

bool
EngineHost::session_extent_sec (float* start_sec, float* end_sec) const
{
	if (start_sec) {
		*start_sec = 0.f;
	}
	if (end_sec) {
		*end_sec = 0.f;
	}
	return false;
}

bool
EngineHost::regions_extent_sec (float* start_sec, float* end_sec) const
{
	if (start_sec) {
		*start_sec = 0.f;
	}
	if (end_sec) {
		*end_sec = 0.f;
	}
	return false;
}

bool
EngineHost::smoke_zoom ()
{
	return false;
}

bool
EngineHost::smoke_loop_markers ()
{
	return false;
}

bool
EngineHost::ensure_midi_track ()
{
	return false;
}

bool
EngineHost::attach_midi_synth ()
{
	return false;
}

bool
EngineHost::insert_track_fx (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::toggle_track_fx_bypass (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::set_track_fx_bypassed (int track, bool bypassed)
{
	(void)track;
	(void)bypassed;
	return false;
}

bool
EngineHost::set_track_fx_param (int track, float value_0_1)
{
	(void)track;
	(void)value_0_1;
	return false;
}

float
EngineHost::track_fx_param (int track) const
{
	(void)track;
	return 0.f;
}

bool
EngineHost::insert_track_comp (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::toggle_track_comp_bypass (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::set_track_comp_bypassed (int track, bool bypassed)
{
	(void)track;
	(void)bypassed;
	return false;
}

bool
EngineHost::set_track_comp_threshold_db (int track, float db)
{
	(void)track;
	(void)db;
	return false;
}

bool
EngineHost::set_track_comp_threshold_norm (int track, float norm_0_1)
{
	(void)track;
	(void)norm_0_1;
	return false;
}

bool
EngineHost::set_track_comp_ratio (int track, float ratio)
{
	(void)track;
	(void)ratio;
	return false;
}

bool
EngineHost::set_track_comp_ratio_norm (int track, float norm_0_1)
{
	(void)track;
	(void)norm_0_1;
	return false;
}

bool
EngineHost::set_track_comp_sidechain (int track, int src_track, bool enable)
{
	(void)track;
	(void)src_track;
	(void)enable;
	return false;
}

bool
EngineHost::smoke_plugin_insert ()
{
	return false;
}

bool
EngineHost::smoke_comp ()
{
	return false;
}

bool
EngineHost::insert_track_eq (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::toggle_track_eq_bypass (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::set_track_eq_bypassed (int track, bool bypassed)
{
	(void)track;
	(void)bypassed;
	return false;
}

bool
EngineHost::set_track_eq_gain_l_db (int track, float db)
{
	(void)track;
	(void)db;
	return false;
}

bool
EngineHost::set_track_eq_gain_m_db (int track, float db)
{
	(void)track;
	(void)db;
	return false;
}

bool
EngineHost::set_track_eq_gain_h_db (int track, float db)
{
	(void)track;
	(void)db;
	return false;
}

bool
EngineHost::set_track_eq_gain_l_norm (int track, float norm_0_1)
{
	(void)track;
	(void)norm_0_1;
	return false;
}

bool
EngineHost::set_track_eq_gain_m_norm (int track, float norm_0_1)
{
	(void)track;
	(void)norm_0_1;
	return false;
}

bool
EngineHost::set_track_eq_gain_h_norm (int track, float norm_0_1)
{
	(void)track;
	(void)norm_0_1;
	return false;
}

bool
EngineHost::smoke_eq ()
{
	return false;
}

bool
EngineHost::insert_track_delay (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::toggle_track_delay_bypass (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::set_track_delay_bypassed (int track, bool bypassed)
{
	(void)track;
	(void)bypassed;
	return false;
}

bool
EngineHost::set_track_delay_time_ms (int track, float ms)
{
	(void)track;
	(void)ms;
	return false;
}

bool
EngineHost::set_track_delay_feedback_pct (int track, float pct)
{
	(void)track;
	(void)pct;
	return false;
}

bool
EngineHost::set_track_delay_time_norm (int track, float norm_0_1)
{
	(void)track;
	(void)norm_0_1;
	return false;
}

bool
EngineHost::set_track_delay_feedback_norm (int track, float norm_0_1)
{
	(void)track;
	(void)norm_0_1;
	return false;
}

bool
EngineHost::smoke_delay ()
{
	return false;
}

bool
EngineHost::insert_track_limiter (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::toggle_track_limiter_bypass (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::set_track_limiter_bypassed (int track, bool bypassed)
{
	(void)track;
	(void)bypassed;
	return false;
}

bool
EngineHost::set_track_limiter_ceiling_db (int track, float db)
{
	(void)track;
	(void)db;
	return false;
}

bool
EngineHost::set_track_limiter_release_ms (int track, float ms)
{
	(void)track;
	(void)ms;
	return false;
}

bool
EngineHost::set_track_limiter_ceiling_norm (int track, float norm_0_1)
{
	(void)track;
	(void)norm_0_1;
	return false;
}

bool
EngineHost::set_track_limiter_release_norm (int track, float norm_0_1)
{
	(void)track;
	(void)norm_0_1;
	return false;
}

bool
EngineHost::smoke_limiter ()
{
	return false;
}

bool
EngineHost::insert_track_gate (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::toggle_track_gate_bypass (int track)
{
	(void)track;
	return false;
}

bool
EngineHost::set_track_gate_bypassed (int track, bool bypassed)
{
	(void)track;
	(void)bypassed;
	return false;
}

bool
EngineHost::set_track_gate_threshold_db (int track, float db)
{
	(void)track;
	(void)db;
	return false;
}

bool
EngineHost::set_track_gate_hold_ms (int track, float ms)
{
	(void)track;
	(void)ms;
	return false;
}

bool
EngineHost::set_track_gate_threshold_norm (int track, float norm_0_1)
{
	(void)track;
	(void)norm_0_1;
	return false;
}

bool
EngineHost::set_track_gate_hold_norm (int track, float norm_0_1)
{
	(void)track;
	(void)norm_0_1;
	return false;
}

bool
EngineHost::smoke_gate ()
{
	return false;
}

bool
EngineHost::smoke_spectrum ()
{
	return false;
}

bool
EngineHost::set_spectrum_enabled (bool on)
{
	(void)on;
	return false;
}

bool
EngineHost::toggle_spectrum_enabled ()
{
	return false;
}

bool
EngineHost::set_spectrum_master (bool master)
{
	(void)master;
	return false;
}

bool
EngineHost::toggle_spectrum_master ()
{
	return false;
}

bool
EngineHost::smoke_correlation ()
{
	return false;
}

bool
EngineHost::set_correlation_enabled (bool on)
{
	(void)on;
	return false;
}

bool
EngineHost::toggle_correlation_enabled ()
{
	return false;
}

bool
EngineHost::set_correlation_master (bool master)
{
	(void)master;
	return false;
}

bool
EngineHost::toggle_correlation_master ()
{
	return false;
}


bool
EngineHost::smoke_loudness ()
{
	return false;
}

bool
EngineHost::set_loudness_enabled (bool on)
{
	(void)on;
	return false;
}

bool
EngineHost::toggle_loudness_enabled ()
{
	return false;
}

bool
EngineHost::set_loudness_master (bool master)
{
	(void)master;
	return false;
}

bool
EngineHost::toggle_loudness_master ()
{
	return false;
}

void
EngineHost::refresh_loudness_snap ()
{
}


bool
EngineHost::smoke_vst3 ()
{
	return false;
}

bool
EngineHost::smoke_perf (unsigned sample_secs)
{
	(void)sample_secs;
	return false;
}

void
EngineHost::refresh_track_fx_snap ()
{
}

void
EngineHost::refresh_track_comp_snap ()
{
}

void
EngineHost::refresh_track_eq_snap ()
{
}

void
EngineHost::refresh_track_delay_snap ()
{
}

void
EngineHost::refresh_track_limiter_snap ()
{
}

void
EngineHost::refresh_track_gate_snap ()
{
}

void
EngineHost::refresh_spectrum_snap ()
{
}

void
EngineHost::refresh_correlation_snap ()
{
}

bool
EngineHost::ensure_plugins_scanned ()
{
	return false;
}

bool
EngineHost::ensure_midi_region (float length_sec)
{
	(void)length_sec;
	return false;
}

bool
EngineHost::add_midi_note (uint8_t pitch, float start_sec, float length_sec, uint8_t velocity)
{
	(void)pitch;
	(void)start_sec;
	(void)length_sec;
	(void)velocity;
	return false;
}

bool
EngineHost::select_midi_note_at (float time_sec, uint8_t pitch, float pitch_slop)
{
	(void)time_sec;
	(void)pitch;
	(void)pitch_slop;
	return false;
}

void
EngineHost::clear_midi_note_selection ()
{
}

bool
EngineHost::delete_selected_midi_note ()
{
	return false;
}

bool
EngineHost::connect_midi_output ()
{
	return false;
}

void
EngineHost::refresh_midi_notes_snap ()
{
}

void
EngineHost::refresh_midi_snap ()
{
}

bool
EngineHost::smoke_midi ()
{
	return false;
}

bool
EngineHost::smoke_midi_edit ()
{
	return false;
}


bool
EngineHost::quantize_midi_notes ()
{
	return false;
}

bool
EngineHost::smoke_midi_quantize ()
{
	return false;
}


bool
EngineHost::transpose_midi_notes (int semitones)
{
	(void)semitones;
	return false;
}

bool
EngineHost::smoke_midi_transpose ()
{
	return false;
}


bool
EngineHost::nudge_midi_velocity (int delta)
{
	(void)delta;
	return false;
}

bool
EngineHost::set_midi_velocity (int velocity)
{
	(void)velocity;
	return false;
}

bool
EngineHost::smoke_midi_velocity ()
{
	return false;
}

bool
EngineHost::nudge_midi_length (float delta_sec)
{
	(void)delta_sec;
	return false;
}

bool
EngineHost::set_midi_note_start (float start_sec)
{
	(void)start_sec;
	return false;
}

bool
EngineHost::set_midi_note_end (float end_sec)
{
	(void)end_sec;
	return false;
}

float
EngineHost::snap_unit_sec () const
{
	return 0.5f;
}

bool
EngineHost::smoke_midi_length ()
{
	return false;
}

bool
EngineHost::has_midi_clipboard () const
{
	return false;
}

bool
EngineHost::copy_selected_midi_notes ()
{
	return false;
}

bool
EngineHost::cut_selected_midi_notes ()
{
	return false;
}

bool
EngineHost::paste_midi_notes (float at_sec)
{
	(void)at_sec;
	return false;
}

bool
EngineHost::smoke_midi_clipboard ()
{
	return false;
}

bool
EngineHost::humanize_midi_notes (float strength, uint32_t seed)
{
	(void)strength;
	(void)seed;
	return false;
}

bool
EngineHost::smoke_midi_humanize ()
{
	return false;
}

void
EngineHost::set_midi_step_enabled (bool on)
{
	_snap.midi_step_enabled = on;
}

bool
EngineHost::midi_step_enabled () const
{
	return false;
}

bool
EngineHost::toggle_midi_step_enabled ()
{
	return false;
}

void
EngineHost::set_midi_chord_quality (int quality)
{
	if (quality < 0) {
		quality = 0;
	}
	if (quality > 2) {
		quality = 2;
	}
	_snap.midi_chord_quality = quality;
}

int
EngineHost::midi_chord_quality () const
{
	return _snap.midi_chord_quality;
}

int
EngineHost::cycle_midi_chord_quality ()
{
	int q = _snap.midi_chord_quality + 1;
	if (q > 2) {
		q = 0;
	}
	set_midi_chord_quality (q);
	return _snap.midi_chord_quality;
}

bool
EngineHost::step_advance (int steps)
{
	(void)steps;
	return false;
}

bool
EngineHost::step_input_midi_note (uint8_t pitch, uint8_t velocity, bool advance)
{
	(void)pitch;
	(void)velocity;
	(void)advance;
	return false;
}

bool
EngineHost::smoke_midi_step ()
{
	return false;
}

bool
EngineHost::smoke_midi_chord ()
{
	return false;
}

void
EngineHost::set_midi_arp_enabled (bool on)
{
	_arp_enabled = on;
	_snap.midi_arp_enabled = on;
	if (!on) {
		_arp_sounding = -1;
		_snap.midi_arp_pitch = -1;
	}
}

bool
EngineHost::midi_arp_enabled () const
{
	return false;
}

bool
EngineHost::toggle_midi_arp_enabled ()
{
	return false;
}

void
EngineHost::set_midi_arp_mode (int mode)
{
	if (mode < 0) {
		mode = 0;
	}
	if (mode > 2) {
		mode = 2;
	}
	_arp_mode = mode;
	_snap.midi_arp_mode = mode;
}

int
EngineHost::midi_arp_mode () const
{
	return _arp_mode;
}

int
EngineHost::cycle_midi_arp_mode ()
{
	int m = _arp_mode + 1;
	if (m > 2) {
		m = 0;
	}
	set_midi_arp_mode (m);
	return _arp_mode;
}

void
EngineHost::set_midi_arp_held_pitches (const uint8_t* pitches, int count)
{
	(void)pitches;
	(void)count;
}

void
EngineHost::clear_midi_arp_held_pitches ()
{
	_arp_held_count = 0;
}

void
EngineHost::rebuild_arp_pitches ()
{
}

int
EngineHost::arp_pitch_at_step (int step) const
{
	(void)step;
	return -1;
}

bool
EngineHost::midi_immediate_note (uint8_t status, uint8_t pitch, uint8_t velocity)
{
	(void)status;
	(void)pitch;
	(void)velocity;
	return false;
}

void
EngineHost::arp_silence ()
{
	_arp_sounding = -1;
	_snap.midi_arp_pitch = -1;
}

bool
EngineHost::tick_arpeggiator (bool force)
{
	(void)force;
	return false;
}

bool
EngineHost::smoke_midi_arp ()
{
	return false;
}

void
EngineHost::set_midi_scale_lock (bool on)
{
	_snap.midi_scale_lock = on;
}

bool
EngineHost::midi_scale_lock () const
{
	return false;
}

bool
EngineHost::toggle_midi_scale_lock ()
{
	return false;
}

void
EngineHost::set_midi_scale_kind (int kind)
{
	if (kind < 0) {
		kind = 0;
	}
	if (kind > 1) {
		kind = 1;
	}
	_snap.midi_scale_kind = kind;
}

int
EngineHost::midi_scale_kind () const
{
	return _snap.midi_scale_kind;
}

int
EngineHost::cycle_midi_scale_kind ()
{
	set_midi_scale_kind (_snap.midi_scale_kind == 0 ? 1 : 0);
	return _snap.midi_scale_kind;
}

void
EngineHost::set_midi_scale_root (int root)
{
	if (root < 0) {
		root = 0;
	}
	if (root > 11) {
		root = 11;
	}
	_snap.midi_scale_root = root;
}

int
EngineHost::midi_scale_root () const
{
	return _snap.midi_scale_root;
}

int
EngineHost::cycle_midi_scale_root ()
{
	int r = _snap.midi_scale_root + 1;
	if (r > 11) {
		r = 0;
	}
	set_midi_scale_root (r);
	return _snap.midi_scale_root;
}

uint8_t
EngineHost::snap_pitch_to_scale (uint8_t pitch) const
{
	return pitch > 127 ? 127 : pitch;
}

bool
EngineHost::smoke_midi_scale ()
{
	return false;
}


bool
EngineHost::configure_backend_midi ()
{
	return false;
}

void
EngineHost::refresh_history_flags ()
{
}

void
EngineHost::refresh_selection_snap ()
{
}

void
EngineHost::refresh_locations_snap ()
{
}

std::string
EngineHost::last_export_path () const
{
	return std::string ();
}

std::string
EngineHost::session_dir () const
{
	return std::string ();
}

void
EngineHost::collect_tracks ()
{
}

void
EngineHost::update_session_path_snap ()
{
}

bool
EngineHost::compute_export_range (int64_t* start_sample, int64_t* end_sample) const
{
	(void)start_sample;
	(void)end_sample;
	return false;
}

bool
EngineHost::fill_track_waveform (int track,
                                 float view_start_sec,
                                 float view_span_sec,
                                 float* peaks,
                                 float* rms,
                                 size_t n_columns)
{
	(void)track;
	(void)view_start_sec;
	(void)view_span_sec;
	if (peaks && n_columns > 0) {
		std::memset (peaks, 0, sizeof (float) * n_columns);
	}
	if (rms && n_columns > 0) {
		std::memset (rms, 0, sizeof (float) * n_columns);
	}
	return false;
}

#endif

