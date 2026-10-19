/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "engine_host.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE

#include "ardour/ardour.h"
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
#include "ardour/session_directory.h"
#include "ardour/presentation_info.h"
#include "ardour/rc_configuration.h"
#include "ardour/region.h"
#include "ardour/region_factory.h"
#include "ardour/route.h"
#include "ardour/audio_backend.h"
#include "ardour/chan_count.h"
#include "ardour/location.h"
#include "ardour/midi_model.h"
#include "ardour/midi_region.h"
#include "ardour/midi_source.h"
#include "ardour/midi_track.h"
#include "ardour/automation_control.h"
#include "ardour/mute_control.h"
#include "ardour/solo_control.h"
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
#include "ardour/types.h"
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

#include <glib.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>
#include "pbd/xml++.h"
#include "temporal/beats.h"
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
		, plugins_scanned (false)
	{
	}
};

EngineHost::EngineHost ()
	: _started (false)
	, _backend_name ("Dummy")
	, _period_frames (0)
	, _gain (0.85f)
	, _impl (new Impl)
{
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_track_gain[i] = (i == 0) ? 0.85f : (0.85f - 0.12f * (float)i);
		_snap.meter_peak[i] = 0.f;
		_snap.meter_clip[i] = false;
		_snap.track_has_regions[i] = false;
		_snap.track_armed[i] = false;
		_snap.track_muted[i] = false;
		_snap.track_soloed[i] = false;
		_snap.track_pan[i] = 0.5f;
	}
	_snap.midi_muted = false;
	_snap.midi_soloed = false;
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
	_snap.has_selection = false;
	_snap.selection_track = -1;
	_snap.selection_start_sec = 0.f;
	_snap.selection_end_sec = 0.f;
	_snap.loop_active = false;
	_snap.loop_playing = false;
	_snap.loop_start_sec = 0.f;
	_snap.loop_end_sec = 0.f;
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
	_snap.backend = utf8_to_wide (resolve_backend_name (_backend_name));
	_snap.detail = L"prepared — press Play to start";
}

void
EngineHost::set_period_frames (uint32_t frames)
{
	_period_frames = frames;
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

		/* Enable click so something always hits the graph when rolling. */
		Config->set_clicking (true);

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

		step ("new_audio_track x3");
		AudioTrackList created = _impl->session->new_audio_track (
			1, 2,
			std::shared_ptr<RouteGroup> (),
			(uint32_t)EngineSnapshot::kMeters,
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
			_snap.detail = L"Session + PeakMeter — demo tones on Audio 1–3";
		} else {
			for (size_t t = 0; t < _impl->tracks.size (); ++t) {
				if (_impl->tracks[t] && _impl->tracks[t]->gain_control ()) {
					_impl->tracks[t]->gain_control ()->set_value (_track_gain[t], Controllable::NoGroup);
				}
			}
			_snap.detail = L"Session + PeakMeter — empty Audio 1–3 (no demo tones)";
		}

		/* Best-effort ACE Reverb on Audio 1 (headless insert, no GTK UI). */
		insert_track_fx (0);

		_snap.session_ok = true;
		_snap.meters_real = true;
		_snap.waves_from_session = true;
		refresh_region_flags ();
		refresh_track_fx_snap ();
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
	_snap.has_selection = false;
	_snap.selection_track = -1;
	_snap.selection_start_sec = 0.f;
	_snap.selection_end_sec = 0.f;
	_snap.loop_active = false;
	_snap.loop_playing = false;
	_snap.loop_start_sec = 0.f;
	_snap.loop_end_sec = 0.f;
	_snap.marker_count = 0;
	_snap.session_path.clear ();
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_snap.track_has_regions[i] = false;
		_snap.track_armed[i] = false;
	}
}

void
EngineHost::collect_tracks ()
{
	_impl->tracks.clear ();
	if (!_impl || !_impl->session) {
		return;
	}
	try {
		std::shared_ptr<RouteList> routes = _impl->session->get_tracks ();
		if (!routes) {
			return;
		}
		for (RouteList::const_iterator it = routes->begin (); it != routes->end (); ++it) {
			std::shared_ptr<AudioTrack> at = std::dynamic_pointer_cast<AudioTrack> (*it);
			if (!at) {
				continue;
			}
			_impl->tracks.push_back (at);
			if (_impl->tracks.size () >= (size_t)EngineSnapshot::kMeters) {
				break;
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
				(uint32_t)EngineSnapshot::kMeters,
				"Audio",
				PresentationInfo::max_order);
			for (AudioTrackList::iterator it = created.begin (); it != created.end (); ++it) {
				_impl->tracks.push_back (*it);
			}
		}

		for (size_t t = 0; t < _impl->tracks.size () && t < (size_t)EngineSnapshot::kMeters; ++t) {
			if (_impl->tracks[t] && _impl->tracks[t]->gain_control ()) {
				_impl->tracks[t]->gain_control ()->set_value (_track_gain[t], Controllable::NoGroup);
			}
		}

		_snap.session_ok = true;
		_snap.meters_real = true;
		_snap.waves_from_session = true;
		_snap.session_saved = true;
		update_session_path_snap ();
		refresh_region_flags ();
		refresh_record_flags ();
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
		_impl->tracks[track]->gain_control ()->set_value (linear, Controllable::NoGroup);
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
		_impl->session->set_control (mc, muted ? 1.0 : 0.0, Controllable::NoGroup);
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
		_impl->session->set_control (sc, soloed ? 1.0 : 0.0, Controllable::NoGroup);
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
	if (!_impl || !_impl->session) {
		return;
	}
	try {
		_snap.undo_depth = _impl->session->undo_depth ();
		_snap.redo_depth = _impl->session->redo_depth ();
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

void
EngineHost::refresh_locations_snap ()
{
	_snap.loop_active = false;
	_snap.loop_playing = false;
	_snap.loop_start_sec = 0.f;
	_snap.loop_end_sec = 0.f;
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
		_snap.detail = snap_to_marker ? L"located (snap mark)" : L"located";
		return true;
	} catch (...) {
		_snap.detail = L"locate failed";
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
			refresh_history_flags ();
			refresh_selection_snap ();
			refresh_locations_snap ();
			refresh_midi_snap ();
			refresh_track_fx_snap ();
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
}

EngineSnapshot
EngineHost::snapshot () const
{
	return _snap;
}

#else /* !AIRDOUR_WIN_SHELL_WITH_ENGINE */

EngineHost::EngineHost ()
	: _started (false)
	, _backend_name ("Dummy")
	, _period_frames (0)
	, _gain (0.85f)
{
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_track_gain[i] = 0.85f;
		_snap.meter_peak[i] = 0.f;
		_snap.meter_clip[i] = false;
		_snap.track_has_regions[i] = false;
		_snap.track_armed[i] = false;
	}
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
	_snap.has_selection = false;
	_snap.selection_track = -1;
	_snap.selection_start_sec = 0.f;
	_snap.selection_end_sec = 0.f;
	_snap.loop_active = false;
	_snap.loop_playing = false;
	_snap.loop_start_sec = 0.f;
	_snap.loop_end_sec = 0.f;
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
EngineHost::refresh_record_flags ()
{
}

void
EngineHost::refresh_mute_solo_flags ()
{
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
EngineHost::redo (uint32_t n)
{
	(void)n;
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
EngineHost::smoke_plugin_insert ()
{
	return false;
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
