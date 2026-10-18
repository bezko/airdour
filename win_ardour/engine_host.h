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
	std::wstring detail; /* error or idle note */
};

class EngineHost
{
public:
	EngineHost ();
	~EngineHost ();

	EngineHost (const EngineHost&) = delete;
	EngineHost& operator= (const EngineHost&) = delete;

	/** Start backend on the calling (UI) thread. Safe to call once. */
	bool start (const std::string& backend_name);

	/** Stop backend I/O on the UI thread. Avoids AudioEngine::stop lock/join hang. */
	void stop ();

	/** UI-thread poll of stable engine fields (SR/period/name). */
	void poll ();

	EngineSnapshot snapshot () const;

	static bool is_linked ();

private:
	void refresh_snapshot ();

	bool           _started;
	EngineSnapshot _snap;

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE
	class Impl;
	Impl* _impl;
#endif
};
