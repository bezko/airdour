/*
 * Copyright (C) 2026 Airdour contributors
 *
 * airdour_win_shell — Win32 + Direct2D + DirectWrite host.
 *
 * Usage:
 *   airdour_win_shell.exe [--timeout N] [--backend Dummy|WASAPI] [--no-engine]
 *                         [--smoke-<name> ...] [--no-splash] [--period N]
 *                         [--session DIR] [--new-session PATH]
 *
 * The full --smoke-* set is whatever ShellWindow::run_smoke_check / has_smoke
 * dispatch. Each --smoke-* argument is stored once and looked up by name.
 */

#include "shell_window.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <set>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#pragma comment(lib, "ole32.lib")

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE
#include "pbd/error.h"
#include "pbd/receiver.h"

namespace {

/*
 * Ardour's info/warning/error transmitters are silent unless something is
 * listening, and PBD::fatal ends the process on Windows. Without a listener an
 * engine start that bails out says nothing at all and merely exits 1, so route
 * every channel to stderr where the smoke logs can see it.
 */
class StderrReceiver : public Receiver
{
  public:
	StderrReceiver ()
	{
		listen_to (PBD::info);
		listen_to (PBD::warning);
		listen_to (PBD::error);
		listen_to (PBD::fatal);
	}

  protected:
	void receive (Transmitter::Channel ch, const char* msg) override
	{
		const char* tag = "info";

		switch (ch) {
		case Transmitter::Debug:
			tag = "debug";
			break;
		case Transmitter::Info:
			tag = "info";
			break;
		case Transmitter::Warning:
			tag = "warning";
			break;
		case Transmitter::Error:
			tag = "error";
			break;
		case Transmitter::Fatal:
			tag = "fatal";
			break;
		case Transmitter::Throw:
			tag = "throw";
			break;
		}

		std::fprintf (stderr, "airdour_win_shell: %s: %s", tag, msg);
		std::fflush (stderr);
	}
};

} /* anonymous namespace */
#endif /* AIRDOUR_WIN_SHELL_WITH_ENGINE */

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

/* Full target folder path for --new-session (parent + name derived from it). */
static std::string
parse_new_session_path (int argc, wchar_t** argv)
{
	for (int i = 1; i < argc; ++i) {
		if (wcscmp (argv[i], L"--new-session") == 0 && i + 1 < argc) {
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

static std::set<std::string>
parse_smoke_flags (int argc, wchar_t** argv)
{
	std::set<std::string> flags;
	for (int i = 1; i < argc; ++i) {
		if (wcsncmp (argv[i], L"--smoke-", 8) == 0) {
			char buf[128];
			WideCharToMultiByte (CP_UTF8, 0, argv[i] + 8, -1, buf, sizeof (buf), nullptr, nullptr);
			flags.insert (std::string (buf));
		}
	}
	return flags;
}

int
wmain (int argc, wchar_t** argv)
{
#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE
	static StderrReceiver log_receiver;
#endif
	unsigned timeout = parse_timeout (argc, argv);
	std::string backend = parse_backend (argc, argv);
	unsigned period = parse_period (argc, argv);
	bool no_engine = parse_flag (argc, argv, L"--no-engine");
	bool no_splash = parse_flag (argc, argv, L"--no-splash");
	std::set<std::string> smoke_flags = parse_smoke_flags (argc, argv);
	const bool smoke_perf = smoke_flags.count ("perf") > 0;
	std::string session_path = parse_session_path (argc, argv);
	std::string new_session_path = parse_new_session_path (argc, argv);
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
		shell.set_smoke_flags (smoke_flags);
		if (shell.attach_engine (backend, timeout, session_path, period)) {
			const char* be = backend.empty () ? "(prefs/Dummy)" : backend.c_str ();
			std::fwprintf (stderr, L"airdour_win_shell: engine attached backend=%hs\n", be);
		} else {
			std::fwprintf (stderr, L"airdour_win_shell: engine not attached (ok for UI-only / CI)\n");
		}
	} else {
		std::fwprintf (stderr, L"airdour_win_shell: --no-engine\n");
	}

	if (!no_engine && !new_session_path.empty ()) {
		shell.new_session_at_path (new_session_path);
	}

	/* Perf sampling already ran inside attach_engine; exit the UI promptly. */
	int rc = shell.run_message_loop (smoke_perf ? 1u : timeout);

	if (SUCCEEDED (co) || co == S_FALSE) {
		CoUninitialize ();
	}
	std::fwprintf (stderr, L"airdour_win_shell: exit %d\n", rc);
	return rc;
}
