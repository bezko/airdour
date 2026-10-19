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
#include "ardour/playlist.h"
#include "ardour/session_directory.h"
#include "ardour/presentation_info.h"
#include "ardour/rc_configuration.h"
#include "ardour/region.h"
#include "ardour/region_factory.h"
#include "ardour/route.h"
#include "ardour/location.h"
#include "ardour/session.h"
#include "ardour/session_event.h"
#include "ardour/audiofilesource.h"
#include "ardour/sndfilesource.h"
#include "ardour/source_factory.h"
#include "ardour/types.h"
#include "pbd/compose.h"
#include "pbd/controllable.h"
#include "pbd/error.h"
#include "pbd/event_loop.h"
#include "pbd/failed_constructor.h"
#include "pbd/memento_command.h"
#include "pbd/rwlock.h"
#include "pbd/xml++.h"
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

#include <shlobj.h>

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
	int             selected_track;
	std::weak_ptr<Region> selected_region;

	Impl ()
		: engine (nullptr)
		, session (nullptr)
		, event_loop (nullptr)
		, ardour_inited (false)
		, transport_playing (false)
		, play_origin_samples (0)
		, selected_track (-1)
	{
	}
};

EngineHost::EngineHost ()
	: _started (false)
	, _backend_name ("Dummy")
	, _gain (0.85f)
	, _impl (new Impl)
{
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_track_gain[i] = (i == 0) ? 0.85f : (0.85f - 0.12f * (float)i);
		_snap.meter_peak[i] = 0.f;
		_snap.track_has_regions[i] = false;
		_snap.track_armed[i] = false;
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

		_snap.session_ok = true;
		_snap.meters_real = true;
		_snap.waves_from_session = true;
		refresh_region_flags ();
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

	step ("engine->start");
	if (_impl->engine->start () != 0) {
		_snap.detail = L"engine start failed (" + utf8_to_wide (name) + L")";
		_snap.backend = utf8_to_wide (_impl->engine->current_backend_name ());
		return false;
	}

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
			if (i < (int)_impl->tracks.size () && _impl->tracks[i]) {
				std::shared_ptr<PeakMeter> pm = _impl->tracks[i]->peak_meter ();
				if (pm) {
					peak = peak_linear_from_meter (pm);
				}
			}
			_snap.meter_peak[i] = peak;
		}
	} catch (...) {
		_snap.meters_real = false;
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

		std::vector<PeakData> buf (n_columns);
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
			refresh_history_flags ();
			refresh_selection_snap ();
			refresh_locations_snap ();
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
	, _gain (0.85f)
{
	for (int i = 0; i < EngineSnapshot::kMeters; ++i) {
		_track_gain[i] = 0.85f;
		_snap.meter_peak[i] = 0.f;
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
