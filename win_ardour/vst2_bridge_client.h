/*
 * Copyright (C) 2026 Airdour contributors
 *
 * x64 host-side client for ardour-vst2-bridge-x86.exe (Phase 4).
 */
#pragma once

#include <cstdint>
#include <string>

namespace airdour {
namespace vst2_bridge {

struct MidiEventWire;

/** Absolute path to ardour-vst2-bridge-x86.exe beside the shell, or empty. */
std::string bridge_exe_path ();

/** True when PE Machine == IMAGE_FILE_MACHINE_I386 (0x014c). */
bool dll_is_i386 (const std::string& path);

/** Launch (or reuse) a bridge process for this plugin path. */
bool ensure_bridge_running (const std::string& plugin_dll_path);

/**
 * Ask the first live bridge to process a silent block (validates IPC Process op).
 * frames clamped to protocol kMaxBlock.
 */
bool process_silence_block (uint32_t frames, float sample_rate);

/** Configure rate/block and process planar audio (+ optional MIDI) for a bridged path. */
bool process_audio (const std::string& plugin_dll_path,
		    float* const* inputs, float* const* outputs,
		    uint32_t n_inputs, uint32_t n_outputs,
		    uint32_t frames, float sample_rate,
		    uint32_t wait_ms = 50,
		    const MidiEventWire* midi = nullptr,
		    uint32_t midi_count = 0);

bool set_sample_rate (const std::string& plugin_dll_path, float sample_rate);
bool set_block_size (const std::string& plugin_dll_path, uint32_t frames);
bool get_io_counts (const std::string& plugin_dll_path, uint32_t* n_in, uint32_t* n_out,
		    uint32_t* n_params);
bool get_parameter (const std::string& plugin_dll_path, uint32_t index, float* out_norm);
bool set_parameter (const std::string& plugin_dll_path, uint32_t index, float norm);

/** Close bridge for one plugin path (or all if path empty). */
void shutdown_plugin (const std::string& plugin_dll_path);

/** Close all bridge processes started by this client. */
void shutdown_all ();

} /* namespace vst2_bridge */
} /* namespace airdour */
