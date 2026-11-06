/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "vst2_bridge_client.h"
#include "vst2_bridge_protocol.h"

#include <cstddef>
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
	HANDLE shm = nullptr;
	SharedState* view = nullptr;
	HANDLE req_event = nullptr;
	HANDLE ack_event = nullptr;
	std::string plugin_path;
	bool alive = false;
	bool rate_set = false;
	bool block_set = false;
	float last_rate = 0.f;
	uint32_t last_block = 0;
};

std::vector<BridgeProc> g_bridges;
uint32_t g_bridge_serial = 0;

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
			WaitForSingleObject (b.ack_event, 500);
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

BridgeProc*
find_bridge (const std::string& plugin_dll_path)
{
	for (size_t i = 0; i < g_bridges.size (); ++i) {
		if (g_bridges[i].alive && g_bridges[i].plugin_path == plugin_dll_path) {
			return &g_bridges[i];
		}
	}
	return nullptr;
}

bool
wait_op (BridgeProc& b, Op op, DWORD timeout_ms)
{
	if (!b.view || !b.req_event || !b.ack_event) {
		return false;
	}
	b.view->hdr.op = (uint32_t)op;
	b.view->hdr.status = 1;
	SetEvent (b.req_event);
	if (WaitForSingleObject (b.ack_event, timeout_ms) != WAIT_OBJECT_0) {
		return false;
	}
	return b.view->hdr.status == 0;
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
	if (ReadFile (f, &dos, sizeof (dos), &rd, nullptr) && rd == sizeof (dos) && dos.e_magic == IMAGE_DOS_SIGNATURE) {
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
	if (find_bridge (plugin_dll_path)) {
		return true;
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
	uint32_t serial = ++g_bridge_serial;
	wchar_t shm_name[128];
	wchar_t req_name[128];
	wchar_t ack_name[128];
	_snwprintf_s (shm_name, _TRUNCATE, L"Local\\AirdourVST2Bridge-%lu-%u",
		      (unsigned long)pid, (unsigned)serial);
	_snwprintf_s (req_name, _TRUNCATE, L"Local\\AirdourVST2BridgeReq-%lu-%u",
		      (unsigned long)pid, (unsigned)serial);
	_snwprintf_s (ack_name, _TRUNCATE, L"Local\\AirdourVST2BridgeAck-%lu-%u",
		      (unsigned long)pid, (unsigned)serial);

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

	std::wstring cmd = utf8_to_wide (exe) + L" --shm " + shm_name + L" --req " + req_name + L" --ack " + ack_name;
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

	if (!wait_op (b, Op::Open, 8000)) {
		std::fprintf (stderr, "airdour_win_shell: bridge Open failed (%s)\n",
			      b.view ? b.view->hdr.error : "?");
		std::fflush (stderr);
		close_bridge (b);
		return false;
	}
	b.alive = true;
	g_bridges.push_back (b);
	std::fprintf (stderr, "airdour_win_shell: vst2-bridge started for %s (in=%u out=%u params=%u)\n",
		      plugin_dll_path.c_str (),
		      (unsigned)(b.view ? b.view->hdr.n_inputs : 0),
		      (unsigned)(b.view ? b.view->hdr.n_outputs : 0),
		      (unsigned)(b.view ? b.view->hdr.n_params : 0));
	std::fflush (stderr);
	return true;
}

bool
get_io_counts (const std::string& plugin_dll_path, uint32_t* n_in, uint32_t* n_out,
	       uint32_t* n_params)
{
	BridgeProc* b = find_bridge (plugin_dll_path);
	if (!b || !b->view) {
		return false;
	}
	if (n_in) {
		*n_in = b->view->hdr.n_inputs;
	}
	if (n_out) {
		*n_out = b->view->hdr.n_outputs;
	}
	if (n_params) {
		*n_params = b->view->hdr.n_params;
	}
	return true;
}

bool
set_sample_rate (const std::string& plugin_dll_path, float sample_rate)
{
	BridgeProc* b = find_bridge (plugin_dll_path);
	if (!b || !b->view) {
		return false;
	}
	if (b->rate_set && b->last_rate == sample_rate) {
		return true;
	}
	b->view->hdr.sample_rate = sample_rate > 0.f ? sample_rate : 48000.f;
	if (!wait_op (*b, Op::SetSampleRate, 2000)) {
		return false;
	}
	b->rate_set = true;
	b->last_rate = b->view->hdr.sample_rate;
	return true;
}

bool
set_block_size (const std::string& plugin_dll_path, uint32_t frames)
{
	BridgeProc* b = find_bridge (plugin_dll_path);
	if (!b || !b->view) {
		return false;
	}
	if (frames == 0) {
		frames = 64;
	}
	if (frames > kMaxBlock) {
		frames = kMaxBlock;
	}
	if (b->block_set && b->last_block == frames) {
		return true;
	}
	b->view->hdr.frames = frames;
	if (!wait_op (*b, Op::SetBlockSize, 2000)) {
		return false;
	}
	b->block_set = true;
	b->last_block = frames;
	return true;
}

bool
process_audio (const std::string& plugin_dll_path,
	       float* const* inputs, float* const* outputs,
	       uint32_t n_inputs, uint32_t n_outputs,
	       uint32_t frames, float sample_rate,
	       uint32_t wait_ms,
	       const MidiEventWire* midi,
	       uint32_t midi_count)
{
	BridgeProc* b = find_bridge (plugin_dll_path);
	if (!b || !b->view || !outputs) {
		return false;
	}
	if (frames == 0) {
		frames = 64;
	}
	if (frames > kMaxBlock) {
		frames = kMaxBlock;
	}
	if (!set_sample_rate (plugin_dll_path, sample_rate) || !set_block_size (plugin_dll_path, frames)) {
		return false;
	}
	if (n_inputs > kMaxChannels) {
		n_inputs = kMaxChannels;
	}
	if (n_outputs > kMaxChannels) {
		n_outputs = kMaxChannels;
	}
	std::memset (b->view->audio.in, 0, sizeof (b->view->audio.in));
	std::memset (b->view->audio.out, 0, sizeof (b->view->audio.out));
	for (uint32_t c = 0; c < n_inputs; ++c) {
		if (inputs && inputs[c]) {
			std::memcpy (b->view->audio.in[c], inputs[c], frames * sizeof (float));
		}
	}
	if (midi_count > kMaxMidiEvents) {
		midi_count = kMaxMidiEvents;
	}
	b->view->hdr.midi_count = midi_count;
	if (midi && midi_count > 0) {
		std::memcpy (b->view->audio.midi, midi, midi_count * sizeof (MidiEventWire));
	} else {
		std::memset (b->view->audio.midi, 0, sizeof (b->view->audio.midi));
	}
	b->view->hdr.frames = frames;
	b->view->hdr.n_inputs = n_inputs;
	b->view->hdr.n_outputs = n_outputs;
	if (wait_ms == 0) {
		wait_ms = 50;
	}
	if (!wait_op (*b, Op::Process, wait_ms)) {
		return false;
	}
	for (uint32_t c = 0; c < n_outputs; ++c) {
		if (outputs[c]) {
			std::memcpy (outputs[c], b->view->audio.out[c], frames * sizeof (float));
		}
	}
	return true;
}

bool
process_silence_block (uint32_t frames, float sample_rate)
{
	if (g_bridges.empty () || !g_bridges[0].alive) {
		return false;
	}
	float zeros_in[kMaxChannels][64];
	float zeros_out[kMaxChannels][64];
	std::memset (zeros_in, 0, sizeof (zeros_in));
	std::memset (zeros_out, 0, sizeof (zeros_out));
	if (frames == 0 || frames > 64) {
		frames = 64;
	}
	float* in_ptrs[kMaxChannels];
	float* out_ptrs[kMaxChannels];
	for (uint32_t c = 0; c < kMaxChannels; ++c) {
		in_ptrs[c] = zeros_in[c];
		out_ptrs[c] = zeros_out[c];
	}
	return process_audio (g_bridges[0].plugin_path, in_ptrs, out_ptrs, 2, 2, frames,
			      sample_rate, 2000);
}

bool
get_parameter (const std::string& plugin_dll_path, uint32_t index, float* out_norm)
{
	BridgeProc* b = find_bridge (plugin_dll_path);
	if (!b || !b->view || !out_norm) {
		return false;
	}
	b->view->hdr.param_index = index;
	if (!wait_op (*b, Op::GetParameter, 2000)) {
		return false;
	}
	*out_norm = b->view->hdr.param_value;
	return true;
}

bool
set_parameter (const std::string& plugin_dll_path, uint32_t index, float norm)
{
	BridgeProc* b = find_bridge (plugin_dll_path);
	if (!b || !b->view) {
		return false;
	}
	b->view->hdr.param_index = index;
	b->view->hdr.param_value = norm;
	return wait_op (*b, Op::SetParameter, 2000);
}

void
shutdown_plugin (const std::string& plugin_dll_path)
{
	for (size_t i = 0; i < g_bridges.size ();) {
		if (plugin_dll_path.empty () || g_bridges[i].plugin_path == plugin_dll_path) {
			close_bridge (g_bridges[i]);
			g_bridges.erase (g_bridges.begin () + (std::ptrdiff_t)i);
		} else {
			++i;
		}
	}
}

void
shutdown_all ()
{
	shutdown_plugin (std::string ());
}

} /* namespace vst2_bridge */
} /* namespace airdour */
