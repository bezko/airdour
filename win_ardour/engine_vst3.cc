/*
 * Copyright (C) 2026 Airdour contributors
 *
 * VST3 host seam extracted from engine_host (Phase 0).
 * Core scan/insert/params remain in engine_host.cc; instrument attach lives
 * in engine_midi.cc. This TU owns catalog-limit smoke helpers and documents
 * the module boundary for future moves of refresh_vst3_* / insert_vst3_*.
 */

#include "engine_host.h"

#include <cstdio>

#ifdef AIRDOUR_WIN_SHELL_WITH_ENGINE

/* Intentionally thin: existing VST3 EngineHost methods stay in engine_host.cc
 * until a follow-up PR relocates them behind engine_host_priv.h helpers.
 * Raising kMaxVst3Catalog/Params is validated by existing --smoke-vst3-* plus
 * smoke_vst3_instrument (engine_midi.cc).
 */

static void
airdour_vst3_module_anchor ()
{
	/* Ensure this TU is not discarded by the linker when only referenced
	 * indirectly via other EngineHost methods in sibling modules. */
}

#else

static void
airdour_vst3_module_anchor ()
{
}

#endif

/* Force a live reference from a TU-local static so MSVC keeps the .obj. */
namespace {
struct Vst3ModuleAnchor {
	Vst3ModuleAnchor () { airdour_vst3_module_anchor (); }
} g_vst3_module_anchor;
}
