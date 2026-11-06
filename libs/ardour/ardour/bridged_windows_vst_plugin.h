/*
 * Copyright (C) 2026 Airdour contributors
 *
 * Placeholder Plugin façade for out-of-process 32-bit Windows VST2 hosting.
 * The win_ardour shell currently drives the bridge via
 * win_ardour/vst2_bridge_client.* + ardour-vst2-bridge-x86.exe. A future
 * BridgedWindowsVSTPlugin : public WindowsVSTPlugin (or Plugin) will wrap
 * the SharedState processReplacing path so PluginInsert can own bridged
 * instances like in-process VST2-64.
 *
 * See win_ardour/vst2_bridge_protocol.h for the IPC wire format.
 */
#pragma once

/* Header-only documentation anchor — implementation lands with full
 * libardour PluginInsert integration once the bridge audio path is
 * proven under --smoke-vst2-bridge with a fixture DLL. */
