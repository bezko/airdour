/*
 * Copyright (C) 2026 Airdour contributors
 *
 * airdour_win_shell — Win32 + Direct2D + DirectWrite host.
 *
 * Usage:
 *   airdour_win_shell.exe [--timeout N] [--backend Dummy|WASAPI] [--no-engine]
 *
 * --timeout N     Auto-quit after N seconds (unattended smoke).
 * --backend NAME  Attach AudioEngine (when built WITH_ENGINE). Default: Dummy.
 * --no-engine     Skip engine attach even if linked.
 */

#include "shell_window.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
	HINSTANCE instance = GetModuleHandleW (nullptr);

	/* STA for UI. WASAPI opens on this thread; process load under Session can
	 * race COM apartment use from the audio thread — --timeout smoke skips
	 * auto-roll (see ShellWindow::attach_engine) so ExitProcess stays reliable. */
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
		if (shell.attach_engine (backend, timeout)) {
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
