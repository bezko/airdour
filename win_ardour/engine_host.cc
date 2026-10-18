/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "engine_host.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE

#include "ardour/ardour.h"
#include "ardour/audio_backend.h"
#include "ardour/audio_track.h"
#include "ardour/audioengine.h"
#include "ardour/audiosource.h"
#include "ardour/meter.h"
#include "ardour/playlist.h"
#include "ardour/presentation_info.h"
#include "ardour/rc_configuration.h"
#include "ardour/region.h"
#include "ardour/region_factory.h"
#include "ardour/route.h"
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
#include "pbd/rwlock.h"
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

	Impl ()
		: engine (nullptr)
		, session (nullptr)
		, event_loop (nullptr)
		, ardour_inited (false)
		, transport_playing (false)
		, play_origin_samples (0)
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
	}
	_snap.available = true;
	_snap.running = false;
	_snap.session_ok = false;
	_snap.transport_rolling = false;
	_snap.meters_real = false;
	_snap.sample_rate = 0;
	_snap.period_frames = 0;
	_snap.gain = _gain;
	_snap.transport_sec = 0.f;
	_snap.backend = L"(idle)";
	_snap.detail = L"engine linked — not started";
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
	if (_impl->engine && _impl->session) {
		try {
			_impl->session->request_stop (false, false, TRS_UI);
		} catch (...) {
		}
		_impl->engine->remove_session ();
	}
	/* Do not delete Session on Windows engine-only teardown: Session/~AudioEngine
	 * join paths can hang (same class of issue as AudioEngine::stop). Leak the
	 * object until process exit — acceptable for shell smoke / UI lifetime. */
	_impl->session = nullptr;
	_snap.session_ok = false;
	_snap.meters_real = false;
	_snap.transport_rolling = false;
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
		_impl->session->request_stop (false, false, TRS_UI);
		_impl->session->request_locate (0, true, MustStop, TRS_UI);
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
		} else {
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
	}
	_snap.available = false;
	_snap.running = false;
	_snap.session_ok = false;
	_snap.transport_rolling = false;
	_snap.meters_real = false;
	_snap.sample_rate = 0;
	_snap.period_frames = 0;
	_snap.gain = _gain;
	_snap.transport_sec = 0.f;
	_snap.backend = L"(not linked)";
	_snap.detail = L"build with AIRDOUR_WIN_SHELL_WITH_ENGINE + waf libs";
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

#endif
