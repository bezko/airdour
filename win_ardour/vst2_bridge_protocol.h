/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Shared-memory + event protocol for hosting 32-bit VST2 in a Win32 bridge
 * process from the x64 Airdour shell (Phase 4).
 */
#pragma once

#include <cstddef>
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
	Open = 1,
	SetBlockSize = 3,
	SetSampleRate = 4,
	Process = 5,
	GetParameter = 6,
	SetParameter = 7,
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
	uint32_t reserved_chunk; /* v1 slot, unused */
	uint32_t n_params;       /* filled by Open */
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

static_assert (kProtocolVersion == 1u, "v1 wire");
static_assert (static_cast<uint32_t> (Op::Open) == 1u, "Open");
static_assert (static_cast<uint32_t> (Op::SetBlockSize) == 3u, "SetBlockSize");
static_assert (static_cast<uint32_t> (Op::SetSampleRate) == 4u, "SetSampleRate");
static_assert (static_cast<uint32_t> (Op::Process) == 5u, "Process");
static_assert (static_cast<uint32_t> (Op::GetParameter) == 6u, "GetParameter");
static_assert (static_cast<uint32_t> (Op::SetParameter) == 7u, "SetParameter");
static_assert (static_cast<uint32_t> (Op::Shutdown) == 11u, "Shutdown");
static_assert (offsetof (SharedHeader, op) == 8, "op");
static_assert (offsetof (SharedHeader, midi_count) == 40, "midi_count");
static_assert (offsetof (SharedHeader, reserved_chunk) == 44, "v1 chunk slot");
static_assert (offsetof (SharedHeader, n_params) == 48, "n_params");
static_assert (offsetof (SharedHeader, plugin_path) == 52, "plugin_path");
static_assert (offsetof (SharedHeader, error) == 1076, "error");
static_assert (sizeof (SharedHeader) == 1204, "SharedHeader");
static_assert (sizeof (SharedState) == 527796, "SharedState");

} /* namespace vst2_bridge */
} /* namespace airdour */
