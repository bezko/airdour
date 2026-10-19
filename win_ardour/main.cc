/*
 * Copyright (C) 2026 Airdour contributors
 *
 * airdour_win_shell — Win32 + Direct2D + DirectWrite host.
 *
 * Usage:
 *   airdour_win_shell.exe [--timeout N] [--backend Dummy|WASAPI] [--no-engine]
 *                         [--smoke-import] [--smoke-record] [--smoke-save-load]
 *                         [--smoke-export] [--smoke-undo] [--smoke-loop]
 *                         [--smoke-midi] [--smoke-midi-edit] [--smoke-plugin]
 *                         [--smoke-perf] [--smoke-vst3] [--smoke-mute-solo]
 *                         [--smoke-pan] [--smoke-send] [--smoke-automation]
 *                         [--smoke-edit] [--smoke-snap] [--smoke-punch]
 *                         [--smoke-tempo] [--smoke-click] [--smoke-tracks]
 *                         [--smoke-rename] [--smoke-group] [--smoke-monitor]
 *                         [--smoke-locate] [--smoke-zoom] [--smoke-history]
 *                         [--smoke-prefs] [--smoke-recent] [--smoke-recovery]
 *                         [--no-splash] [--period N] [--session DIR]
 */

#include "shell_window.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#pragma comment(lib, "ole32.lib")

static unsigned
parse_timeout (int argc, wchar_t** argv)
{
	for (int i = 1; i < argc; ++i) {
		if (wcscmp (argv[i], L"--timeout") == 0 && i + 1 < argc) {
			return (unsigned)wcstoul (argv[i + 1], nullptr, 10);
		}
		if (wcsncmp (argv[i], L"--timeout=", 10) == 0) {
			return (unsigned)wcstoul (argv[i] + 10, nullptr, 10);
		}
	}
	return 0;
}

static std::string
parse_backend (int argc, wchar_t** argv)
{
	for (int i = 1; i < argc; ++i) {
		if (wcscmp (argv[i], L"--backend") == 0 && i + 1 < argc) {
			char buf[128];
			WideCharToMultiByte (CP_UTF8, 0, argv[i + 1], -1, buf, sizeof (buf), nullptr, nullptr);
			return std::string (buf);
		}
	}
	/* Empty → EngineHost resolves WinShellPrefs then Dummy. */
	return std::string ();
}

static std::string
parse_session_path (int argc, wchar_t** argv)
{
	for (int i = 1; i < argc; ++i) {
		if (wcscmp (argv[i], L"--session") == 0 && i + 1 < argc) {
			char buf[MAX_PATH * 4];
			WideCharToMultiByte (CP_UTF8, 0, argv[i + 1], -1, buf, sizeof (buf), nullptr, nullptr);
			return std::string (buf);
		}
	}
	return std::string ();
}

static unsigned
parse_period (int argc, wchar_t** argv)
{
	for (int i = 1; i < argc; ++i) {
		if (wcscmp (argv[i], L"--period") == 0 && i + 1 < argc) {
			return (unsigned)wcstoul (argv[i + 1], nullptr, 10);
		}
		if (wcsncmp (argv[i], L"--period=", 9) == 0) {
			return (unsigned)wcstoul (argv[i] + 9, nullptr, 10);
		}
	}
	return 0;
}

static bool
parse_flag (int argc, wchar_t** argv, const wchar_t* name)
{
	for (int i = 1; i < argc; ++i) {
		if (wcscmp (argv[i], name) == 0) {
			return true;
		}
	}
	return false;
}

