/*
 * Copyright (C) 2026 Airdour contributors
 *
 * x64 host-side client for ardour-vst2-bridge-x86.exe (Phase 4).
 */
#pragma once

#include <string>

namespace airdour {
namespace vst2_bridge {

/** Absolute path to ardour-vst2-bridge-x86.exe beside the shell, or empty. */
std::string bridge_exe_path ();

/** True when PE Machine == IMAGE_FILE_MACHINE_I386 (0x014c). */
bool dll_is_i386 (const std::string& path);

/** Launch (or reuse) a bridge process for this plugin path. */
bool ensure_bridge_running (const std::string& plugin_dll_path);

/** Close all bridge processes started by this client. */
void shutdown_all ();

} /* namespace vst2_bridge */
} /* namespace airdour */
