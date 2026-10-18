/*
 * Copyright (C) 2026 Airdour contributors
 *
 * UI-thread engine attach for win_ardour.
 * Audio callbacks stay on the backend realtime thread; the shell only
 * starts/stops and reads stable status fields from the UI thread.
 */

#pragma once

#include <cstdint>
#include <string>

struct EngineSnapshot {
	bool         available; /* compiled with AIRDOUR_WIN_SHELL_WITH_ENGINE */
	bool         running;
	std::wstring backend;
	uint32_t     sample_rate;
	uint32_t     period_frames;
	float        gain;   /* stub linear 0..1 until Session routes */
	std::wstring detail; /* error or idle note */
};

class EngineHost
{
public:
	EngineHost ();
	~EngineHost ();

	EngineHost (const EngineHost&) = delete;
	EngineHost& operator= (const EngineHost&) = delete;

	/** Remember backend for later Play; does not start I/O. */
	void prepare (const std::string& backend_name);

	/** Start backend on the calling (UI) thread. Restart-safe after stop(). */
	bool start ();
	bool start (const std::string& backend_name);

	/** Stop backend I/O on the UI thread. Avoids AudioEngine::stop lock/join hang. */
	void stop ();

	/** UI-thread poll of stable engine fields (SR/period/name). */
	void poll ();

	/** Stub monitor/track gain (no Session yet). Visual + snapshot only. */
	void  set_gain (float linear);
	float gain () const;

	EngineSnapshot snapshot () const;

	static bool is_linked ();

private:
	void refresh_snapshot ();

	bool        _started;
	std::string _backend_name;
	float       _gain;
	EngineSnapshot _snap;

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE
	class Impl;
	Impl* _impl;
#endif
};
