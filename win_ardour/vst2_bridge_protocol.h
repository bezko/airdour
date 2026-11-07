/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Shared-memory + event protocol for hosting 32-bit VST2 in a Win32 bridge
 * process from the x64 Airdour shell (Phase 4).
 */
#pragma once

#include <cstdint>

namespace airdour {
namespace vst2_bridge {

static constexpr uint32_t kProtocolMagic = 0x41325632u; /* 'A2V2' */
static constexpr uint32_t kProtocolVersion = 1;
static constexpr uint32_t kMaxPathChars = 512;
static constexpr uint32_t kMaxChannels = 8;
static constexpr uint32_t kMaxBlock = 8192;
static constexpr uint32_t kMaxMidiEvents = 256;

enum class Op : uint32_t {
	Nop = 0,
	Open = 1,
	Close = 2,
	SetBlockSize = 3,
	SetSampleRate = 4,
	Process = 5,
	GetParameter = 6,
	SetParameter = 7,
	GetChunk = 8,
	SetChunk = 9,
	Idle = 10,
	Shutdown = 11,
};

#pragma pack(push, 1)
struct MidiEventWire {
	uint32_t sample_offset;
	uint8_t size;
	uint8_t data[3];
	uint8_t pad;
};

struct SharedHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t op;     /* Op */
	uint32_t status; /* 0=ok, nonzero=error */
	uint32_t frames;
	uint32_t n_inputs;
	uint32_t n_outputs;
	uint32_t param_index;
	float param_value;
	float sample_rate;
	uint32_t midi_count;
	uint32_t chunk_bytes;
	uint32_t n_params; /* filled by Open */
	wchar_t plugin_path[kMaxPathChars];
	char error[128];
};

struct SharedAudio {
	float in[kMaxChannels][kMaxBlock];
	float out[kMaxChannels][kMaxBlock];
	MidiEventWire midi[kMaxMidiEvents];
};

struct SharedState {
	SharedHeader hdr;
	SharedAudio audio;
};
#pragma pack(pop)

inline const wchar_t*
shm_name_for_pid (unsigned long host_pid, wchar_t* buf, size_t n)
{
	/* Caller formats: AirdourVST2Bridge-<pid> */
	(void)host_pid;
	(void)buf;
	(void)n;
	return buf;
}

} /* namespace vst2_bridge */
} /* namespace airdour */
