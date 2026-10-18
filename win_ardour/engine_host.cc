/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "engine_host.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE

/* Include Ardour/glib before windows.h; COM's `interface` macro breaks glib. */
#include "ardour/ardour.h"
#include "ardour/audio_backend.h"
#include "ardour/audioengine.h"
#include "ardour/session_event.h"
#include "pbd/error.h"

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

} // namespace

class EngineHost::Impl
{
public:
	AudioEngine* engine;
	bool         ardour_inited;

	Impl ()
		: engine (nullptr)
		, ardour_inited (false)
	{
	}
};

EngineHost::EngineHost ()
	: _started (false)
	, _backend_name ("Dummy")
	, _gain (0.85f)
	, _impl (new Impl)
{
	_snap.available = true;
	_snap.running = false;
	_snap.sample_rate = 0;
	_snap.period_frames = 0;
	_snap.gain = _gain;
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
EngineHost::start ()
{
	if (_started) {
		return true;
	}

	auto step = [] (const char* msg) {
		std::fprintf (stderr, "airdour_win_shell: engine: %s\n", msg);
		std::fflush (stderr);
	};

	/* Restart path: engine already created, backend stopped. */
	if (_impl->engine) {
		step ("engine->start (restart)");
		if (_impl->engine->start () != 0) {
			_snap.detail = L"engine restart failed";
			_snap.backend = utf8_to_wide (_impl->engine->current_backend_name ());
			return false;
		}
		_started = true;
		refresh_snapshot ();
		_snap.detail = L"UI thread attach - audio on backend RT thread";
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

	/* After ARDOUR::init (same order as hardour). */
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
	refresh_snapshot ();
	_snap.detail = L"UI thread attach - audio on backend RT thread";
	step ("running");
	return true;
}

void
EngineHost::stop ()
{
	if (!_impl || !_impl->engine) {
		_started = false;
		_snap.running = false;
		return;
	}

	/* Do not call AudioEngine::stop() here: it takes _process_lock then
	 * joins the process thread and can hang on Windows engine-only runs.
	 * Stop the backend directly from the UI thread instead. */
	std::shared_ptr<AudioBackend> be = _impl->engine->current_backend ();
	if (be) {
		be->stop ();
	}

	_started = false;
	_snap.running = false;
	_snap.detail = L"engine stopped — press Play to restart";
}

void
EngineHost::set_gain (float linear)
{
	_gain = std::max (0.f, std::min (1.f, linear));
	_snap.gain = _gain;
}

float
EngineHost::gain () const
{
	return _gain;
}

void
EngineHost::refresh_snapshot ()
{
	if (!_impl || !_impl->engine || !_started) {
		_snap.gain = _gain;
		return;
	}
	_snap.available = true;
	_snap.running = _impl->engine->running ();
	_snap.backend = utf8_to_wide (_impl->engine->current_backend_name ());
	_snap.sample_rate = (uint32_t)_impl->engine->sample_rate ();
	_snap.period_frames = (uint32_t)_impl->engine->samples_per_cycle ();
	_snap.gain = _gain;
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
	_snap.available = false;
	_snap.running = false;
	_snap.sample_rate = 0;
	_snap.period_frames = 0;
	_snap.gain = _gain;
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
EngineHost::stop ()
{
	_started = false;
	_snap.running = false;
}

void
EngineHost::set_gain (float linear)
{
	_gain = std::max (0.f, std::min (1.f, linear));
	_snap.gain = _gain;
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

#endif
