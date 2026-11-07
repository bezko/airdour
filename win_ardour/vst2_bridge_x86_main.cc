/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Win32 (x86) VST2 bridge process. Built with -A Win32 on MSVC.
 * Loads a 32-bit VST2 DLL and services SharedState ops from the x64 host.
 */

#include "vst2_bridge_protocol.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using airdour::vst2_bridge::kProtocolMagic;
using airdour::vst2_bridge::Op;
using airdour::vst2_bridge::SharedState;

namespace {

SharedState* g_view = nullptr;
HANDLE g_req = nullptr;
HANDLE g_ack = nullptr;
HMODULE g_dll = nullptr;

/* Packed AEffect layout matching Steinberg VST2 on Win32 (32-bit pointers). */
#pragma pack(push, 8)
struct BridgeAEffect {
	int32_t magic;
	intptr_t (*dispatcher) (BridgeAEffect*, int32_t, int32_t, intptr_t, void*, float);
	void (*process) (BridgeAEffect*, float**, float**, int32_t);
	void (*setParameter) (BridgeAEffect*, int32_t, float);
	float (*getParameter) (BridgeAEffect*, int32_t);
	int32_t numPrograms;
	int32_t numParams;
	int32_t numInputs;
	int32_t numOutputs;
	int32_t flags;
	intptr_t resvd1;
	intptr_t resvd2;
	int32_t initialDelay;
	int32_t realQualities;
	int32_t offQualities;
	float ioRatio;
	void* object;
	void* user;
	int32_t uniqueID;
	int32_t version;
	void (*processReplacing) (BridgeAEffect*, float**, float**, int32_t);
	void (*processDoubleReplacing) (BridgeAEffect*, double**, double**, int32_t);
};
#pragma pack(pop)

typedef intptr_t (*AudioMasterProc) (void*, int32_t, int32_t, intptr_t, void*, float);
BridgeAEffect* g_effect = nullptr;

intptr_t
bridge_audio_master (void*, int32_t opcode, int32_t, intptr_t, void*, float)
{
	/* audioMasterVersion */
	if (opcode == 1) {
		return 2400;
	}
	return 0;
}

bool
open_plugin ()
{
	if (!g_view) {
		return false;
	}
	g_dll = LoadLibraryW (g_view->hdr.plugin_path);
	if (!g_dll) {
		std::snprintf (g_view->hdr.error, sizeof (g_view->hdr.error),
			       "LoadLibrary failed (%lu)", (unsigned long)GetLastError ());
		return false;
	}
	typedef BridgeAEffect* (*VstMain) (AudioMasterProc);
	VstMain entry = (VstMain)GetProcAddress (g_dll, "VSTPluginMain");
	if (!entry) {
		entry = (VstMain)GetProcAddress (g_dll, "main");
	}
	if (!entry) {
		std::snprintf (g_view->hdr.error, sizeof (g_view->hdr.error), "no VST entry");
		return false;
	}
	g_effect = entry (bridge_audio_master);
	if (!g_effect || g_effect->magic != 0x56737450 /* 'VstP' */) {
		std::snprintf (g_view->hdr.error, sizeof (g_view->hdr.error), "bad AEffect");
		g_effect = nullptr;
		return false;
	}
	if (g_effect->dispatcher) {
		g_effect->dispatcher (g_effect, 0 /*effOpen*/, 0, 0, nullptr, 0.f);
		g_effect->dispatcher (g_effect, 12 /*effMainsChanged*/, 0, 1, nullptr, 0.f);
		g_effect->dispatcher (g_effect, 71 /*effStartProcess*/, 0, 0, nullptr, 0.f);
	}
	g_view->hdr.n_inputs = g_effect->numInputs > 0 ? (uint32_t)g_effect->numInputs : 0u;
	g_view->hdr.n_outputs = g_effect->numOutputs > 0 ? (uint32_t)g_effect->numOutputs : 0u;
	g_view->hdr.n_params = g_effect->numParams > 0 ? (uint32_t)g_effect->numParams : 0u;
	g_view->hdr.error[0] = 0;
	return true;
}

void
close_plugin ()
{
	if (g_effect && g_effect->dispatcher) {
		g_effect->dispatcher (g_effect, 72 /*effStopProcess*/, 0, 0, nullptr, 0.f);
		g_effect->dispatcher (g_effect, 12 /*effMainsChanged*/, 0, 0, nullptr, 0.f);
		g_effect->dispatcher (g_effect, 1 /*effClose*/, 0, 0, nullptr, 0.f);
		g_effect = nullptr;
	}
	if (g_dll) {
		FreeLibrary (g_dll);
		g_dll = nullptr;
	}
}

void
handle_op ()
{
	if (!g_view) {
		return;
	}
	Op op = (Op)g_view->hdr.op;
	g_view->hdr.status = 0;
	g_view->hdr.error[0] = 0;
	switch (op) {
	case Op::Open:
		if (!open_plugin ()) {
			g_view->hdr.status = 1;
		}
		break;
	case Op::Shutdown:
		close_plugin ();
		break;
	case Op::SetBlockSize:
		if (g_effect && g_effect->dispatcher) {
			g_effect->dispatcher (g_effect, 11 /*effSetBlockSize*/, 0,
					      (intptr_t)g_view->hdr.frames, nullptr, 0.f);
		}
		break;
	case Op::SetSampleRate:
		if (g_effect && g_effect->dispatcher) {
			g_effect->dispatcher (g_effect, 10 /*effSetSampleRate*/, 0, 0, nullptr,
					      g_view->hdr.sample_rate);
		}
		break;
	case Op::Process:
		if (g_effect) {
			float* inputs[airdour::vst2_bridge::kMaxChannels];
			float* outputs[airdour::vst2_bridge::kMaxChannels];
			for (uint32_t c = 0; c < airdour::vst2_bridge::kMaxChannels; ++c) {
				inputs[c] = g_view->audio.in[c];
				outputs[c] = g_view->audio.out[c];
			}
			/* Feed host MIDI into the plugin before audio (instruments). */
			uint32_t n_midi = g_view->hdr.midi_count;
			if (n_midi > airdour::vst2_bridge::kMaxMidiEvents) {
				n_midi = airdour::vst2_bridge::kMaxMidiEvents;
			}
			if (n_midi > 0 && g_effect->dispatcher) {
#pragma pack(push, 8)
				struct LocalMidiEvent {
					int32_t type;
					int32_t byteSize;
					int32_t deltaSamples;
					int32_t flags;
					int32_t noteLength;
					int32_t noteOffset;
					char midiData[4];
					char detune;
					char noteOffVelocity;
					char reserved1;
					char reserved2;
				};
				struct LocalVstEvents {
					int32_t numEvents;
					void* reserved;
					void* events[airdour::vst2_bridge::kMaxMidiEvents];
				};
#pragma pack(pop)
				LocalMidiEvent midi_store[airdour::vst2_bridge::kMaxMidiEvents];
				LocalVstEvents ve {};
				ve.numEvents = (int32_t)n_midi;
				ve.reserved = nullptr;
				for (uint32_t i = 0; i < n_midi; ++i) {
					std::memset (&midi_store[i], 0, sizeof (midi_store[i]));
					midi_store[i].type = 1; /* kVstMidiType */
					midi_store[i].byteSize = (int32_t)sizeof (LocalMidiEvent);
					midi_store[i].deltaSamples =
						(int32_t)g_view->audio.midi[i].sample_offset;
					uint8_t sz = g_view->audio.midi[i].size;
					if (sz > 3) {
						sz = 3;
					}
					for (uint8_t b = 0; b < sz; ++b) {
						midi_store[i].midiData[b] =
							(char)g_view->audio.midi[i].data[b];
					}
					ve.events[i] = &midi_store[i];
				}
				g_effect->dispatcher (g_effect, 25 /*effProcessEvents*/, 0, 0, &ve,
						      0.f);
			}
			if (g_effect->processReplacing) {
				g_effect->processReplacing (g_effect, inputs, outputs,
							    (int32_t)g_view->hdr.frames);
			} else if (g_effect->process) {
				g_effect->process (g_effect, inputs, outputs,
						   (int32_t)g_view->hdr.frames);
			} else {
				g_view->hdr.status = 1;
				std::snprintf (g_view->hdr.error, sizeof (g_view->hdr.error),
					       "no process entry");
			}
		} else {
			g_view->hdr.status = 1;
		}
		break;
	case Op::GetParameter:
		if (g_effect && g_effect->getParameter) {
			g_view->hdr.param_value =
				g_effect->getParameter (g_effect, (int32_t)g_view->hdr.param_index);
		} else {
			g_view->hdr.status = 1;
		}
		break;
	case Op::SetParameter:
		if (g_effect && g_effect->setParameter) {
			g_effect->setParameter (g_effect, (int32_t)g_view->hdr.param_index,
						g_view->hdr.param_value);
		} else {
			g_view->hdr.status = 1;
		}
		break;
	default:
		break;
	}
}

} // namespace

