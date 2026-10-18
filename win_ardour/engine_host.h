/*
 * Copyright (C) 2026 Airdour contributors
 *
 * UI-thread engine + minimal Session attach for win_ardour.
 * Peak meters and transport time come from Session when available.
 */

#pragma once

#include <cstdint>
#include <string>

struct EngineSnapshot {
	static constexpr int kMeters = 3;

	bool         available; /* compiled with AIRDOUR_WIN_SHELL_WITH_ENGINE */
	bool         running;
	bool         session_ok;
	bool         transport_rolling;
	bool         meters_real; /* PeakMeter (not stub) */
	std::wstring backend;
	uint32_t     sample_rate;
	uint32_t     period_frames;
	float        gain; /* track 0 stub/session gain */
	float        meter_peak[kMeters]; /* linear 0..1 */
	float        transport_sec;
	std::wstring detail;
};

class EngineHost
{
public:
	EngineHost ();
	~EngineHost ();

	EngineHost (const EngineHost&) = delete;
	EngineHost& operator= (const EngineHost&) = delete;

	void prepare (const std::string& backend_name);

	/** Start backend + scaffold minimal Session (tracks + demo tones). */
	bool start ();
	bool start (const std::string& backend_name);

	/** Tear down Session + stop backend I/O (UI thread). */
	void stop ();

	/** Stop backend I/O only (no Session/Graph join). For timeout smoke. */
	void stop_backend_only ();

	/** Session transport roll / stop (keeps engine running). */
	void transport_play ();
	void transport_stop ();

	void poll ();

	void  set_gain (float linear);
	void  set_track_gain (int track, float linear);
	float gain () const;

	EngineSnapshot snapshot () const;

	static bool is_linked ();

private:
	void refresh_snapshot ();
	bool ensure_session ();
	void destroy_session ();
	void read_meters ();

	bool        _started;
	std::string _backend_name;
	float       _gain;
	float       _track_gain[EngineSnapshot::kMeters];
	EngineSnapshot _snap;

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE
	class Impl;
	Impl* _impl;
#endif
};
