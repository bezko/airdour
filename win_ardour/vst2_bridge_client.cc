/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "vst2_bridge_client.h"
#include "vst2_bridge_protocol.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace airdour {
namespace vst2_bridge {
namespace {

struct BridgeProc {
	PROCESS_INFORMATION pi {};
	HANDLE              shm = nullptr;
	SharedState*        view = nullptr;
	HANDLE              req_event = nullptr;
	HANDLE              ack_event = nullptr;
	std::string         plugin_path;
	bool                alive = false;
};

std::vector<BridgeProc> g_bridges;

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
wide_to_utf8 (const std::wstring& s)
{
	if (s.empty ()) {
		return std::string ();
	}
	int n = WideCharToMultiByte (CP_UTF8, 0, s.c_str (), (int)s.size (), nullptr, 0, nullptr, nullptr);
	if (n <= 0) {
		return std::string ();
	}
	std::string out ((size_t)n, '\0');
	WideCharToMultiByte (CP_UTF8, 0, s.c_str (), (int)s.size (), &out[0], n, nullptr, nullptr);
	return out;
}

void
close_bridge (BridgeProc& b)
{
	if (b.view && b.shm) {
		b.view->hdr.op = (uint32_t)Op::Shutdown;
		if (b.req_event) {
			SetEvent (b.req_event);
		}
	}
	if (b.pi.hProcess) {
		WaitForSingleObject (b.pi.hProcess, 500);
		TerminateProcess (b.pi.hProcess, 0);
		CloseHandle (b.pi.hProcess);
		CloseHandle (b.pi.hThread);
		b.pi = {};
	}
	if (b.view) {
		UnmapViewOfFile (b.view);
		b.view = nullptr;
	}
	if (b.shm) {
		CloseHandle (b.shm);
		b.shm = nullptr;
	}
	if (b.req_event) {
		CloseHandle (b.req_event);
		b.req_event = nullptr;
	}
	if (b.ack_event) {
		CloseHandle (b.ack_event);
		b.ack_event = nullptr;
	}
	b.alive = false;
}

} // namespace

std::string
bridge_exe_path ()
{
	wchar_t mod[MAX_PATH];
	DWORD n = GetModuleFileNameW (nullptr, mod, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		return std::string ();
	}
	std::wstring path (mod);
	size_t slash = path.find_last_of (L"\\/");
	if (slash == std::wstring::npos) {
		return std::string ();
	}
	std::wstring exe = path.substr (0, slash + 1) + L"ardour-vst2-bridge-x86.exe";
	DWORD attr = GetFileAttributesW (exe.c_str ());
	if (attr == INVALID_FILE_ATTRIBUTES) {
		/* Also try tools staging name used by packaging. */
		exe = path.substr (0, slash + 1) + L"vst2-bridge-x86.exe";
		attr = GetFileAttributesW (exe.c_str ());
		if (attr == INVALID_FILE_ATTRIBUTES) {
			return std::string ();
		}
	}
	return wide_to_utf8 (exe);
}

bool
dll_is_i386 (const std::string& path)
{
	std::wstring w = utf8_to_wide (path);
	HANDLE f = CreateFileW (w.c_str (), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) {
		return false;
	}
	IMAGE_DOS_HEADER dos {};
	DWORD rd = 0;
	bool ok = false;
	if (ReadFile (f, &dos, sizeof (dos), &rd, nullptr) && rd == sizeof (dos)
	    && dos.e_magic == IMAGE_DOS_SIGNATURE) {
		SetFilePointer (f, dos.e_lfanew, nullptr, FILE_BEGIN);
		DWORD pe_sig = 0;
		if (ReadFile (f, &pe_sig, sizeof (pe_sig), &rd, nullptr) && pe_sig == IMAGE_NT_SIGNATURE) {
			IMAGE_FILE_HEADER fh {};
			if (ReadFile (f, &fh, sizeof (fh), &rd, nullptr) && rd == sizeof (fh)) {
				ok = (fh.Machine == IMAGE_FILE_MACHINE_I386);
			}
		}
	}
	CloseHandle (f);
	return ok;
}

bool
ensure_bridge_running (const std::string& plugin_dll_path)
{
	for (size_t i = 0; i < g_bridges.size (); ++i) {
		if (g_bridges[i].alive && g_bridges[i].plugin_path == plugin_dll_path) {
			return true;
		}
	}

	std::string exe = bridge_exe_path ();
	if (exe.empty ()) {
		std::fprintf (stderr, "airdour_win_shell: vst2-bridge exe not found beside shell\n");
		std::fflush (stderr);
		return false;
	}

	BridgeProc b;
	b.plugin_path = plugin_dll_path;
	DWORD pid = GetCurrentProcessId ();
	wchar_t shm_name[128];
	wchar_t req_name[128];
	wchar_t ack_name[128];
	_snwprintf_s (shm_name, _TRUNCATE, L"Local\\AirdourVST2Bridge-%lu", (unsigned long)pid);
	_snwprintf_s (req_name, _TRUNCATE, L"Local\\AirdourVST2BridgeReq-%lu", (unsigned long)pid);
	_snwprintf_s (ack_name, _TRUNCATE, L"Local\\AirdourVST2BridgeAck-%lu", (unsigned long)pid);

	b.shm = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
	                            (DWORD)sizeof (SharedState), shm_name);
	if (!b.shm) {
		return false;
	}
	b.view = (SharedState*)MapViewOfFile (b.shm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (SharedState));
	if (!b.view) {
		CloseHandle (b.shm);
		return false;
	}
	std::memset (b.view, 0, sizeof (SharedState));
	b.view->hdr.magic = kProtocolMagic;
	b.view->hdr.version = kProtocolVersion;
	std::wstring wplug = utf8_to_wide (plugin_dll_path);
	wcsncpy_s (b.view->hdr.plugin_path, kMaxPathChars, wplug.c_str (), _TRUNCATE);