int
wmain (int argc, wchar_t** argv)
{
	const wchar_t* shm_name = nullptr;
	const wchar_t* req_name = nullptr;
	const wchar_t* ack_name = nullptr;
	for (int i = 1; i < argc; ++i) {
		if (!wcscmp (argv[i], L"--shm") && i + 1 < argc) {
			shm_name = argv[++i];
		} else if (!wcscmp (argv[i], L"--req") && i + 1 < argc) {
			req_name = argv[++i];
		} else if (!wcscmp (argv[i], L"--ack") && i + 1 < argc) {
			ack_name = argv[++i];
		}
	}
	if (!shm_name || !req_name || !ack_name) {
		std::fwprintf (stderr, L"usage: ardour-vst2-bridge-x86 --shm NAME --req NAME --ack NAME\n");
		return 2;
	}

	HANDLE shm = OpenFileMappingW (FILE_MAP_ALL_ACCESS, FALSE, shm_name);
	if (!shm) {
		return 3;
	}
	g_view = (SharedState*)MapViewOfFile (shm, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (SharedState));
	if (!g_view || g_view->hdr.magic != kProtocolMagic) {
		return 4;
	}
	g_req = OpenEventW (EVENT_ALL_ACCESS, FALSE, req_name);
	g_ack = OpenEventW (EVENT_ALL_ACCESS, FALSE, ack_name);
	if (!g_req || !g_ack) {
		return 5;
	}

	for (;;) {
		DWORD w = WaitForSingleObject (g_req, INFINITE);
		if (w != WAIT_OBJECT_0) {
			break;
		}
		Op op = (Op)g_view->hdr.op;
		handle_op ();
		SetEvent (g_ack);
		if (op == Op::Shutdown) {
			break;
		}
	}

	close_plugin ();
	return 0;
}
