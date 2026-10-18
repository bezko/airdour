/*
 * Copyright (C) 2026 Airdour contributors
 *
 * airdour_win_shell — minimal Win32 + Direct2D + DirectWrite host.
 *
 * Usage:
 *   airdour_win_shell.exe [--timeout N]
 *
 * --timeout N   Auto-quit after N seconds (unattended smoke on CI/laptop agents).
 */

#include "shell_window.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

/* Console subsystem entry (capturable smoke logs) + HWND UI. */
int
wmain (int argc, wchar_t** argv)
{
	unsigned timeout = parse_timeout (argc, argv);
	HINSTANCE instance = GetModuleHandleW (nullptr);
	int show_cmd = SW_SHOWNORMAL;

	/* Apartment-friendly for later WASAPI COM attach from this process. */
	HRESULT co = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
	(void)co;

	ShellWindow shell;
	if (!shell.create (instance, show_cmd)) {
		std::fwprintf (stderr, L"airdour_win_shell: failed to create window / Direct2D\n");
		if (SUCCEEDED (co) || co == S_FALSE) {
			CoUninitialize ();
		}
		return 1;
	}

	std::fwprintf (stderr,
	               L"airdour_win_shell: window up (timeout=%u s). Brand=Airdour, D2D+DirectWrite.\n",
	               timeout);

	int rc = shell.run_message_loop (timeout);

	if (SUCCEEDED (co) || co == S_FALSE) {
		CoUninitialize ();
	}
	std::fwprintf (stderr, L"airdour_win_shell: exit %d\n", rc);
	return rc;
}