	b.req_event = CreateEventW (nullptr, FALSE, FALSE, req_name);
	b.ack_event = CreateEventW (nullptr, FALSE, FALSE, ack_name);
	if (!b.req_event || !b.ack_event) {
		close_bridge (b);
		return false;
	}

	std::wstring cmd = utf8_to_wide (exe) + L" --shm " + shm_name + L" --req " + req_name
	                   + L" --ack " + ack_name;
	STARTUPINFOW si {};
	si.cb = sizeof (si);
	std::vector<wchar_t> cmdline (cmd.begin (), cmd.end ());
	cmdline.push_back (L'\0');
	if (!CreateProcessW (nullptr, cmdline.data (), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
	                     &si, &b.pi)) {
		std::fprintf (stderr, "airdour_win_shell: CreateProcess bridge failed (%lu)\n",
		              (unsigned long)GetLastError ());
		std::fflush (stderr);
		close_bridge (b);
		return false;
	}

	/* Open plugin. */
	b.view->hdr.op = (uint32_t)Op::Open;
	b.view->hdr.status = 1;
	SetEvent (b.req_event);
	DWORD wr = WaitForSingleObject (b.ack_event, 8000);
	if (wr != WAIT_OBJECT_0 || b.view->hdr.status != 0) {
		std::fprintf (stderr, "airdour_win_shell: bridge Open failed (%s)\n",
		              b.view->hdr.error);
		std::fflush (stderr);
		close_bridge (b);
		return false;
	}
	b.alive = true;
	g_bridges.push_back (b);
	std::fprintf (stderr, "airdour_win_shell: vst2-bridge started for %s\n",
	              plugin_dll_path.c_str ());
	std::fflush (stderr);
	return true;
}

void
shutdown_all ()
{
	for (size_t i = 0; i < g_bridges.size (); ++i) {
		close_bridge (g_bridges[i]);
	}
	g_bridges.clear ();
}

} /* namespace vst2_bridge */
} /* namespace airdour */
