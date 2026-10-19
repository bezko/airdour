/*
 * Copyright (C) 2026 Airdour contributors
 *
 * airdour_win_shell — Win32 + Direct2D + DirectWrite host.
 *
 * Usage:
 *   airdour_win_shell.exe [--timeout N] [--backend Dummy|WASAPI] [--no-engine]
 *                         [--smoke-import] [--smoke-record] [--smoke-save-load]
 *                         [--smoke-export] [--smoke-undo] [--smoke-loop]
 *                         [--smoke-midi] [--smoke-midi-edit] [--session DIR]
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
	return "Dummy";
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
	bool no_engine = parse_flag (argc, argv, L"--no-engine");
	bool smoke_import = parse_flag (argc, argv, L"--smoke-import");
	bool smoke_record = parse_flag (argc, argv, L"--smoke-record");
	bool smoke_save_load = parse_flag (argc, argv, L"--smoke-save-load");
	bool smoke_export = parse_flag (argc, argv, L"--smoke-export");
	bool smoke_undo = parse_flag (argc, argv, L"--smoke-undo");
	bool smoke_loop = parse_flag (argc, argv, L"--smoke-loop");
	bool smoke_midi = parse_flag (argc, argv, L"--smoke-midi");
	bool smoke_midi_edit = parse_flag (argc, argv, L"--smoke-midi-edit");
	std::string session_path = parse_session_path (argc, argv);
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

	std::fwprintf (stderr,
	               L"airdour_win_shell: window up (timeout=%u). Brand=Airdour. DPI host + panes.\n",
	               timeout);

	if (!no_engine) {
		if (shell.attach_engine (backend, timeout, smoke_import, smoke_record,
		                         smoke_save_load, smoke_export, smoke_undo,
		                         smoke_loop, smoke_midi, smoke_midi_edit,
		                         session_path)) {
			std::fwprintf (stderr, L"airdour_win_shell: engine attached backend=%hs\n", backend.c_str ());
		} else {
			std::fwprintf (stderr, L"airdour_win_shell: engine not attached (ok for UI-only / CI)\n");
		}
	} else {
		std::fwprintf (stderr, L"airdour_win_shell: --no-engine\n");
	}

	int rc = shell.run_message_loop (timeout);

	if (SUCCEEDED (co) || co == S_FALSE) {
		CoUninitialize ();
	}
	std::fwprintf (stderr, L"airdour_win_shell: exit %d\n", rc);
	return rc;
}