int
wmain (int argc, wchar_t** argv)
{
	unsigned timeout = parse_timeout (argc, argv);
	std::string backend = parse_backend (argc, argv);
	unsigned period = parse_period (argc, argv);
	bool no_engine = parse_flag (argc, argv, L"--no-engine");
	bool smoke_import = parse_flag (argc, argv, L"--smoke-import");
	bool smoke_record = parse_flag (argc, argv, L"--smoke-record");
	bool smoke_save_load = parse_flag (argc, argv, L"--smoke-save-load");
	bool smoke_export = parse_flag (argc, argv, L"--smoke-export");
	bool smoke_undo = parse_flag (argc, argv, L"--smoke-undo");
	bool smoke_loop = parse_flag (argc, argv, L"--smoke-loop");
	bool smoke_midi = parse_flag (argc, argv, L"--smoke-midi");
	bool smoke_midi_edit = parse_flag (argc, argv, L"--smoke-midi-edit");
	bool smoke_plugin = parse_flag (argc, argv, L"--smoke-plugin");
	bool smoke_vst3 = parse_flag (argc, argv, L"--smoke-vst3");
	bool smoke_mute_solo = parse_flag (argc, argv, L"--smoke-mute-solo");
	bool smoke_pan = parse_flag (argc, argv, L"--smoke-pan");
	bool smoke_send = parse_flag (argc, argv, L"--smoke-send");
	bool smoke_automation = parse_flag (argc, argv, L"--smoke-automation");
	bool smoke_edit = parse_flag (argc, argv, L"--smoke-edit");
	bool smoke_snap = parse_flag (argc, argv, L"--smoke-snap");
	bool smoke_punch = parse_flag (argc, argv, L"--smoke-punch");
	bool smoke_tempo = parse_flag (argc, argv, L"--smoke-tempo");
	bool smoke_click = parse_flag (argc, argv, L"--smoke-click");
	bool smoke_tracks = parse_flag (argc, argv, L"--smoke-tracks");
	bool smoke_rename = parse_flag (argc, argv, L"--smoke-rename");
	bool smoke_group = parse_flag (argc, argv, L"--smoke-group");
	bool smoke_monitor = parse_flag (argc, argv, L"--smoke-monitor");
	bool smoke_locate = parse_flag (argc, argv, L"--smoke-locate");
	bool smoke_zoom = parse_flag (argc, argv, L"--smoke-zoom");
	bool smoke_history = parse_flag (argc, argv, L"--smoke-history");
	bool smoke_prefs = parse_flag (argc, argv, L"--smoke-prefs");
	bool smoke_recent = parse_flag (argc, argv, L"--smoke-recent");
	bool smoke_recovery = parse_flag (argc, argv, L"--smoke-recovery");
	bool smoke_stretch = parse_flag (argc, argv, L"--smoke-stretch");
	bool smoke_pitch = parse_flag (argc, argv, L"--smoke-pitch");
	bool smoke_reverse = parse_flag (argc, argv, L"--smoke-reverse");
	bool smoke_normalize = parse_flag (argc, argv, L"--smoke-normalize");
	bool smoke_strip_silence = parse_flag (argc, argv, L"--smoke-strip-silence");
	bool smoke_perf = parse_flag (argc, argv, L"--smoke-perf");
	bool no_splash = parse_flag (argc, argv, L"--no-splash");
	std::string session_path = parse_session_path (argc, argv);
	/* --smoke-perf uses --timeout as the sample window (default 5s). */
	if (smoke_perf && timeout == 0) {
		timeout = 5;
	}
	/* Unattended / smoke: skip splash so --timeout exit-0 stays snappy. */
	if (timeout > 0 || smoke_perf) {
		no_splash = true;
	}
	HINSTANCE instance = GetModuleHandleW (nullptr);

	HRESULT co = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
	(void)co;

	ShellWindow shell;
	if (!shell.create (instance, SW_SHOWNORMAL)) {
		std::fwprintf (stderr, L"airdour_win_shell: failed to create window / Direct2D\n");
		if (SUCCEEDED (co) || co == S_FALSE) {
			CoUninitialize ();
		}
		return 1;
	}

	if (!no_splash) {
		shell.show_splash ();
	}

	std::fwprintf (stderr,
	               L"airdour_win_shell: window up (timeout=%u). Brand=Airdour. DPI host + panes%s.\n",
	               timeout, no_splash ? L" (--no-splash)" : L"");

	if (!no_engine) {
		if (shell.attach_engine (backend, timeout, smoke_import, smoke_record,
		                         smoke_save_load, smoke_export, smoke_undo,
		                         smoke_loop, smoke_midi, smoke_midi_edit,
		                         smoke_plugin, smoke_perf, session_path, period,
		                         smoke_vst3, smoke_mute_solo, smoke_pan, smoke_send,
		                         smoke_automation, smoke_edit, smoke_snap, smoke_punch,
		                         smoke_tempo, smoke_click, smoke_tracks,
		                         smoke_rename, smoke_group, smoke_monitor,
		                         smoke_locate, smoke_zoom, smoke_history,
		                         smoke_prefs, smoke_recent, smoke_recovery,
		                         smoke_stretch, smoke_pitch, smoke_reverse,
		                         smoke_normalize, smoke_strip_silence)) {
			const char* be = backend.empty () ? "(prefs/Dummy)" : backend.c_str ();
			std::fwprintf (stderr, L"airdour_win_shell: engine attached backend=%hs\n", be);
		} else {
			std::fwprintf (stderr, L"airdour_win_shell: engine not attached (ok for UI-only / CI)\n");
		}
	} else {
		std::fwprintf (stderr, L"airdour_win_shell: --no-engine\n");
	}

	/* Perf sampling already ran inside attach_engine; exit the UI promptly. */
	int rc = shell.run_message_loop (smoke_perf ? 1u : timeout);

	if (SUCCEEDED (co) || co == S_FALSE) {
		CoUninitialize ();
	}
	std::fwprintf (stderr, L"airdour_win_shell: exit %d\n", rc);
	return rc;
}
