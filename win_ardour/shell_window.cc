/*
 * Copyright (C) 2026 Airdour contributors
 */

#include "shell_window.h"
#include "cairo_wave_blit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kClassName[] = L"AirdourWinShell";
constexpr wchar_t kWindowTitle[] = L"Airdour";

/* Legacy brush palette — mirrors ui_theme tokens so unconverted panels
 * inherit the refined-dark-studio colors. */
const D2D1_COLOR_F kBgTop      = ui::rgb (0x0a0d12);
const D2D1_COLOR_F kBgBottom   = ui::rgb (0x121821);
const D2D1_COLOR_F kBrand      = ui::rgb (0xedf3f8);
const D2D1_COLOR_F kBody       = ui::rgb (0xa7b8c5);
const D2D1_COLOR_F kAccent     = ui::rgb (0x25d9bc);
const D2D1_COLOR_F kPaneFill   = ui::rgb (0x11161d, 0.94f);
const D2D1_COLOR_F kPaneEdge   = ui::rgb (0x27323f);
const D2D1_COLOR_F kPeak       = ui::rgb (0x35e6c8);
const D2D1_COLOR_F kRms        = ui::rgb (0x1c7a6e, 0.88f);
const D2D1_COLOR_F kPlayhead   = ui::rgb (0xffd24a);
const D2D1_COLOR_F kSelection  = ui::rgb (0x3fc3e8);
const D2D1_COLOR_F kLoop       = ui::rgb (0xf0a93b);
const D2D1_COLOR_F kPunch      = ui::rgb (0xe8574a);
/* Ruler / lane wash opacities (smoke-punch-loop-tint asserts these). */
constexpr float kLoopRulerTintOp   = 0.52f; /* was 0.35 */
constexpr float kPunchRulerTintOn  = 0.72f; /* was 0.55 */
constexpr float kPunchRulerTintOff = 0.42f; /* was 0.28 */
constexpr float kLoopLaneTintOp    = 0.18f; /* play-loop on */
constexpr float kLoopLaneTintIdle  = 0.10f; /* range on, play off */
constexpr float kLoopInactiveHatchOp = 0.30f; /* soft hatch when idle */
constexpr float kLoopInactiveHatchPx = 6.f;   /* stripe pitch @ dpi=1 */
constexpr float kPunchLaneTintOn   = 0.34f; /* was 0.22 */
constexpr float kPunchLaneTintOff  = 0.12f; /* was 0.16 — idle + hatch */
constexpr float kPunchInactiveHatchOp = 0.32f; /* soft hatch when disarmed */
constexpr float kPunchInactiveHatchPx = 6.f;
const D2D1_COLOR_F kMarker     = ui::rgb (0xd6e36b);
const D2D1_COLOR_F kMidi       = ui::rgb (0x46c2e8); /* cyan, not purple */
const D2D1_COLOR_F kMeterHot   = ui::rgb (0xf26b3a);
/* Piano-roll outside-ranges cue: dim unselected notes inside loop∪punch. */
constexpr float kMidiNoteSelOp           = 0.95f;
constexpr float kMidiNoteIdleOp          = 0.70f;
constexpr float kMidiOutsideRangesInWashOp = 0.28f; /* in-union, unselected, after outside select */

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

UINT
query_dpi_for_hwnd (HWND hwnd)
{
	HMODULE user32 = GetModuleHandleW (L"user32.dll");
	if (user32) {
		typedef UINT (WINAPI *GetDpiForWindowFn) (HWND);
		auto fn = (GetDpiForWindowFn)GetProcAddress (user32, "GetDpiForWindow");
		if (fn && hwnd) {
			UINT d = fn (hwnd);
			if (d > 0) {
				return d;
			}
		}
	}
	HDC dc = GetDC (nullptr);
	UINT d = dc ? (UINT)GetDeviceCaps (dc, LOGPIXELSX) : 96;
	if (dc) {
		ReleaseDC (nullptr, dc);
	}
	return d ? d : 96;
}

void
enable_dpi_awareness ()
{
	HMODULE user32 = LoadLibraryW (L"user32.dll");
	if (user32) {
		typedef BOOL (WINAPI *SetCtxFn) (HANDLE);
		auto set_ctx = (SetCtxFn)GetProcAddress (user32, "SetProcessDpiAwarenessContext");
		if (set_ctx) {
			set_ctx ((HANDLE)(intptr_t)-4);
			FreeLibrary (user32);
			return;
		}
		FreeLibrary (user32);
	}
	HMODULE shcore = LoadLibraryW (L"Shcore.dll");
	if (shcore) {
		typedef HRESULT (WINAPI *SetAwareFn) (int);
		auto set_aware = (SetAwareFn)GetProcAddress (shcore, "SetProcessDpiAwareness");
		if (set_aware) {
			set_aware (2);
		}
		FreeLibrary (shcore);
	}
}

bool run_text_prompt (HWND parent, const wchar_t* title, wchar_t* name, size_t name_chars);
bool pick_folder (HWND owner, const wchar_t* title, std::wstring* out);
std::wstring utf8_to_wstring (const std::string& s);
std::string wstring_to_utf8 (const std::wstring& s);

} // namespace

ShellWindow::ShellWindow ()
	: _hwnd (nullptr)
	, _width (1180)
	, _height (760)
	, _dpi (96)
	, _dpi_scale (1.f)
	, _mouse_x (0.f)
	, _mouse_y (0.f)
	, _mouse_tracking (false)
	, _running (false)
	, _start_tick (0)
	, _timeout_secs (0)
	, _drag (DragMode::None)
	, _drag_index (-1)
	, _focus_audio_track (0)
	, _pan_last_x (0.f)
	, _pan_last_y (0.f)
	, _pan_down_x (0.f)
	, _pan_down_y (0.f)
	, _region_move_grab_offset_sec (0.f)
	, _midi_draw_start_sec (0.f)
	, _midi_draw_pitch (60)
	, _midi_marquee_t0 (0.f)
	, _midi_marquee_p0 (60)
	, _midi_vel_drag (100)
	, _midi_trim_start_sec (0.f)
	, _midi_trim_end_sec (0.f)
	, _midi_move_origin_sec (0.f)
	, _midi_move_origin_pitch (60)
	, _midi_move_preview_sec (0.f)
	, _midi_move_preview_pitch (60)
	, _step_piano_held_count (0)
	, _transport_playing (false)
	, _playhead_sec (0.f)
	, _last_tick_ms (0)
	, _smoke_record (SmokeRecordPhase::Off)
	, _smoke_record_start_ms (0)
	, _smoke_failed (false)
	, _help_visible (false)
	, _help_scroll (0)
	, _help_scroll_max (0)
	, _prefs_visible (false)
	, _splash_shown_ms (0)
	, _brand_bar ()
	, _transport_bounds ()
	, _editor_pane ()
	, _editor_ruler ()
	, _midi_lane ()
	, _auto_lane_bounds ()
	, _midi_cc_lane_bounds ()
	, _editor_scroll_y (0.f)
	, _editor_scroll_max (0.f)
	, _editor_lanes_top (0.f)
	, _editor_lanes_bot (0.f)
	, _editor_scroll_focus (-1)
	, _editor_scrollbar ()
	, _auto_lane_track (0)
	, _mixer_pane ()
	, _mixer_strips_clip ()
	, _mixer_scroll_x (0.f)
	, _mixer_scroll_max (0.f)
	, _mixer_force_min_w (0.f)
	, _status_bar ()
	, _status_vst3_bypass_btn ()
	, _status_vst3_param_reset_btn ()
	, _status_vst3_unload_btn ()
	, _status_snap_btn ()
	, _d2d_factory (nullptr)
	, _dwrite_factory (nullptr)
	, _rt (nullptr)
	, _brush_brand (nullptr)
	, _brush_body (nullptr)
	, _brush_accent (nullptr)
	, _brush_pane (nullptr)
	, _brush_pane_edge (nullptr)
	, _brush_peak (nullptr)
	, _brush_rms (nullptr)
	, _brush_playhead (nullptr)
	, _brush_selection (nullptr)
	, _brush_loop (nullptr)
	, _brush_punch (nullptr)
	, _brush_marker (nullptr)
	, _brush_midi (nullptr)
	, _brush_meter_hot (nullptr)
	, _brush_bg (nullptr)
	, _fmt_brand (nullptr)
	, _fmt_body (nullptr)
	, _fmt_meta (nullptr)
	, _fmt_pane (nullptr)
	, _pulse (0.f)
	, _engine_snap ()
	, _menu_bar_bounds ()
	, _auto_last_write_sec (-1.f)
	, _midi_cc_last_write_sec (-1.f)
	, _auto_lane_kind (0)
	, _midi_cc_number (7)
{
	enable_dpi_awareness ();
	_engine_snap = _engine.snapshot ();

	for (int i = 0; i < kTracksMax; ++i) {
		wchar_t lab[24];
		_snwprintf_s (lab, _TRUNCATE, L"Audio %d", i + 1);
		wcsncpy_s (_lanes[i].label, lab, _TRUNCATE);
		_lanes[i].bounds = D2D1::RectF ();
		_mixers[i].set_label (lab);
		_mixers[i].set_gain (0.85f - 0.04f * (float)i);
		_mixers[i].set_midi_strip (false);
		_group_pick[i] = false;
	}
	for (int i = 0; i < kMixerStripsMax; ++i) {
		if (i >= kTracksMax) {
			_mixers[i].set_label (L"MIDI 1");
			_mixers[i].set_gain (0.85f);
			_mixers[i].set_midi_strip (true);
		}
	}
	/* Default MIDI strip after 3 audio (session default). */
	_mixers[3].set_label (L"MIDI 1");
	_mixers[3].set_gain (0.85f);
	_mixers[3].set_midi_strip (true);
	for (int i = 0; i < 128; ++i) {
		_step_piano_held[i] = false;
	}
}

ShellWindow::~ShellWindow ()
{
	_engine.stop ();
	discard_device_resources ();
	if (_fmt_pane) { _fmt_pane->Release (); _fmt_pane = nullptr; }
	if (_fmt_meta) { _fmt_meta->Release (); _fmt_meta = nullptr; }
	if (_fmt_body) { _fmt_body->Release (); _fmt_body = nullptr; }
	if (_fmt_brand) { _fmt_brand->Release (); _fmt_brand = nullptr; }
	if (_dwrite_factory) { _dwrite_factory->Release (); _dwrite_factory = nullptr; }
	if (_d2d_factory) { _d2d_factory->Release (); _d2d_factory = nullptr; }
	if (_hwnd) { DestroyWindow (_hwnd); _hwnd = nullptr; }
}

void
ShellWindow::run_smoke_check (bool ok, const char* name, std::function<bool()> fn, bool select_lanes)
{
	bool requested = has_smoke (name);
	if (!requested) {
		return;
	}
	if (ok && _engine.snapshot ().session_ok) {
		if (!fn ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			_playhead_sec = _engine_snap.transport_sec;
			sync_transport_clock_from_engine ();
			if (select_lanes) {
				apply_region_selection_to_lanes ();
			}
		}
		std::fflush (stderr);
	} else {
		std::fprintf (stderr, "airdour_win_shell: smoke-%s FAILED (no session)\n", name);
		_smoke_failed = true;
		std::fflush (stderr);
	}
}

bool
ShellWindow::attach_engine (const std::string& backend_name,
                            unsigned smoke_timeout_secs,
                            bool smoke_import,
                            bool smoke_record,
                            bool smoke_save_load,
                            bool smoke_export,
                            bool smoke_undo,
                            bool smoke_loop,
                            bool smoke_midi,
                            bool smoke_midi_edit,
                            bool smoke_plugin,
                            bool smoke_perf,
                            const std::string& open_session_path,
                            unsigned period_frames,
                            bool smoke_vst3,
                            bool smoke_mute_solo,
                            bool smoke_pan,
                            bool smoke_send,
                            bool smoke_automation,
                            bool smoke_edit,
                            bool smoke_snap,
                            bool smoke_punch,
                            bool smoke_tempo,
                            bool smoke_click,
                            bool smoke_tracks,
                            bool smoke_stereo_track,
                            bool smoke_rename,
                            bool smoke_group,
                            bool smoke_monitor,
                            bool smoke_locate,
                            bool smoke_zoom,
                            bool smoke_history,
                            bool smoke_prefs,
                            bool smoke_recent,
                            bool smoke_recovery,
                            bool smoke_stretch,
                            bool smoke_pitch,
                            bool smoke_reverse,
                            bool smoke_normalize,
                            bool smoke_strip_silence,
                            bool smoke_consolidate,
                            bool smoke_freeze,
                            bool smoke_comp,
                            bool smoke_comp_sc_hotkey,
                            bool smoke_eq,
                            bool smoke_delay,
                            bool smoke_limiter,
                            bool smoke_gate,
                            bool smoke_spectrum,
                            bool smoke_correlation,
                            bool smoke_loudness,
                            bool smoke_midi_quantize,
                            bool smoke_midi_transpose,
                            bool smoke_midi_velocity,
                            bool smoke_midi_length,
                            bool smoke_midi_clipboard,
                            bool smoke_midi_humanize,
                            bool smoke_midi_step,
                            bool smoke_midi_chord,
                            bool smoke_midi_arp,
                            bool smoke_midi_scale,
                            bool smoke_midi_cc,
                            bool smoke_midi_bend,
                            bool smoke_midi_pressure,
                            bool smoke_locate_selection,
                            bool smoke_pan_automation,
                            bool smoke_auto_lane_unify,
                            bool smoke_auto_state,
                            bool smoke_pan_auto_state,
                            bool smoke_send_auto_state,
                            bool smoke_auto_state_unify)
{
	if (!_engine.is_linked ()) {
		_engine.prepare (backend_name);
		refresh_engine_snap ();
		return false;
	}
	/* --timeout smokes: autosave off (unless --smoke-recovery). */
	_engine.set_autosave_enabled (smoke_recovery || smoke_timeout_secs == 0);
	if (period_frames > 0) {
		_engine.set_period_frames (period_frames);
		std::fprintf (stderr, "airdour_win_shell: --period %u\n", period_frames);
		std::fflush (stderr);
	}
	bool ok = _engine.start (backend_name);
	/* Unattended --timeout: keep Session + PeakMeter (not stubs) but skip
	 * auto-roll — rolling under Debug has intermittent AVs on Dummy/WASAPI
	 * that prevent ExitProcess(0). Interactive runs still auto-play.
	 * --smoke-record / --smoke-perf drive their own short roll. */
	const bool autoplay = ok && (smoke_timeout_secs == 0) && !smoke_record && !smoke_perf;

	if (ok && !open_session_path.empty ()) {
		if (_engine.open_session (open_session_path)) {
			std::fprintf (stderr, "airdour_win_shell: opened --session path\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: --session open failed\n");
			ok = false;
		}
		std::fflush (stderr);
	}

	if (ok && smoke_import && engine_session_ok ()) {
		if (_engine.import_test_tone (0, 220.f, 2.f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-import ok (track 0)\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-import failed\n");
		}
		std::fflush (stderr);
	}

	if (ok && smoke_save_load && engine_session_ok ()) {
		refresh_engine_snap ();
		if (!_engine_snap.track_has_regions[0]) {
			_engine.import_test_tone (0, 330.f, 1.5f);
		}
		const bool reloaded = _engine.reload_session ();
		refresh_engine_snap ();
		if (reloaded && _engine_snap.track_has_regions[0]) {
			std::fprintf (stderr, "airdour_win_shell: smoke-save-load ok (regions restored)\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-save-load FAILED (saved=%d regions0=%d)\n",
			              _engine_snap.session_saved ? 1 : 0,
			              _engine_snap.track_has_regions[0] ? 1 : 0);
		}
		std::fflush (stderr);
	}

	if (ok && smoke_export && engine_session_ok ()) {
		std::string exported;
		if (_engine.smoke_export_wav (&exported)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-export ok → %s\n", exported.c_str ());
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-export FAILED\n");
		}
		std::fflush (stderr);
	}

	if (ok && smoke_undo && engine_session_ok ()) {
		if (_engine.smoke_undo ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-undo ok\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-undo FAILED\n");
		}
		std::fflush (stderr);
	}

	if (ok && smoke_loop && engine_session_ok ()) {
		if (_engine.smoke_loop_markers ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-loop ok\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-loop FAILED\n");
		}
		std::fflush (stderr);
	}

	if (ok && smoke_midi_edit && engine_session_ok ()) {
		if (_engine.smoke_midi_edit ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-edit ok\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi-edit FAILED\n");
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (ok && smoke_midi_quantize && engine_session_ok ()) {
		if (!_engine.smoke_midi_quantize ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_quantize) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-quantize FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_transpose && engine_session_ok ()) {
		if (!_engine.smoke_midi_transpose ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_transpose) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-transpose FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_velocity && engine_session_ok ()) {
		if (!_engine.smoke_midi_velocity ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_velocity) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-velocity FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_length && engine_session_ok ()) {
		if (!_engine.smoke_midi_length ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_length) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-length FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_clipboard && engine_session_ok ()) {
		if (!_engine.smoke_midi_clipboard ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_clipboard) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-clipboard FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_humanize && engine_session_ok ()) {
		if (!_engine.smoke_midi_humanize ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_humanize) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-humanize FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_step && engine_session_ok ()) {
		if (!_engine.smoke_midi_step ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_step) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-step FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_chord && engine_session_ok ()) {
		if (!_engine.smoke_midi_chord ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_chord) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-chord FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_arp && engine_session_ok ()) {
		if (!_engine.smoke_midi_arp ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_arp) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-arp FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_scale && engine_session_ok ()) {
		if (!_engine.smoke_midi_scale ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_midi_scale) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-scale FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_cc && engine_session_ok ()) {
		if (!_engine.smoke_midi_cc ()) {
			_smoke_failed = true;
		} else {
			refresh_midi_cc_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_midi_cc) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_bend && engine_session_ok ()) {
		if (!_engine.smoke_midi_bend ()) {
			_smoke_failed = true;
		} else {
			_midi_cc_number = -1;
			refresh_midi_cc_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_midi_bend) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-bend FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi_pressure && engine_session_ok ()) {
		if (!_engine.smoke_midi_pressure ()) {
			_smoke_failed = true;
		} else {
			_midi_cc_number = -2;
			refresh_midi_cc_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_midi_pressure) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-pressure FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && has_smoke ("master-fx") && engine_session_ok ()) {
		if (!_engine.smoke_master_fx ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (has_smoke ("master-fx")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-master-fx FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && smoke_midi && engine_session_ok ()) {
		if (_engine.smoke_midi ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi ok\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-midi FAILED\n");
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (ok && has_smoke ("vca") && engine_session_ok ()) {
		if (!_engine.smoke_vca ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (has_smoke ("vca")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vca FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && has_smoke ("group-vca") && engine_session_ok ()) {
		if (!_engine.smoke_group_vca ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (has_smoke ("group-vca")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group-vca FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && has_smoke ("vca-status") && engine_session_ok ()) {
		if (!_engine.smoke_vca_status ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (has_smoke ("vca-status")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vca-status FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	} else if (ok && engine_session_ok ()) {
		/* Always scaffold a MIDI track + empty region so the piano-roll is live. */
		_engine.ensure_midi_track ();
		_engine.ensure_midi_region ();
	}

	if (ok && has_smoke ("pan-reset-all") && engine_session_ok ()) {
		if (!_engine.smoke_pan_reset_all ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_pan_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("pan-reset-all")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pan-reset-all FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("track-color") && engine_session_ok ()) {
		if (!_engine.smoke_track_color ()) {
			_smoke_failed = true;
		} else {
			sync_track_labels_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("track-color")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-track-color FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("send-reset-all") && engine_session_ok ()) {
		if (!_engine.smoke_send_reset_all ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_send_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("send-reset-all")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-send-reset-all FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("fader-reset-all") && engine_session_ok ()) {
		if (!_engine.smoke_fader_reset_all ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_gains_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("fader-reset-all")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-fader-reset-all FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("disarm-all") && engine_session_ok ()) {
		if (!_engine.smoke_disarm_all ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_arm_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("disarm-all")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-disarm-all FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("clear-meter-clips") && engine_session_ok ()) {
		if (!_engine.smoke_clear_meter_clips ()) {
			_smoke_failed = true;
		} else {
			for (int i = 0; i < kMixerStripsMax; ++i) {
				_mixers[i].clear_meter_clip ();
			}
		}
		std::fflush (stderr);
	} else if (has_smoke ("clear-meter-clips")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-clear-meter-clips FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("fx-bypass-all") && engine_session_ok ()) {
		if (!_engine.smoke_fx_bypass_all ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("fx-bypass-all")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-fx-bypass-all FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("fx-activate-all") && engine_session_ok ()) {
		if (!_engine.smoke_fx_activate_all ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("fx-activate-all")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-fx-activate-all FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_plugin && engine_session_ok ()) {
		if (_engine.smoke_plugin_insert ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-plugin ok\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: smoke-plugin FAILED\n");
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_plugin) {
		std::fprintf (stderr, "airdour_win_shell: smoke-plugin FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("fx-param-reset") && engine_session_ok ()) {
		if (!_engine.smoke_fx_param_reset ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("fx-param-reset")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-fx-param-reset FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_comp && engine_session_ok ()) {
		if (!_engine.smoke_comp ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_comp) {
		std::fprintf (stderr, "airdour_win_shell: smoke-comp FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_comp_sc_hotkey && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_comp_sc_hotkey ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_comp_sc_hotkey) {
		std::fprintf (stderr, "airdour_win_shell: smoke-comp-sc-hotkey FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}


	if (ok && smoke_eq && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_eq ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_eq) {
		std::fprintf (stderr, "airdour_win_shell: smoke-eq FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_delay && engine_session_ok ()) {
		if (!_engine.smoke_delay ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_delay) {
		std::fprintf (stderr, "airdour_win_shell: smoke-delay FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_limiter && engine_session_ok ()) {
		if (!_engine.smoke_limiter ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_limiter) {
		std::fprintf (stderr, "airdour_win_shell: smoke-limiter FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_gate && engine_session_ok ()) {
		if (!_engine.smoke_gate ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_fx_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_gate) {
		std::fprintf (stderr, "airdour_win_shell: smoke-gate FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_spectrum && engine_session_ok ()) {
		if (!_engine.smoke_spectrum ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_spectrum) {
		std::fprintf (stderr, "airdour_win_shell: smoke-spectrum FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_correlation && engine_session_ok ()) {
		if (!_engine.smoke_correlation ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_correlation) {
		std::fprintf (stderr, "airdour_win_shell: smoke-correlation FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_loudness && engine_session_ok ()) {
		if (!_engine.smoke_loudness ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_loudness) {
		std::fprintf (stderr, "airdour_win_shell: smoke-loudness FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_vst3 && engine_session_ok ()) {
		if (!_engine.smoke_vst3 ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_vst3) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vst3 FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_mute_solo && engine_session_ok ()) {
		if (!_engine.smoke_mute_solo ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_mute_solo_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_mute_solo) {
		std::fprintf (stderr, "airdour_win_shell: smoke-mute-solo FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("solo-isolate") && engine_session_ok ()) {
		if (!_engine.smoke_solo_isolate ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_solo_isolate_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("solo-isolate")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-solo-isolate FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("meter-point") && engine_session_ok ()) {
		if (!_engine.smoke_meter_point ()) {
			_smoke_failed = true;
		} else {
			const int na = n_audio_lanes ();
			for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
				_mixers[i].set_meter_point (_engine.snapshot ().track_meter_point[i]);
			}
		}
		std::fflush (stderr);
	} else if (has_smoke ("meter-point")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-meter-point FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("meter-type") && engine_session_ok ()) {
		if (!_engine.smoke_meter_type ()) {
			_smoke_failed = true;
		} else {
			const int na = n_audio_lanes ();
			for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
				_mixers[i].set_meter_type (_engine.snapshot ().track_meter_type[i]);
			}
		}
		std::fflush (stderr);
	} else if (has_smoke ("meter-type")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-meter-type FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("input-monitor") && engine_session_ok ()) {
		if (!_engine.smoke_input_monitor ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_monitor_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("input-monitor")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-input-monitor FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("listen-mode") && engine_session_ok ()) {
		if (!_engine.smoke_listen_mode ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_listen_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("listen-mode")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-listen-mode FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("mon-peer-badge") && engine_session_ok ()) {
		if (!_engine.smoke_mon_peer_badge ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_monitor_from_engine ();
			sync_mixer_group_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("mon-peer-badge")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-mon-peer-badge FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("phase-invert") && engine_session_ok ()) {
		if (!_engine.smoke_phase_invert ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_phase_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("phase-invert")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-phase-invert FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("track-comment") && engine_session_ok ()) {
		if (!_engine.smoke_track_comment ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_comment_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("track-comment")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-track-comment FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("solo-safe") && engine_session_ok ()) {
		if (!_engine.smoke_solo_safe ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_solo_safe_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("solo-safe")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-solo-safe FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("arm-all") && engine_session_ok ()) {
		if (!_engine.smoke_arm_all ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_arm_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("arm-all")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-arm-all FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("record-safe") && engine_session_ok ()) {
		if (!_engine.smoke_record_safe ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_arm_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("record-safe")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-record-safe FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("mute-points") && engine_session_ok ()) {
		if (!_engine.smoke_mute_points ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_mute_solo_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("mute-points")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-mute-points FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("clear-solos") && engine_session_ok ()) {
		if (!_engine.smoke_clear_solos ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_mute_solo_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("clear-solos")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-clear-solos FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("clear-mutes") && engine_session_ok ()) {
		if (!_engine.smoke_clear_mutes ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_mute_solo_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("clear-mutes")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-clear-mutes FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("exclusive-solo") && engine_session_ok ()) {
		if (!_engine.smoke_exclusive_solo ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_mute_solo_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("exclusive-solo")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-exclusive-solo FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_pan && engine_session_ok ()) {
		if (!_engine.smoke_pan ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_pan_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_pan) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pan FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("pan-width") && engine_session_ok ()) {
		if (!_engine.smoke_pan_width ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_pan_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("pan-width")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pan-width FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("mixer-reset") && engine_session_ok ()) {
		if (!_engine.smoke_mixer_reset ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_gains_from_engine ();
			sync_mixer_pan_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("mixer-reset")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-mixer-reset FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("strip-trim") && engine_session_ok ()) {
		if (!_engine.smoke_strip_trim ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_trim_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("strip-trim")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-strip-trim FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_send && engine_session_ok ()) {
		if (!_engine.smoke_send ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_send_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_send) {
		std::fprintf (stderr, "airdour_win_shell: smoke-send FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("strip-mixer-reset") && engine_session_ok ()) {
		if (!_engine.smoke_strip_mixer_reset ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_gains_from_engine ();
			sync_mixer_pan_from_engine ();
			sync_mixer_send_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("strip-mixer-reset")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-strip-mixer-reset FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("aux-return") && engine_session_ok ()) {
		if (!_engine.smoke_aux_return ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_monitor_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("aux-return")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-aux-return FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("send-pan") && engine_session_ok ()) {
		if (!_engine.smoke_send_pan ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_send_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("send-pan")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-send-pan FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("send-reset") && engine_session_ok ()) {
		if (!_engine.smoke_send_reset ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_send_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("send-reset")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-send-reset FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("send-mute") && engine_session_ok ()) {
		if (!_engine.smoke_send_mute ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_send_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("send-mute")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-send-mute FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_automation && engine_session_ok ()) {
		if (!_engine.smoke_automation ()) {
			_smoke_failed = true;
		} else {
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_automation) {
		std::fprintf (stderr, "airdour_win_shell: smoke-automation FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_pan_automation && engine_session_ok ()) {
		_auto_lane_kind = 1;
		if (!_engine.smoke_pan_automation ()) {
			_smoke_failed = true;
		} else {
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_pan_automation) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pan-automation FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("auto-lane-focus") && engine_session_ok ()) {
		/* Focus Audio 2 so the D2D lane label/curve target matches smoke writes. */
		_focus_audio_track = 1;
		if (!_engine.smoke_auto_lane_focus ()) {
			_smoke_failed = true;
		} else {
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("auto-lane-focus")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-auto-lane-focus FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_auto_lane_unify && engine_session_ok ()) {
		_auto_lane_kind = 0;
		if (!_engine.smoke_auto_lane_unify ()) {
			_smoke_failed = true;
		} else {
			_auto_lane_kind = 2; /* leave on Send after unify */
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_auto_lane_unify) {
		std::fprintf (stderr, "airdour_win_shell: smoke-auto-lane-unify FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_auto_state && engine_session_ok ()) {
		if (!_engine.smoke_auto_state ()) {
			_smoke_failed = true;
		} else {
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_auto_state) {
		std::fprintf (stderr, "airdour_win_shell: smoke-auto-state FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_pan_auto_state && engine_session_ok ()) {
		if (!_engine.smoke_pan_auto_state ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_pan_from_engine ();
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_pan_auto_state) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pan-auto-state FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_send_auto_state && engine_session_ok ()) {
		if (!_engine.smoke_send_auto_state ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_send_from_engine ();
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_send_auto_state) {
		std::fprintf (stderr, "airdour_win_shell: smoke-send-auto-state FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_auto_state_unify && engine_session_ok ()) {
		_auto_lane_kind = 0;
		if (!_engine.smoke_auto_state_unify ()) {
			_smoke_failed = true;
		} else {
			refresh_automation_lane ();
		}
		std::fflush (stderr);
	} else if (smoke_auto_state_unify) {
		std::fprintf (stderr, "airdour_win_shell: smoke-auto-state-unify FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_edit && engine_session_ok ()) {
		if (!_engine.smoke_edit ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_edit) {
		std::fprintf (stderr, "airdour_win_shell: smoke-edit FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_stretch && engine_session_ok ()) {
		if (!_engine.smoke_stretch ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_stretch) {
		std::fprintf (stderr, "airdour_win_shell: smoke-stretch FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_pitch && engine_session_ok ()) {
		if (!_engine.smoke_pitch ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_pitch) {
		std::fprintf (stderr, "airdour_win_shell: smoke-pitch FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_reverse && engine_session_ok ()) {
		if (!_engine.smoke_reverse ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_reverse) {
		std::fprintf (stderr, "airdour_win_shell: smoke-reverse FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_normalize && engine_session_ok ()) {
		if (!_engine.smoke_normalize ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_normalize) {
		std::fprintf (stderr, "airdour_win_shell: smoke-normalize FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_strip_silence && engine_session_ok ()) {
		if (!_engine.smoke_strip_silence ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_strip_silence) {
		std::fprintf (stderr, "airdour_win_shell: smoke-strip-silence FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_consolidate && engine_session_ok ()) {
		if (!_engine.smoke_consolidate ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_consolidate) {
		std::fprintf (stderr, "airdour_win_shell: smoke-consolidate FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_freeze && engine_session_ok ()) {
		if (!_engine.smoke_freeze ()) {
			_smoke_failed = true;
		} else {
			apply_region_selection_to_lanes ();
			sync_transport_freeze_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_freeze) {
		std::fprintf (stderr, "airdour_win_shell: smoke-freeze FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_snap && engine_session_ok ()) {
		if (!_engine.smoke_snap ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_snap) {
		std::fprintf (stderr, "airdour_win_shell: smoke-snap FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_punch && engine_session_ok ()) {
		if (!_engine.smoke_punch ()) {
			_smoke_failed = true;
		} else {
			sync_transport_punch_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_punch) {
		std::fprintf (stderr, "airdour_win_shell: smoke-punch FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_tempo && engine_session_ok ()) {
		if (!_engine.smoke_tempo ()) {
			_smoke_failed = true;
		} else {
			sync_transport_tempo_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_tempo) {
		std::fprintf (stderr, "airdour_win_shell: smoke-tempo FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_click && engine_session_ok ()) {
		if (!_engine.smoke_click ()) {
			_smoke_failed = true;
		} else {
			sync_transport_tempo_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_click) {
		std::fprintf (stderr, "airdour_win_shell: smoke-click FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("click-gain") && engine_session_ok ()) {
		if (!_engine.smoke_click_gain ()) {
			_smoke_failed = true;
		} else {
			sync_transport_tempo_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("click-gain")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-click-gain FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_tracks && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_tracks ()) {
			_smoke_failed = true;
		} else {
			sync_track_labels_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (smoke_tracks) {
		std::fprintf (stderr, "airdour_win_shell: smoke-tracks FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_stereo_track && engine_session_ok ()) {
		if (!_engine.smoke_stereo_track ()) {
			_smoke_failed = true;
		} else {
			sync_track_labels_from_engine ();
			sync_mixer_gains_from_engine ();
			sync_mixer_pan_from_engine ();
			sync_mixer_send_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (smoke_stereo_track) {
		std::fprintf (stderr, "airdour_win_shell: smoke-stereo-track FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_rename && engine_session_ok ()) {
		if (!_engine.smoke_rename ()) {
			_smoke_failed = true;
		} else {
			sync_track_labels_from_engine ();
			sync_mixer_gains_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (smoke_rename) {
		std::fprintf (stderr, "airdour_win_shell: smoke-rename FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_group && engine_session_ok ()) {
		if (!_engine.smoke_group ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_group_from_engine ();
			sync_mixer_gains_from_engine ();
			sync_mixer_mute_solo_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (smoke_group) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("narrow-mixer")) {
		if (!_engine.smoke_narrow_mixer ()) {
			_smoke_failed = true;
		} else {
			_engine_snap = _engine.snapshot ();
			layout_panes ();
		}
		std::fflush (stderr);
	}

	if (ok && has_smoke ("group-recenable") && engine_session_ok ()) {
		if (!_engine.smoke_group_recenable ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_group_from_engine ();
			sync_mixer_arm_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("group-recenable")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group-recenable FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("group-relative") && engine_session_ok ()) {
		if (!_engine.smoke_group_relative ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_group_from_engine ();
			sync_mixer_gains_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("group-relative")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group-relative FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("mixer-scroll") && engine_session_ok ()) {
		if (!smoke_mixer_scroll_layout ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (has_smoke ("mixer-scroll")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-mixer-scroll FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("group-color") && engine_session_ok ()) {
		if (!_engine.smoke_group_color ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_group_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("group-color")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group-color FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("group-monitoring") && engine_session_ok ()) {
		if (!_engine.smoke_group_monitoring ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_group_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("group-monitoring")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group-monitoring FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("group-route-active") && engine_session_ok ()) {
		if (!_engine.smoke_group_route_active ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_group_from_engine ();
			sync_mixer_mute_solo_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("group-route-active")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group-route-active FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("group-select") && engine_session_ok ()) {
		if (!_engine.smoke_group_select ()) {
			_smoke_failed = true;
		} else {
			sync_mixer_group_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("group-select")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-group-select FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_monitor && _engine.snapshot ().session_ok) {
		if (!_engine.smoke_monitor ()) {
			_smoke_failed = true;
		} else {
			sync_monitor_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (smoke_monitor) {
		std::fprintf (stderr, "airdour_win_shell: smoke-monitor FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("monitor-dim-level") && engine_session_ok ()) {
		if (!_engine.smoke_monitor_dim_level ()) {
			_smoke_failed = true;
		} else {
			sync_monitor_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("monitor-dim-level")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-monitor-dim-level FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("solo-boost") && engine_session_ok ()) {
		if (!_engine.smoke_solo_boost ()) {
			_smoke_failed = true;
		} else {
			sync_monitor_from_engine ();
		}
		std::fflush (stderr);
	} else if (has_smoke ("solo-boost")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-solo-boost FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_locate && engine_session_ok ()) {
		if (!_engine.smoke_locate ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			refresh_engine_snap ();
			_playhead_sec = _engine_snap.transport_sec;
			sync_transport_clock_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_locate) {
		std::fprintf (stderr, "airdour_win_shell: smoke-locate FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_locate_selection && engine_session_ok ()) {
		if (!_engine.smoke_locate_selection ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			refresh_engine_snap ();
			_playhead_sec = _engine_snap.transport_sec;
			sync_transport_clock_from_engine ();
			apply_region_selection_to_lanes ();
		}
		std::fflush (stderr);
	} else if (smoke_locate_selection) {
		std::fprintf (stderr, "airdour_win_shell: smoke-locate-selection FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}
	run_smoke_check (ok, "marker-navigate", [this]() { return _engine.smoke_marker_navigate (); });
	run_smoke_check (ok, "marker-rename", [this]() { return _engine.smoke_marker_rename (); });
	run_smoke_check (ok, "marker-name-ellipsis", [this]() { return smoke_marker_name_ellipsis (); });
	run_smoke_check (ok, "marker-lock", [this]() { return _engine.smoke_marker_lock (); });
	run_smoke_check (ok, "midi-marker-overlay", [this]() { return smoke_midi_marker_overlay (); });
	run_smoke_check (ok, "cd-marker", [this]() { return _engine.smoke_cd_marker (); });
	run_smoke_check (ok, "range-marker", [this]() { return _engine.smoke_range_marker (); });
	run_smoke_check (ok, "region-sync", [this]() { return _engine.smoke_region_sync (); });
	run_smoke_check (ok, "region-sync-draw", [this]() { return smoke_region_sync_draw (); }, true);
	run_smoke_check (ok, "scene-channel", [this]() { return _engine.smoke_scene_channel (); });
	run_smoke_check (ok, "cue-navigate", [this]() { return _engine.smoke_cue_navigate (); });
	run_smoke_check (ok, "section-rename", [this]() { return _engine.smoke_section_rename (); });
	run_smoke_check (ok, "punch-navigate", [this]() { return _engine.smoke_punch_navigate (); });
	run_smoke_check (ok, "session-extent-navigate", [this]() { return _engine.smoke_session_extent_navigate (); });
	run_smoke_check (ok, "loop-navigate", [this]() { return _engine.smoke_loop_navigate (); });
	run_smoke_check (ok, "loop-punch-edge-labels", [this]() {
		return smoke_loop_punch_edge_labels ();
	});
	run_smoke_check (ok, "loop-punch-edge-ticks", [this]() {
		return smoke_loop_punch_edge_ticks ();
	});
	run_smoke_check (ok, "punch-loop-tint", [this]() {
		return smoke_punch_loop_tint ();
	});
	run_smoke_check (ok, "loop-inactive-hatch", [this]() {
		return smoke_loop_inactive_hatch ();
	});
	run_smoke_check (ok, "punch-inactive-hatch", [this]() {
		return smoke_punch_inactive_hatch ();
	});
	run_smoke_check (ok, "playhead-nudge", [this]() { return _engine.smoke_playhead_nudge (); });
	run_smoke_check (ok, "zoom-punch", [this]() {
		if (!_engine.smoke_zoom_punch ()) return false;
		_engine.poll ();
		refresh_engine_snap ();
		sync_transport_punch_from_engine ();
		float before = _lanes[0].strip.view_span_sec ();
		do_zoom_to_punch ();
		float span = _lanes[0].strip.view_span_sec ();
		if (span < 1.8f || span > 2.8f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-zoom-punch FAILED (span=%.2f before=%.2f)\n", span, before);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-zoom-punch ok (span=%.2fs)\n", span);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "zoom-loop", [this]() {
		if (!_engine.smoke_zoom_loop ()) return false;
		_engine.poll ();
		refresh_engine_snap ();
		sync_transport_loop_from_engine ();
		apply_loop_to_lanes ();
		float before = _lanes[0].strip.view_span_sec ();
		do_zoom_to_loop ();
		float span = _lanes[0].strip.view_span_sec ();
		if (span < 1.8f || span > 2.8f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-zoom-loop FAILED (span=%.2f before=%.2f)\n", span, before);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-zoom-loop ok (span=%.2fs)\n", span);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "zoom-selection", [this]() {
		if (!_engine.smoke_zoom_selection ()) return false;
		_engine.poll ();
		refresh_engine_snap ();
		apply_region_selection_to_lanes ();
		float before = _lanes[0].strip.view_span_sec ();
		float want = _engine_snap.selection_end_sec - _engine_snap.selection_start_sec;
		do_zoom_to_selection ();
		float span = _lanes[0].strip.view_span_sec ();
		float expect = want * 1.12f;
		if (span < expect * 0.85f || span > expect * 1.25f + 0.3f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-zoom-selection FAILED (span=%.2f want≈%.2f before=%.2f)\n", span, expect, before);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-zoom-selection ok (span=%.2fs)\n", span);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "selection-marquee-tint", [this]() {
		return smoke_selection_marquee_tint ();
	});
	run_smoke_check (ok, "selection-mute-wash", [this]() {
		return smoke_selection_mute_wash ();
	});
	run_smoke_check (ok, "zoom-session", [this]() {
		if (!_engine.smoke_zoom_session ()) return false;
		_engine.poll ();
		refresh_engine_snap ();
		float before = _lanes[0].strip.view_span_sec ();
		float s0 = 0.f, e0 = 0.f;
		float want = 2.f;
		if (_engine.session_extent_sec (&s0, &e0) && e0 > s0) {
			want = e0 - s0;
		}
		do_zoom_to_session ();
		float span = _lanes[0].strip.view_span_sec ();
		float expect = want * 1.12f;
		if (span < expect * 0.85f || span > expect * 1.25f + 0.3f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-zoom-session FAILED (span=%.2f want≈%.2f before=%.2f)\n", span, expect, before);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-zoom-session ok (span=%.2fs)\n", span);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "zoom-regions", [this]() {
		if (!_engine.smoke_zoom_regions ()) return false;
		_engine.poll ();
		refresh_engine_snap ();
		float before = _lanes[0].strip.view_span_sec ();
		float s0 = 0.f, e0 = 0.f;
		float want = 2.f;
		if (_engine.regions_extent_sec (&s0, &e0) && e0 > s0) {
			want = e0 - s0;
		}
		do_zoom_to_regions ();
		float span = _lanes[0].strip.view_span_sec ();
		float expect = want * 1.12f;
		if (span < expect * 0.85f || span > expect * 1.25f + 0.3f) {
			std::fprintf (stderr, "airdour_win_shell: smoke-zoom-regions FAILED (span=%.2f want≈%.2f before=%.2f)\n", span, expect, before);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-zoom-regions ok (span=%.2fs)\n", span);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "center-playhead", [this]() {
		if (!_engine.smoke_center_playhead ()) return false;
		refresh_engine_snap ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		_lanes[0].strip.set_view_window (0.f, 2.f);
		apply_shared_view (0);
		float before_mid = _lanes[0].strip.view_start_sec () + 0.5f * _lanes[0].strip.view_span_sec ();
		do_center_on_playhead ();
		float span = _lanes[0].strip.view_span_sec ();
		float mid = _lanes[0].strip.view_start_sec () + 0.5f * span;
		float ph = _playhead_sec;
		if (std::fabs (mid - ph) > std::max (0.15f, span * 0.12f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-center-playhead FAILED (mid=%.3f ph=%.3f before_mid=%.3f)\n", mid, ph, before_mid);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-center-playhead ok (mid=%.3f ph=%.3f)\n", mid, ph);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "zoom-playhead", [this]() {
		if (!_engine.smoke_zoom_playhead ()) return false;
		refresh_engine_snap ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		_lanes[0].strip.set_view_window (0.f, 8.f);
		apply_shared_view (0);
		float before = _lanes[0].strip.view_span_sec ();
		do_zoom_around_playhead (1.15f);
		float span = _lanes[0].strip.view_span_sec ();
		float start = _lanes[0].strip.view_start_sec ();
		float mid = start + 0.5f * span;
		if (span >= before * 0.98f || span < before / 1.15f * 0.85f
		    || std::fabs (mid - _playhead_sec) > std::max (0.2f, span * 0.15f)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-zoom-playhead FAILED (span=%.3f before=%.3f mid=%.3f ph=%.3f)\n", span, before, mid, _playhead_sec);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-zoom-playhead ok (span=%.3f→%.3f mid=%.3f)\n", before, span, mid);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "scroll-playhead", [this]() {
		if (!_engine.smoke_scroll_playhead ()) return false;
		refresh_engine_snap ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		_lanes[0].strip.set_view_window (0.f, 2.f);
		apply_shared_view (0);
		do_scroll_playhead_into_view ();
		float start = _lanes[0].strip.view_start_sec ();
		float span = _lanes[0].strip.view_span_sec ();
		float end = start + span;
		float ph = _playhead_sec;
		if (ph < start || ph > end) {
			std::fprintf (stderr, "airdour_win_shell: smoke-scroll-playhead FAILED (ph=%.3f view=[%.3f..%.3f])\n", ph, start, end);
			return false;
		}
		std::fprintf (stderr, "airdour_win_shell: smoke-scroll-playhead ok (ph=%.3f view=[%.3f..%.3f])\n", ph, start, end);
		layout_panes ();
		return true;
	});
	run_smoke_check (ok, "region-mute-gain", [this]() { return _engine.smoke_region_mute_gain (); });
	run_smoke_check (ok, "region-lock", [this]() { return _engine.smoke_region_lock (); });
	run_smoke_check (ok, "fade-nudge", [this]() { return _engine.smoke_fade_nudge (); });
	run_smoke_check (ok, "region-duplicate", [this]() { return _engine.smoke_region_duplicate (); });
	run_smoke_check (ok, "region-nudge", [this]() { return _engine.smoke_region_nudge (); });
	run_smoke_check (ok, "region-layer", [this]() { return _engine.smoke_region_layer (); });
	run_smoke_check (ok, "region-opaque", [this]() { return _engine.smoke_region_opaque (); });
	run_smoke_check (ok, "export-selection", [this]() { return _engine.smoke_export_selection (); });
	run_smoke_check (ok, "punch-selection", [this]() { return _engine.smoke_punch_selection (); });
	run_smoke_check (ok, "region-rename", [this]() { return _engine.smoke_region_rename (); });
	run_smoke_check (ok, "region-caption", [this]() { return _engine.smoke_region_caption (); });
	run_smoke_check (ok, "loop-selection", [this]() { return _engine.smoke_loop_selection (); });
	run_smoke_check (ok, "crossfade-handles", [this]() { return _engine.smoke_crossfade_handles (); });
	run_smoke_check (ok, "crossfade-shape", [this]() { return _engine.smoke_crossfade_shape (); });
	run_smoke_check (ok, "fade-shape-expand", [this]() { return _engine.smoke_fade_shape_expand (); });
	run_smoke_check (ok, "region-envelope", [this]() { return _engine.smoke_region_envelope (); });
	run_smoke_check (ok, "region-envelope-nudge", [this]() { return _engine.smoke_region_envelope_nudge (); });
	run_smoke_check (ok, "region-envelope-drag", [this]() { return _engine.smoke_region_envelope_drag (); });
	run_smoke_check (ok, "envelope-mid-handle", [this]() { return smoke_envelope_mid_handle (); }, true);
	run_smoke_check (ok, "fade-active", [this]() { return _engine.smoke_fade_active (); });
	run_smoke_check (ok, "region-position-lock", [this]() { return _engine.smoke_region_position_lock (); });
	run_smoke_check (ok, "skip-range", [this]() { return _engine.smoke_skip_range (); });
	run_smoke_check (ok, "region-cycle", [this]() { return _engine.smoke_region_cycle (); });
	run_smoke_check (ok, "region-layer-cycle", [this]() { return _engine.smoke_region_layer_cycle (); });
	run_smoke_check (ok, "clear-selection", [this]() { return _engine.smoke_clear_selection (); });
	run_smoke_check (ok, "snap-toggle", [this]() { return _engine.smoke_snap_toggle (); });
	run_smoke_check (ok, "snap-density", [this]() { return _engine.smoke_snap_density (); });
	run_smoke_check (ok, "snap-density-label", [this]() { return smoke_snap_density_label (); });
	run_smoke_check (ok, "snap-ruler-ticks", [this]() { return smoke_snap_ruler_ticks (); });
	run_smoke_check (ok, "snap-status-chip", [this]() { return smoke_snap_status_chip (); });
	run_smoke_check (ok, "grid-refresh", [this]() { return _engine.smoke_grid_lines (); });
	run_smoke_check (ok, "tempo-nudge", [this]() { return _engine.smoke_tempo_nudge (); });
	run_smoke_check (ok, "count-in", [this]() { return _engine.smoke_count_in (); });
	run_smoke_check (ok, "click-toggle", [this]() { return _engine.smoke_click (); });
	run_smoke_check (ok, "punch-toggle", [this]() { return _engine.smoke_punch (); });
	run_smoke_check (ok, "midi-punch-grid", [this]() { return smoke_midi_punch_grid (); });
	run_smoke_check (ok, "midi-note-pressure", [this]() {
		if (!_engine.smoke_midi_note_pressure ()) {
			return false;
		}
		_midi_cc_number = -3;
		refresh_midi_cc_lane ();
		return true;
	});
	run_smoke_check (ok, "midi-vel-chord", [this]() { return smoke_midi_vel_chord (); });
	run_smoke_check (ok, "midi-cc-clear", [this]() { return smoke_midi_cc_clear (); });
	run_smoke_check (ok, "midi-duplicate", [this]() { return _engine.smoke_midi_duplicate (); });
	run_smoke_check (ok, "midi-tempo-map", [this]() { return _engine.smoke_midi_tempo_map (); });
	run_smoke_check (ok, "midi-move", [this]() { return _engine.smoke_midi_move (); });
	run_smoke_check (ok, "midi-panic", [this]() { return _engine.smoke_midi_panic (); });
	run_smoke_check (ok, "stop-midi-panic", [this]() { return _engine.smoke_stop_midi_panic (); });
	run_smoke_check (ok, "midi-audition", [this]() { return _engine.smoke_midi_audition (); });
	run_smoke_check (ok, "midi-audition-ghost", [this]() { return smoke_midi_audition_ghost (); });
	run_smoke_check (ok, "midi-legato", [this]() { return _engine.smoke_midi_legato (); });
	run_smoke_check (ok, "midi-step-piano-highlight", [this]() {
		return smoke_midi_step_piano_highlight ();
	});
	run_smoke_check (ok, "midi-force-length", [this]() { return _engine.smoke_midi_force_length (); });
	run_smoke_check (ok, "midi-playhead-draw", [this]() { return smoke_midi_playhead_draw (); });
	run_smoke_check (ok, "midi-multi-select", [this]() { return _engine.smoke_midi_multi_select (); });
	run_smoke_check (ok, "midi-marquee", [this]() { return _engine.smoke_midi_marquee (); });
	run_smoke_check (ok, "midi-select-all", [this]() { return _engine.smoke_midi_select_all (); });
	run_smoke_check (ok, "midi-invert-select", [this]() { return _engine.smoke_midi_invert_select (); });
	run_smoke_check (ok, "midi-select-same-pitch", [this]() { return _engine.smoke_midi_select_same_pitch (); });
	run_smoke_check (ok, "midi-select-same-velocity", [this]() { return _engine.smoke_midi_select_same_velocity (); });
	run_smoke_check (ok, "midi-select-pitch-class", [this]() { return _engine.smoke_midi_select_pitch_class (); });
	run_smoke_check (ok, "midi-select-same-length", [this]() { return _engine.smoke_midi_select_same_length (); });
	run_smoke_check (ok, "midi-select-same-channel", [this]() { return _engine.smoke_midi_select_same_channel (); });
	run_smoke_check (ok, "midi-select-in-loop", [this]() { return _engine.smoke_midi_select_in_loop (); });
	run_smoke_check (ok, "midi-select-in-punch", [this]() { return _engine.smoke_midi_select_in_punch (); });
	run_smoke_check (ok, "midi-select-outside-loop", [this]() { return _engine.smoke_midi_select_outside_loop (); });
	run_smoke_check (ok, "midi-select-outside-punch", [this]() { return _engine.smoke_midi_select_outside_punch (); });
	run_smoke_check (ok, "midi-select-outside-ranges", [this]() { return _engine.smoke_midi_select_outside_ranges (); });
	run_smoke_check (ok, "midi-select-in-ranges", [this]() { return _engine.smoke_midi_select_in_ranges (); });
	run_smoke_check (ok, "midi-outside-ranges-wash", [this]() { return smoke_midi_outside_ranges_wash (); });
	run_smoke_check (ok, "midi-force-velocity", [this]() { return _engine.smoke_midi_force_velocity (); });
	run_smoke_check (ok, "midi-program", [this]() { return _engine.smoke_midi_program (); });
	run_smoke_check (ok, "vst3-params", [this]() { return _engine.smoke_vst3_params (); });
	run_smoke_check (ok, "vst3-mixer-param-reset", [this]() {
		return _engine.smoke_vst3_mixer_param_reset ();
	});
	run_smoke_check (ok, "vst3-lifecycle", [this]() {
		if (!_engine.smoke_vst3_lifecycle ()) {
			return false;
		}
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_fx_from_engine ();
		return true;
	});
	run_smoke_check (ok, "vst3-catalog", [this]() {
		if (!_engine.smoke_vst3_catalog ()) {
			return false;
		}
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_prefs_from_engine ();
		return true;
	});
	run_smoke_check (ok, "vst3-insert-index", [this]() {
		if (!_engine.smoke_vst3_insert_index ()) {
			return false;
		}
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_fx_from_engine ();
		sync_prefs_from_engine ();
		return true;
	});
	run_smoke_check (ok, "vst3-prefs-click", [this]() { return smoke_vst3_prefs_click (); });
	run_smoke_check (ok, "vst3-mixer-unload", [this]() { return smoke_vst3_mixer_unload (); });
	run_smoke_check (ok, "vst3-mixer-bypass", [this]() { return smoke_vst3_mixer_bypass (); });
	run_smoke_check (ok, "vst3-mixer-param-chips", [this]() { return smoke_vst3_mixer_param_reset (); });
	run_smoke_check (ok, "vst3-catalog-highlight", [this]() { return smoke_vst3_catalog_highlight (); });
	run_smoke_check (ok, "vst3-cycle-insert", [this]() { return _engine.smoke_vst3_cycle_insert (); });
	run_smoke_check (ok, "vst3-prefs-bypass", [this]() { return smoke_vst3_prefs_bypass (); });
	run_smoke_check (ok, "vst3-prefs-param-reset", [this]() { return smoke_vst3_prefs_param_reset (); });
	run_smoke_check (ok, "vst3-prefs-unload", [this]() { return smoke_vst3_prefs_unload (); });
	run_smoke_check (ok, "vst3-status-unload", [this]() { return smoke_vst3_status_unload (); });
	run_smoke_check (ok, "vst3-status-bypass", [this]() { return smoke_vst3_status_bypass (); });
	run_smoke_check (ok, "vst3-status-param-reset", [this]() { return smoke_vst3_status_param_reset (); });
	run_smoke_check (ok, "meter-clip-led", [this]() { return smoke_meter_clip_led (); });
	run_smoke_check (ok, "export-stems", [this]() { return _engine.smoke_export_stems (); });
	run_smoke_check (ok, "mute-all", [this]() { return _engine.smoke_mute_all (); });
	run_smoke_check (ok, "solo-all", [this]() { return _engine.smoke_solo_all (); });
	run_smoke_check (ok, "midi-arm", [this]() { return _engine.smoke_midi_arm (); });
	run_smoke_check (true, "cairo-wave", []() {
		if (!CairoWaveBlit::available ()) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-cairo-wave FAILED (AIRDOUR_HAVE_CAIRO off — run Install-HeadlessDeps -WithGtkLibs)\n");
			return false;
		}
		return CairoWaveBlit::smoke_offline ();
	});
	run_smoke_check (ok, "new-session", [this]() { return _engine.smoke_new_session (); });
	run_smoke_check (ok, "save-as", [this]() { return _engine.smoke_save_as (); });
	run_smoke_check (ok, "open-session", [this]() { return _engine.smoke_open_session (); });

	if (ok && smoke_zoom && engine_session_ok ()) {
		if (!_engine.smoke_zoom ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			refresh_engine_snap ();
			apply_region_selection_to_lanes ();
			float before = _lanes[0].strip.view_span_sec ();
			do_fit_session ();
			float span_sess = _lanes[0].strip.view_span_sec ();
			do_fit_selection ();
			float span_sel = _lanes[0].strip.view_span_sec ();
			do_fit_all_regions ();
			float span_all = _lanes[0].strip.view_span_sec ();
			if (span_sess < 0.2f || span_sel < 0.05f || span_all < 0.2f
			    || span_sel > span_sess * 1.05f + 0.5f) {
				std::fprintf (stderr,
				              "airdour_win_shell: smoke-zoom FAILED (UI fit spans sess=%.2f sel=%.2f all=%.2f before=%.2f)\n",
				              span_sess, span_sel, span_all, before);
				_smoke_failed = true;
			} else {
				std::fprintf (stderr,
				              "airdour_win_shell: smoke-zoom UI fit ok (sess=%.2fs sel=%.2fs all=%.2fs)\n",
				              span_sess, span_sel, span_all);
			}
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (smoke_zoom) {
		std::fprintf (stderr, "airdour_win_shell: smoke-zoom FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_history && engine_session_ok ()) {
		if (!_engine.smoke_history ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			refresh_engine_snap ();
			sync_history_from_engine ();
			layout_panes ();
		}
		std::fflush (stderr);
	} else if (smoke_history) {
		std::fprintf (stderr, "airdour_win_shell: smoke-history FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_prefs && engine_session_ok ()) {
		if (!_engine.smoke_prefs ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			refresh_engine_snap ();
			sync_prefs_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_prefs) {
		std::fprintf (stderr, "airdour_win_shell: smoke-prefs FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_recent && engine_session_ok ()) {
		if (!_engine.smoke_recent ()) {
			_smoke_failed = true;
		} else {
			_engine.poll ();
			refresh_engine_snap ();
			sync_recent_from_engine ();
		}
		std::fflush (stderr);
	} else if (smoke_recent) {
		std::fprintf (stderr, "airdour_win_shell: smoke-recent FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && smoke_recovery && engine_session_ok ()) {
		if (!_engine.smoke_recovery ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	} else if (smoke_recovery) {
		std::fprintf (stderr, "airdour_win_shell: smoke-recovery FAILED (no session)\n");
		_smoke_failed = true;
		std::fflush (stderr);
	}

	if (ok && has_smoke ("ui-layout")) {
		if (!smoke_ui_layout ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	}

	if (ok && has_smoke ("track-capacity")) {
		if (!smoke_track_capacity ()) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	}

	if (ok && has_smoke ("device")) {
		if (!_engine.smoke_device ()) {
			_smoke_failed = true;
		}
		_engine.poll ();
		refresh_engine_snap ();
		sync_prefs_from_engine ();
		std::fflush (stderr);
	}

	if (ok && has_smoke ("latency")) {
		if (!_engine.smoke_latency ()) {
			_smoke_failed = true;
		}
		_engine.poll ();
		refresh_engine_snap ();
		sync_prefs_from_engine ();
		std::fflush (stderr);
	}

	if (ok && smoke_perf) {
		unsigned secs = smoke_timeout_secs > 0 ? smoke_timeout_secs : 4u;
		if (secs > 12) {
			secs = 12;
		}
		if (!_engine.smoke_perf (secs)) {
			_smoke_failed = true;
		}
		std::fflush (stderr);
	}

	if (ok && smoke_record && engine_session_ok ()) {
		_smoke_record = SmokeRecordPhase::NeedArm;
		std::fprintf (stderr, "airdour_win_shell: smoke-record scheduled (arm → ~1.2s take)\n");
		std::fflush (stderr);
	}
	if (autoplay) {
		_engine.transport_play ();
	}
	_engine.poll ();
	refresh_engine_snap ();
	sync_mixer_arm_from_engine ();
	sync_mixer_fx_from_engine ();
	sync_transport_loop_from_engine ();
	sync_transport_punch_from_engine ();
	sync_transport_tempo_from_engine ();
	sync_transport_freeze_from_engine ();
	sync_track_labels_from_engine ();
	sync_mixer_gains_from_engine ();
	sync_mixer_group_from_engine ();
	if (_engine.is_linked () && _engine_snap.session_ok) {
		_engine.ensure_monitor_section ();
		_engine.poll ();
		refresh_engine_snap ();
	}
	sync_monitor_from_engine ();
	sync_history_from_engine ();
	sync_prefs_from_engine ();
	sync_recent_from_engine ();
	_transport.set_snap_enabled (_engine_snap.snap_enabled);
	_transport.set_snap_density (_engine_snap.snap_density);
	_transport.set_midi_step_enabled (_engine_snap.midi_step_enabled);
	_transport.set_midi_chord_quality (_engine_snap.midi_chord_quality);
	_transport.set_midi_arp_enabled (_engine_snap.midi_arp_enabled);
	_transport.set_midi_arp_mode (_engine_snap.midi_arp_mode);
	_transport.set_dsp_load_pct (_engine_snap.dsp_load_pct);
	sync_scale_to_transport ();
	layout_panes ();
	apply_loop_to_lanes ();
	apply_region_selection_to_lanes ();
	if (smoke_timeout_secs == 0 && !smoke_recovery) {
		maybe_show_recovery_prompt ();
	}
	if (autoplay) {
		_transport_playing = true;
		_transport.set_playing (true);
		if (_engine_snap.session_ok) {
			_playhead_sec = _engine_snap.transport_sec;
		}
	}
	return ok;
}

void
ShellWindow::refresh_engine_snap ()
{
	_engine_snap = _engine.snapshot ();
}

bool
ShellWindow::engine_session_ok ()
{
	return _engine.snapshot ().session_ok;
}

void
ShellWindow::sync_transport_loop_from_engine ()
{
	_transport.set_loop_active (_engine_snap.loop_active);
	_transport.set_loop_playing (_engine_snap.loop_playing);
}

void
ShellWindow::sync_transport_punch_from_engine ()
{
	_transport.set_punch_enabled (_engine_snap.punch_enabled);
}

void
ShellWindow::sync_transport_tempo_from_engine ()
{
	_transport.set_tempo_bpm (_engine_snap.tempo_bpm);
	_transport.set_count_in_enabled (_engine_snap.count_in_enabled);
	_transport.set_click_enabled (_engine_snap.click_enabled);
	_transport.set_click_gain (_engine_snap.click_gain);
}

void
ShellWindow::sync_transport_clock_from_engine ()
{
	_transport.set_clock_bbt (_engine_snap.bbt_bars, _engine_snap.bbt_beats, _engine_snap.bbt_ticks);
	_transport.set_clock_timecode (_engine_snap.tc_hours, _engine_snap.tc_minutes,
	                               _engine_snap.tc_seconds, _engine_snap.tc_frames);
	float sn = 0.f;
	if (std::fabs (_engine_snap.transport_speed) > 0.05f) {
		/* Map ±8 → ±1 for thumb. */
		sn = _engine_snap.transport_speed / 8.f;
		if (sn > 1.f) {
			sn = 1.f;
		}
		if (sn < -1.f) {
			sn = -1.f;
		}
	}
	if (_drag != DragMode::Shuttle) {
		_transport.set_shuttle_norm (sn);
	}
}

int
ShellWindow::n_audio_lanes () const
{
	int n = _engine_snap.audio_track_count;
	if (!_engine_snap.session_ok || n < 1) {
		n = 3;
	}
	if (n > kTracksMax) {
		n = kTracksMax;
	}
	return n;
}

int
ShellWindow::n_mixer_strips () const
{
	int n = n_audio_lanes ();
	if (_engine_snap.midi_track_ok || _engine_snap.midi_track_count > 0 || !_engine_snap.session_ok) {
		++n;
	}
	if (n > kMixerStripsMax) {
		n = kMixerStripsMax;
	}
	return n;
}

int
ShellWindow::midi_mixer_index () const
{
	return n_audio_lanes ();
}

void
ShellWindow::sync_track_labels_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < kTracksMax; ++i) {
		wchar_t lab[32];
		if (i < na && _engine_snap.track_name[i][0] != L'\0') {
			wcsncpy_s (lab, _engine_snap.track_name[i], _TRUNCATE);
		} else {
			_snwprintf_s (lab, _TRUNCATE, L"Audio %d", i + 1);
		}
		wcsncpy_s (_lanes[i].label, lab, _TRUNCATE);
		_mixers[i].set_label (lab);
		_mixers[i].set_color (i < EngineSnapshot::kMeters ? _engine_snap.track_color[i] : 0);
		_mixers[i].set_midi_strip (false);
	}
	const int mi = midi_mixer_index ();
	if (mi >= 0 && mi < kMixerStripsMax) {
		if (_engine_snap.midi_track_name[0] != L'\0') {
			_mixers[mi].set_label (_engine_snap.midi_track_name);
		} else {
			_mixers[mi].set_label (L"MIDI 1");
		}
		_mixers[mi].set_color (0);
		_mixers[mi].set_midi_strip (true);
	}
	refresh_automation_lane ();
}

void
ShellWindow::sync_mixer_gains_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_gain (_engine_snap.track_gain[i]);
	}
}

void
ShellWindow::sync_mixer_group_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < kTracksMax; ++i) {
		bool in = (i < na && i < EngineSnapshot::kMeters && _engine_snap.track_in_group[i]);
		_mixers[i].set_in_group (in);
		_mixers[i].set_group_picked (_group_pick[i]);
		uint32_t rgba = 0;
		if (in && _engine_snap.group_shares_color) {
			rgba = _engine_snap.group_rgba;
		}
		_mixers[i].set_group_rgba (rgba);
		bool sel = (i < na && i < EngineSnapshot::kMeters && _engine_snap.track_selected[i]);
		_mixers[i].set_selected (sel);
	}
	_transport.set_group_active (_engine_snap.group_member_count >= 2
	                             && _engine_snap.group_active);
	_transport.set_group_relative (_engine_snap.group_member_count >= 2
	                              && _engine_snap.group_relative);
	_transport.set_spectrum_enabled (_engine_snap.spectrum_enabled);
	_transport.set_correlation_enabled (_engine_snap.correlation_enabled);
	_transport.set_loudness_enabled (_engine_snap.loudness_enabled);
}

void
ShellWindow::sync_mixer_arm_from_engine ()
{
	const int na = n_audio_lanes ();
	const int mi = midi_mixer_index ();
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].set_armed (false);
		_mixers[i].set_record_safe (false);
	}
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_armed (_engine_snap.track_armed[i]);
		_mixers[i].set_record_safe (_engine_snap.track_record_safe[i]);
	}
	if (mi >= 0 && mi < kMixerStripsMax) {
		_mixers[mi].set_armed (_engine_snap.midi_armed);
		_mixers[mi].set_record_safe (false);
	}
	_transport.set_record_armed (_engine_snap.session_record_armed);
}

void
ShellWindow::sync_mixer_monitor_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].set_monitor_choice (0);
		_mixers[i].set_monitor_peer_count (0);
	}
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_monitor_choice (_engine_snap.track_monitor_choice[i]);
		_mixers[i].set_monitor_peer_count (_engine_snap.track_monitor_peer_count[i]);
	}
}

void
ShellWindow::sync_mixer_mute_solo_from_engine ()
{
	const int na = n_audio_lanes ();
	const int mi = midi_mixer_index ();
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_muted (_engine_snap.track_muted[i]);
		_mixers[i].set_mute_points (_engine_snap.track_mute_points[i]);
		_mixers[i].set_soloed (_engine_snap.track_soloed[i]);
		_mixers[i].set_route_active (_engine_snap.track_active[i]);
	}
	if (mi >= 0 && mi < kMixerStripsMax) {
		_mixers[mi].set_muted (_engine_snap.midi_muted);
		_mixers[mi].set_mute_points (0);
		_mixers[mi].set_soloed (_engine_snap.midi_soloed);
		_mixers[mi].set_route_active (true);
	}
}

void
ShellWindow::sync_mixer_solo_isolate_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].set_solo_isolated (false);
	}
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_solo_isolated (_engine_snap.track_solo_isolated[i]);
	}
}

void
ShellWindow::sync_mixer_solo_safe_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].set_solo_safe (false);
	}
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_solo_safe (_engine_snap.track_solo_safe[i]);
	}
}

void
ShellWindow::sync_mixer_comment_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].set_has_comment (false);
	}
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_has_comment (_engine_snap.track_comment[i][0] != L'\0');
	}
}

void
ShellWindow::sync_mixer_phase_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].set_phase_inverted (false);
	}
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_phase_inverted (_engine_snap.track_phase_invert[i]);
	}
}

void
ShellWindow::sync_mixer_trim_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_trim (_engine_snap.track_trim_norm[i]);
	}
}

void
ShellWindow::sync_mixer_listen_from_engine ()
{
	const int n = n_mixer_strips ();
	for (int i = 0; i < n; ++i) {
		_mixers[i].set_listen_mode (_engine_snap.listen_mode);
	}
}

void
ShellWindow::sync_mixer_pan_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_pan (_engine_snap.track_pan[i]);
		_mixers[i].set_pan_width (_engine_snap.track_pan_width[i]);
		_mixers[i].set_pan_width_ok (_engine_snap.track_pan_width_ok[i]);
	}
	_transport.set_recording (_engine_snap.session_recording);
}

void
ShellWindow::sync_mixer_send_from_engine ()
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na && i < EngineSnapshot::kMeters; ++i) {
		_mixers[i].set_send (_engine_snap.track_send[i]);
		_mixers[i].set_send_muted (_engine_snap.track_send_muted[i]);
		_mixers[i].set_send_pan (_engine_snap.track_send_pan[i]);
		_mixers[i].set_send_pan_ok (_engine_snap.track_send_pan_ok[i]);
	}
}

int
ShellWindow::auto_lane_track () const
{
	int t = _focus_audio_track;
	const int na = n_audio_lanes ();
	if (t < 0) {
		t = 0;
	}
	if (na > 0 && t >= na) {
		t = na - 1;
	}
	if (t >= EngineSnapshot::kMeters) {
		t = EngineSnapshot::kMeters - 1;
	}
	return t;
}

void
ShellWindow::cycle_auto_lane_kind ()
{
	_auto_lane_kind = (_auto_lane_kind + 1) % 3;
	refresh_automation_lane ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

const wchar_t*
ShellWindow::auto_state_label (int state_0_3)
{
	switch (state_0_3) {
	case 1:
		return L"Play";
	case 2:
		return L"Write";
	case 3:
		return L"Touch";
	default:
		return L"Off";
	}
}

void
ShellWindow::do_cycle_auto_state ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	switch (_auto_lane_kind) {
	case 1:
		_engine.cycle_track_pan_auto_state (auto_lane_track ());
		break;
	case 2:
		_engine.cycle_track_send_auto_state (auto_lane_track ());
		break;
	default:
		_engine.cycle_track_gain_auto_state (auto_lane_track ());
		break;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	refresh_automation_lane ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::refresh_automation_lane ()
{
	const int track = auto_lane_track ();
	const wchar_t* kind = L"Gain";
	int st_i = _engine_snap.track_gain_auto_state[track];
	if (_auto_lane_kind == 1) {
		kind = L"Pan";
		st_i = _engine_snap.track_pan_auto_state[track];
	} else if (_auto_lane_kind == 2) {
		kind = L"Send";
		st_i = _engine_snap.track_send_auto_state[track];
	}
	const wchar_t* st = auto_state_label (st_i);
	if (_engine_snap.track_name[track][0] != L'\0') {
		wchar_t auto_lab[64];
		_snwprintf_s (auto_lab, _TRUNCATE, L"%s · %s · %s", kind, st,
		              _engine_snap.track_name[track]);
		_auto_lane.set_label (auto_lab);
	} else {
		wchar_t auto_lab[48];
		_snwprintf_s (auto_lab, _TRUNCATE, L"%s · %s · Audio %d", kind, st, track + 1);
		_auto_lane.set_label (auto_lab);
	}
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	float curve[AutomationLane::kMaxColumns];
	size_t n = AutomationLane::kMaxColumns;
	if (_engine.is_linked () && _engine_snap.session_ok) {
		if (_auto_lane_kind == 1) {
			_engine.fill_pan_automation_curve (track, view0, span, curve, n);
		} else if (_auto_lane_kind == 2) {
			_engine.fill_send_automation_curve (track, view0, span, curve, n);
		} else {
			_engine.fill_gain_automation_curve (track, view0, span, curve, n);
		}
	} else {
		float flat = (track >= 0 && track < kMixerStripsMax) ? _mixers[track].gain () : 0.5f;
		if (_auto_lane_kind == 1) {
			flat = (track >= 0 && track < kMixerStripsMax) ? _mixers[track].pan () : 0.f;
		} else if (_auto_lane_kind == 2) {
			flat = (track >= 0 && track < kMixerStripsMax) ? _mixers[track].send () : 0.f;
		}
		for (size_t i = 0; i < n; ++i) {
			curve[i] = flat;
		}
	}
	_auto_lane.set_curve (curve, n);

	float px[AutomationLane::kMaxPoints];
	float py[AutomationLane::kMaxPoints];
	int npt = 0;
	if (_engine.is_linked () && _engine_snap.session_ok) {
		if (_auto_lane_kind == 1) {
			npt = _engine.fill_pan_automation_points (
				track, view0, span, px, py, (int)AutomationLane::kMaxPoints);
		} else if (_auto_lane_kind == 2) {
			npt = _engine.fill_send_automation_points (
				track, view0, span, px, py, (int)AutomationLane::kMaxPoints);
		} else {
			npt = _engine.fill_gain_automation_points (
				track, view0, span, px, py, (int)AutomationLane::kMaxPoints);
		}
	}
	_auto_lane.set_points (px, py, (size_t)std::max (0, npt));
	_auto_lane.set_playhead_norm (_lanes[0].strip.playhead_norm ());
}

void
ShellWindow::write_automation_at (float x, float y)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	const int track = auto_lane_track ();
	float time_sec = _lanes[track].strip.time_at_x (_auto_lane_bounds, x);
	float value = _auto_lane.value_at_y (_auto_lane_bounds, y);
	/* Thin writes while dragging — skip near-duplicate times. */
	if (_drag == DragMode::AutoWrite && _auto_last_write_sec >= 0.f
	    && std::fabs (time_sec - _auto_last_write_sec) < 0.035f) {
		return;
	}
	bool ok = false;
	if (_auto_lane_kind == 1) {
		ok = _engine.write_track_pan_automation (track, time_sec, value);
	} else if (_auto_lane_kind == 2) {
		ok = _engine.write_track_send_automation (track, time_sec, value);
	} else {
		ok = _engine.write_track_gain_automation (track, time_sec, value);
	}
	if (ok) {
		_auto_last_write_sec = time_sec;
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		refresh_automation_lane ();
	}
}

void
ShellWindow::draw_editor_scrollbar ()
{
	if (_editor_scrollbar.right <= _editor_scrollbar.left || _editor_scroll_max <= 0.5f) {
		return;
	}
	const float w = _editor_scrollbar.right - _editor_scrollbar.left;
	const float rad = w * 0.5f / std::max (0.01f, _dpi_scale);
	_ui.fill_round (_editor_scrollbar, rad, ui::ColorToken::Control, 0.45f);
	const float view = std::max (1.f, _editor_lanes_bot - _editor_lanes_top);
	const float content = view + _editor_scroll_max;
	float thumb_h = view * (view / content);
	thumb_h = std::max (thumb_h, 24.f * _dpi_scale);
	if (thumb_h > _editor_scrollbar.bottom - _editor_scrollbar.top) {
		thumb_h = _editor_scrollbar.bottom - _editor_scrollbar.top;
	}
	const float travel = (_editor_scrollbar.bottom - _editor_scrollbar.top) - thumb_h;
	const float pos = std::max (0.f, std::min (1.f, _editor_scroll_y / _editor_scroll_max));
	const float top = _editor_scrollbar.top + travel * pos;
	_ui.fill_round (D2D1::RectF (_editor_scrollbar.left, top,
	                             _editor_scrollbar.right, top + thumb_h),
	                rad, ui::ColorToken::Accent, 0.65f);
}

void
ShellWindow::draw_automation_lane ()
{
	if (_auto_lane_bounds.right <= _auto_lane_bounds.left) {
		return;
	}
	_ui.fill_round (_auto_lane_bounds, 3.f, ui::ColorToken::Surface, 0.42f);
	_ui.stroke_round (_auto_lane_bounds, 3.f, ui::ColorToken::Border, 1.f, 0.55f);
	_auto_lane.draw (_rt, _auto_lane_bounds, _brush_accent, _brush_body,
	                 _brush_pane_edge, _brush_playhead, _fmt_meta, _dpi_scale);
}

const wchar_t*
ShellWindow::midi_cc_label () const
{
	switch (_midi_cc_number) {
	case 1:
		return L"CC1 · Mod";
	case 11:
		return L"CC11 · Expr";
	case 64:
		return L"CC64 · Sustain";
	case -1:
		return L"Bend";
	case -2:
		return L"Pressure";
	case -3:
		return L"PolyAT · C4";
	default:
		return L"CC7 · Volume";
	}
}

void
ShellWindow::cycle_midi_cc_controller ()
{
	if (_midi_cc_number == 7) {
		_midi_cc_number = 1;
	} else if (_midi_cc_number == 1) {
		_midi_cc_number = 11;
	} else if (_midi_cc_number == 11) {
		_midi_cc_number = 64;
	} else if (_midi_cc_number == 64) {
		_midi_cc_number = -1; /* pitch bend */
	} else if (_midi_cc_number == -1) {
		_midi_cc_number = -2; /* channel pressure */
	} else if (_midi_cc_number == -2) {
		_midi_cc_number = -3; /* poly aftertouch · C4 */
	} else {
		_midi_cc_number = 7;
	}
	_midi_cc_last_write_sec = -1.f;
	if (_engine.is_linked () && _engine_snap.session_ok) {
		if (_midi_cc_number == -1) {
			_engine.ensure_midi_bend_automation ();
		} else if (_midi_cc_number == -2) {
			_engine.ensure_midi_pressure_automation ();
		} else if (_midi_cc_number == -3) {
			_engine.ensure_midi_note_pressure_automation ();
		} else {
			_engine.ensure_midi_cc_automation (_midi_cc_number);
		}
	}
	refresh_midi_cc_lane ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::refresh_midi_cc_lane ()
{
	_midi_cc_lane.set_label (midi_cc_label ());
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	float curve[AutomationLane::kMaxColumns];
	size_t n = AutomationLane::kMaxColumns;
	if (_engine.is_linked () && _engine_snap.session_ok) {
		if (_midi_cc_number == -1) {
			_engine.fill_midi_bend_automation_curve (view0, span, curve, n);
		} else if (_midi_cc_number == -2) {
			_engine.fill_midi_pressure_automation_curve (view0, span, curve, n);
		} else if (_midi_cc_number == -3) {
			_engine.fill_midi_note_pressure_automation_curve (view0, span, curve, n);
		} else {
			_engine.fill_midi_cc_automation_curve (
				view0, span, curve, n, _midi_cc_number);
		}
	} else {
		float def = 0.f;
		if (_midi_cc_number == 7) {
			def = 100.f / 127.f;
		} else if (_midi_cc_number == -1) {
			def = 0.5f;
		}
		for (size_t i = 0; i < n; ++i) {
			curve[i] = def;
		}
	}
	_midi_cc_lane.set_curve (curve, n);

	float px[AutomationLane::kMaxPoints];
	float py[AutomationLane::kMaxPoints];
	int npt = 0;
	if (_engine.is_linked () && _engine_snap.session_ok) {
		if (_midi_cc_number == -1) {
			npt = _engine.fill_midi_bend_automation_points (
				view0, span, px, py, (int)AutomationLane::kMaxPoints);
		} else if (_midi_cc_number == -2) {
			npt = _engine.fill_midi_pressure_automation_points (
				view0, span, px, py, (int)AutomationLane::kMaxPoints);
		} else if (_midi_cc_number == -3) {
			npt = _engine.fill_midi_note_pressure_automation_points (
				view0, span, px, py, (int)AutomationLane::kMaxPoints);
		} else {
			npt = _engine.fill_midi_cc_automation_points (
				view0, span, px, py, (int)AutomationLane::kMaxPoints,
				_midi_cc_number);
		}
	}
	_midi_cc_lane.set_points (px, py, (size_t)std::max (0, npt));
	_midi_cc_lane.set_playhead_norm (_lanes[0].strip.playhead_norm ());
}

void
ShellWindow::write_midi_cc_at (float x, float y)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	float time_sec = _lanes[0].strip.time_at_x (_midi_cc_lane_bounds, x);
	float value = _midi_cc_lane.value_at_y (_midi_cc_lane_bounds, y);
	if (_drag == DragMode::MidiCcWrite && _midi_cc_last_write_sec >= 0.f
	    && std::fabs (time_sec - _midi_cc_last_write_sec) < 0.035f) {
		return;
	}
	bool ok = false;
	if (_midi_cc_number == -1) {
		ok = _engine.write_midi_bend_automation (time_sec, value);
	} else if (_midi_cc_number == -2) {
		ok = _engine.write_midi_pressure_automation (time_sec, value);
	} else if (_midi_cc_number == -3) {
		ok = _engine.write_midi_note_pressure_automation (time_sec, value);
	} else {
		ok = _engine.write_midi_cc_automation (time_sec, value, _midi_cc_number);
	}
	if (ok) {
		_midi_cc_last_write_sec = time_sec;
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		refresh_midi_cc_lane ();
	}
}

void
ShellWindow::draw_midi_cc_lane ()
{
	if (_midi_cc_lane_bounds.right <= _midi_cc_lane_bounds.left) {
		return;
	}
	_ui.fill_round (_midi_cc_lane_bounds, 3.f, ui::ColorToken::Surface, 0.42f);
	_ui.stroke_round (_midi_cc_lane_bounds, 3.f, ui::ColorToken::Border, 1.f, 0.55f);
	const float inset = 12.f * _dpi_scale;
	_ui.text_vcenter (midi_cc_label (), ui::TextRole::Caption,
	                  ui::ColorToken::TextSecondary,
	                  D2D1::RectF (_editor_pane.left + inset, _midi_cc_lane_bounds.top,
	                               _midi_cc_lane_bounds.left - 4.f * _dpi_scale,
	                               _midi_cc_lane_bounds.bottom),
	                  DWRITE_TEXT_ALIGNMENT_TRAILING);
	_midi_cc_lane.draw (_rt, _midi_cc_lane_bounds, _brush_midi, _brush_body,
	                    _brush_pane_edge, _brush_playhead, _fmt_meta, _dpi_scale);
}

void
ShellWindow::sync_mixer_fx_from_engine ()
{
	for (int i = 0; i < kTracks; ++i) {
		_mixers[i].set_fx_can_unload (false);
		if (i == 0 && _engine_snap.track_fx_ok && !_engine_snap.track_fx_name.empty ()) {
			_mixers[i].set_fx_label (_engine_snap.track_fx_name.c_str ());
			_mixers[i].set_fx_bypassed (_engine_snap.track_fx_bypassed);
			_mixers[i].set_fx_param_count (1);
			_mixers[i].set_fx_param (_engine_snap.track_fx_param);
		} else if (i == 1 && _engine_snap.vst3_ok && _engine_snap.vst3_name[0] != L'\0') {
			/* Prefer live VST3 insert on Audio 2 over ACE Comp when both present. */
			_mixers[i].set_fx_label (_engine_snap.vst3_name);
			_mixers[i].set_fx_bypassed (_engine_snap.vst3_bypassed);
			_mixers[i].set_fx_can_unload (true);
			const int n = (_engine_snap.vst3_param_count > 0)
			                  ? (std::min) (_engine_snap.vst3_param_count,
			                               EngineSnapshot::kMaxVst3Params)
			                  : 0;
			_mixers[i].set_fx_param_count (n);
			_mixers[i].set_fx_param (
			    (_engine_snap.vst3_param_count > 0) ? _engine_snap.vst3_param_norm[0] : 0.5f);
			_mixers[i].set_fx_param2 (
			    (_engine_snap.vst3_param_count > 1) ? _engine_snap.vst3_param_norm[1] : 0.5f);
			_mixers[i].set_fx_param3 (
			    (_engine_snap.vst3_param_count > 2) ? _engine_snap.vst3_param_norm[2] : 0.5f);
		} else if (i == 1 && _engine_snap.track_comp_ok
		           && !_engine_snap.track_comp_name.empty ()) {
			wchar_t label[40];
			if (_engine_snap.track_comp_sidechain) {
				_snwprintf_s (label, _TRUNCATE, L"%s SC",
				              _engine_snap.track_comp_name.c_str ());
			} else {
				wcsncpy_s (label, _engine_snap.track_comp_name.c_str (), _TRUNCATE);
			}
			_mixers[i].set_fx_label (label);
			_mixers[i].set_fx_bypassed (_engine_snap.track_comp_bypassed);
			_mixers[i].set_fx_param_count (2);
			_mixers[i].set_fx_param (_engine_snap.track_comp_threshold_norm);
			_mixers[i].set_fx_param2 (_engine_snap.track_comp_ratio_norm);
		} else if (i == 2 && _engine_snap.track_eq_ok
		           && !_engine_snap.track_eq_name.empty ()) {
			_mixers[i].set_fx_label (_engine_snap.track_eq_name.c_str ());
			_mixers[i].set_fx_bypassed (_engine_snap.track_eq_bypassed);
			_mixers[i].set_fx_param_count (3);
			_mixers[i].set_fx_param (_engine_snap.track_eq_gain_l_norm);
			_mixers[i].set_fx_param2 (_engine_snap.track_eq_gain_m_norm);
			_mixers[i].set_fx_param3 (_engine_snap.track_eq_gain_h_norm);
		} else if (i == 3 && _engine_snap.track_delay_ok
		           && !_engine_snap.track_delay_name.empty ()) {
			_mixers[i].set_fx_label (_engine_snap.track_delay_name.c_str ());
			_mixers[i].set_fx_bypassed (_engine_snap.track_delay_bypassed);
			_mixers[i].set_fx_param_count (2);
			_mixers[i].set_fx_param (_engine_snap.track_delay_time_norm);
			_mixers[i].set_fx_param2 (_engine_snap.track_delay_feedback_norm);
		} else if (i == 4 && _engine_snap.track_limiter_ok
		           && !_engine_snap.track_limiter_name.empty ()) {
			_mixers[i].set_fx_label (_engine_snap.track_limiter_name.c_str ());
			_mixers[i].set_fx_bypassed (_engine_snap.track_limiter_bypassed);
			_mixers[i].set_fx_param_count (2);
			_mixers[i].set_fx_param (_engine_snap.track_limiter_ceiling_norm);
			_mixers[i].set_fx_param2 (_engine_snap.track_limiter_release_norm);
		} else if (i == 5 && _engine_snap.track_gate_ok
		           && !_engine_snap.track_gate_name.empty ()) {
			_mixers[i].set_fx_label (_engine_snap.track_gate_name.c_str ());
			_mixers[i].set_fx_bypassed (_engine_snap.track_gate_bypassed);
			_mixers[i].set_fx_param_count (2);
			_mixers[i].set_fx_param (_engine_snap.track_gate_threshold_norm);
			_mixers[i].set_fx_param2 (_engine_snap.track_gate_hold_norm);
		} else {
			_mixers[i].set_fx_label (nullptr);
			_mixers[i].set_fx_bypassed (false);
			_mixers[i].set_fx_param_count (1);
			_mixers[i].set_fx_param (0.3f);
			_mixers[i].set_fx_param2 (0.16f);
			_mixers[i].set_fx_param3 (0.5f);
		}
	}
}

void
ShellWindow::transport_toggle_record ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	_engine.toggle_session_record ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_arm_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::prompt_export_wav ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}

	wchar_t path[MAX_PATH] = L"airdour_export.wav";
	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof (ofn);
	ofn.hwndOwner = _hwnd;
	ofn.lpstrFilter = L"WAV audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0";
	ofn.lpstrFile = path;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
	ofn.lpstrTitle = L"Airdour — export WAV";
	ofn.lpstrDefExt = L"wav";
	if (!GetSaveFileNameW (&ofn)) {
		return;
	}

	char utf8[MAX_PATH * 4];
	WideCharToMultiByte (CP_UTF8, 0, path, -1, utf8, sizeof (utf8), nullptr, nullptr);
	if (_engine.export_session_wav (utf8)) {
		std::fprintf (stderr, "airdour_win_shell: export dialog ok\n");
	} else {
		std::fprintf (stderr, "airdour_win_shell: export dialog failed\n");
	}
	std::fflush (stderr);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_save_session ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	_engine.save_session ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::new_session_at_path (const std::string& path)
{
	if (!_engine.is_linked () || path.empty ()) {
		return;
	}
	std::string p = path;
	while (!p.empty () && (p.back () == '\\' || p.back () == '/')) {
		p.pop_back ();
	}
	const size_t sep = p.find_last_of ("\\/");
	const std::string parent = (sep == std::string::npos)
	                           ? std::string (".") : p.substr (0, sep);
	const std::string name = (sep == std::string::npos) ? p : p.substr (sep + 1);

	const bool ok = _engine.new_session (parent, name);
	std::fprintf (stderr, "airdour_win_shell: --new-session %s (%s)\n",
	              ok ? "ok" : "FAILED", path.c_str ());
	std::fflush (stderr);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_arm_from_engine ();
	sync_track_labels_from_engine ();
	sync_recent_from_engine ();
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_new_session ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}

	std::wstring folder;
	if (!pick_folder (_hwnd, L"Airdour — choose a folder for the new session", &folder)) {
		return;
	}
	wchar_t name[128] = L"Untitled";
	if (!run_text_prompt (_hwnd, L"New session — name", name, 128)) {
		return;
	}

	const bool ok = _engine.new_session (wstring_to_utf8 (folder),
	                                     wstring_to_utf8 (std::wstring (name)));
	std::fprintf (stderr, "airdour_win_shell: new session %s\n", ok ? "ok" : "FAILED");
	std::fflush (stderr);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_arm_from_engine ();
	sync_track_labels_from_engine ();
	sync_recent_from_engine ();
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
	if (!ok) {
		MessageBoxW (_hwnd, L"Could not create the session.\n"
		             L"The session folder must not already exist.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
}

void
ShellWindow::do_save_session_as ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	if (!_engine_snap.session_ok) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	if (!_engine_snap.session_ok) {
		return;
	}

	std::wstring folder;
	if (!pick_folder (_hwnd, L"Save session as — choose a folder", &folder)) {
		return;
	}

	wchar_t name[128] = L"Untitled";
	if (!_engine_snap.session_path.empty ()) {
		size_t slash = _engine_snap.session_path.find_last_of (L"\\/");
		std::wstring base = (slash == std::wstring::npos)
		                    ? _engine_snap.session_path
		                    : _engine_snap.session_path.substr (slash + 1);
		wcsncpy_s (name, base.c_str (), _TRUNCATE);
	}
	if (!run_text_prompt (_hwnd, L"Save session as — name", name, 128)) {
		return;
	}

	const bool ok = _engine.save_session_as (wstring_to_utf8 (folder),
	                                         wstring_to_utf8 (std::wstring (name)));
	std::fprintf (stderr, "airdour_win_shell: save as %s\n", ok ? "ok" : "FAILED");
	std::fflush (stderr);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_arm_from_engine ();
	sync_track_labels_from_engine ();
	sync_recent_from_engine ();
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
	if (!ok) {
		MessageBoxW (_hwnd, L"Could not save a copy of the session.\n"
		             L"The destination folder must not already exist.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
}

void
ShellWindow::do_open_session_folder ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}

	std::wstring folder;
	if (!pick_folder (_hwnd, L"Airdour — open session folder", &folder)) {
		return;
	}

	const bool ok = _engine.open_session (wstring_to_utf8 (folder));
	std::fprintf (stderr, "airdour_win_shell: open session folder %s\n", ok ? "ok" : "FAILED");
	std::fflush (stderr);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_arm_from_engine ();
	sync_track_labels_from_engine ();
	sync_recent_from_engine ();
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
	if (!ok) {
		MessageBoxW (_hwnd, L"Could not open that folder as an Airdour session.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
}

void
ShellWindow::prompt_open_session ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}

	wchar_t path[MAX_PATH] = {};
	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof (ofn);
	ofn.hwndOwner = _hwnd;
	ofn.lpstrFilter = L"Airdour session (*.ardour)\0*.ardour\0All files (*.*)\0*.*\0";
	ofn.lpstrFile = path;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
	ofn.lpstrTitle = L"Airdour — open session";
	if (!GetOpenFileNameW (&ofn)) {
		return;
	}

	/* path = ...\session_dir\snapshot.ardour → dir + snapshot stem */
	wchar_t* slash = wcsrchr (path, L'\\');
	if (!slash) {
		slash = wcsrchr (path, L'/');
	}
	if (!slash) {
		return;
	}
	*slash = L'\0';
	const wchar_t* file = slash + 1;
	std::wstring snap_w (file);
	size_t dot = snap_w.find_last_of (L'.');
	if (dot != std::wstring::npos) {
		snap_w = snap_w.substr (0, dot);
	}

	char dir_utf8[MAX_PATH * 4];
	char snap_utf8[MAX_PATH];
	WideCharToMultiByte (CP_UTF8, 0, path, -1, dir_utf8, sizeof (dir_utf8), nullptr, nullptr);
	WideCharToMultiByte (CP_UTF8, 0, snap_w.c_str (), -1, snap_utf8, sizeof (snap_utf8), nullptr, nullptr);

	if (_engine.open_session (dir_utf8, snap_utf8)) {
		std::fprintf (stderr, "airdour_win_shell: open dialog ok\n");
	} else {
		std::fprintf (stderr, "airdour_win_shell: open dialog failed\n");
	}
	std::fflush (stderr);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_arm_from_engine ();
	sync_recent_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::sync_recent_from_engine ()
{
	_recent.set_rows (_engine_snap.recent_count, _engine_snap.recent_label, _engine_snap.recent_path);
}

void
ShellWindow::toggle_recent_menu ()
{
	dismiss_splash ();
	if (_prefs_visible) {
		_prefs_visible = false;
		_prefs.set_visible (false);
	}
	if (_about.about_visible ()) {
		_about.set_about_visible (false);
	}
	if (_help_visible) {
		_help_visible = false;
	}
	bool on = !_recent.visible ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_recent_from_engine ();
	_recent.set_visible (on);
	if (on) {
		_recent.layout_under (_transport.open_btn (), (float)_width, (float)_height, _dpi_scale);
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::open_recent_at (int index)
{
	_recent.set_visible (false);
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	if (_engine.open_recent (index)) {
		std::fprintf (stderr, "airdour_win_shell: recent open ok (index=%d)\n", index);
	} else {
		std::fprintf (stderr, "airdour_win_shell: recent open FAILED (index=%d)\n", index);
	}
	std::fflush (stderr);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_arm_from_engine ();
	sync_track_labels_from_engine ();
	sync_recent_from_engine ();
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::apply_recent_hit (RecentHit hit)
{
	switch (hit) {
	case RecentHit::Outside:
		_recent.set_visible (false);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	case RecentHit::Browse:
		_recent.set_visible (false);
		prompt_open_session ();
		return;
	case RecentHit::Clear:
		_engine.clear_recent_sessions (true);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_recent_from_engine ();
		_recent.layout_under (_transport.open_btn (), (float)_width, (float)_height, _dpi_scale);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	default: {
		int row = RecentPanel::row_index (hit);
		if (row >= 0) {
			open_recent_at (row);
		}
		return;
	}
	}
}

void
ShellWindow::maybe_show_recovery_prompt ()
{
	if (!_engine.recovery_offer_available ()) {
		return;
	}
	std::string path;
	std::string snap;
	if (!_engine.recovery_offer_path (&path, &snap)) {
		return;
	}
	std::wstring wpath;
	int n = MultiByteToWideChar (CP_UTF8, 0, path.c_str (), (int)path.size (), nullptr, 0);
	if (n > 0) {
		wpath.assign ((size_t)n, L'\0');
		MultiByteToWideChar (CP_UTF8, 0, path.c_str (), (int)path.size (), &wpath[0], n);
	}
	std::wstring label = L"session";
	size_t slash = path.find_last_of ("/\\");
	if (slash != std::string::npos && slash + 1 < path.size ()) {
		std::string base = path.substr (slash + 1);
		int nb = MultiByteToWideChar (CP_UTF8, 0, base.c_str (), (int)base.size (), nullptr, 0);
		if (nb > 0) {
			label.assign ((size_t)nb, L'\0');
			MultiByteToWideChar (CP_UTF8, 0, base.c_str (), (int)base.size (), &label[0], nb);
		}
	}
	_recovery.set_session_hint (label.c_str (), wpath.c_str ());
	_recovery.set_visible (true);
	_recovery.layout ((float)_width, (float)_height, _dpi_scale);
	_recent.set_visible (false);
	_prefs_visible = false;
	_prefs.set_visible (false);
	_help_visible = false;
	_about.set_about_visible (false);
	dismiss_splash ();
	std::fprintf (stderr, "airdour_win_shell: recovery prompt shown\n");
	std::fflush (stderr);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::apply_recovery_hit (RecoveryHit hit)
{
	if (hit == RecoveryHit::Restore) {
		_recovery.set_visible (false);
		if (_engine.restore_recovery_session ()) {
			std::fprintf (stderr, "airdour_win_shell: recovery restored\n");
		} else {
			std::fprintf (stderr, "airdour_win_shell: recovery restore FAILED\n");
		}
		std::fflush (stderr);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_arm_from_engine ();
		sync_track_labels_from_engine ();
		sync_recent_from_engine ();
		layout_panes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	if (hit == RecoveryHit::Discard) {
		_engine.discard_recovery_offer ();
		_recovery.set_visible (false);
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

int
ShellWindow::import_target_track () const
{
	/* Import lands on the selected region's track, else the focused lane. */
	int target = _engine_snap.selection_track;
	if (target < 0 || target >= n_audio_lanes ()) {
		target = _focus_audio_track;
	}
	if (target < 0 || target >= n_audio_lanes ()) {
		target = 0;
	}
	return target;
}

void
ShellWindow::prompt_import_audio ()
{	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}

	wchar_t path[MAX_PATH] = {};
	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof (ofn);
	ofn.hwndOwner = _hwnd;
	ofn.lpstrFilter = L"WAV audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0";
	ofn.lpstrFile = path;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
	ofn.lpstrTitle = L"Airdour — import audio";
	if (!GetOpenFileNameW (&ofn)) {
		return;
	}

	char utf8[MAX_PATH * 4];
	WideCharToMultiByte (CP_UTF8, 0, path, -1, utf8, sizeof (utf8), nullptr, nullptr);
	const int target = import_target_track ();
	if (_engine.import_audio_file (target, utf8)) {
		std::fprintf (stderr, "airdour_win_shell: import dialog ok (track %d)\n", target);
	} else {
		std::fprintf (stderr, "airdour_win_shell: import dialog failed\n");
	}
	std::fflush (stderr);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();
	refresh_automation_lane ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::tick_smoke_record (DWORD now)
{
	if (_smoke_record == SmokeRecordPhase::Off || _smoke_record == SmokeRecordPhase::Done) {
		return;
	}
	if (!_engine_snap.session_ok) {
		return;
	}

	if (_smoke_record == SmokeRecordPhase::NeedArm) {
		_engine.set_track_armed (0, true);
		_smoke_record_start_ms = now;
		_smoke_record = SmokeRecordPhase::ArmWait;
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_arm_from_engine ();
		std::fprintf (stderr, "airdour_win_shell: smoke-record track0 arm queued\n");
		std::fflush (stderr);
		return;
	}

	if (_smoke_record == SmokeRecordPhase::ArmWait
	    && (now - _smoke_record_start_ms) >= 250u) {
		_engine.set_session_record (true);
		_engine.transport_play ();
		_transport_playing = true;
		_transport.set_playing (true);
		_smoke_record_start_ms = now;
		_smoke_record = SmokeRecordPhase::Rolling;
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_arm_from_engine ();
		std::fprintf (stderr, "airdour_win_shell: smoke-record rolling (armed=%d session_rec=%d)\n",
		              _engine_snap.track_armed[0] ? 1 : 0,
		              _engine_snap.session_record_armed ? 1 : 0);
		std::fflush (stderr);
		return;
	}

	if (_smoke_record == SmokeRecordPhase::Rolling
	    && (now - _smoke_record_start_ms) >= 1200u) {
		transport_stop ();
		_smoke_record_start_ms = now;
		_smoke_record = SmokeRecordPhase::FinalizeWait;
		std::fprintf (stderr, "airdour_win_shell: smoke-record transport stopped — waiting finalize\n");
		std::fflush (stderr);
		return;
	}

	if (_smoke_record == SmokeRecordPhase::FinalizeWait
	    && (now - _smoke_record_start_ms) >= 400u) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_arm_from_engine ();
		_smoke_record = SmokeRecordPhase::Done;
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-record done — track0 regions=%d\n",
		              _engine_snap.track_has_regions[0] ? 1 : 0);
		std::fflush (stderr);
		/* Leave early so ExitProcess is less likely to race Butler after a take. */
		PostQuitMessage (0);
	}
}

void
ShellWindow::transport_play ()
{
	if (_engine.is_linked ()) {
		if (!_engine.start ()) {
			_engine_snap = _engine.snapshot ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return;
		}
		_engine.transport_play ();
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_arm_from_engine ();
	}
	_transport_playing = true;
	_transport.set_playing (true);
	_last_tick_ms = GetTickCount ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::transport_stop ()
{
	_transport_playing = false;
	_transport.set_playing (false);
	_playhead_sec = 0.f;
	if (_engine.is_linked ()) {
		_engine.transport_stop ();
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_arm_from_engine ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::transport_toggle ()
{
	if (_transport_playing) {
		transport_stop ();
	} else {
		transport_play ();
	}
}

void
ShellWindow::apply_shared_view (int source_lane)
{
	if (source_lane < 0 || source_lane >= kTracks) {
		return;
	}
	for (int i = 0; i < kTracks; ++i) {
		if (i != source_lane) {
			_lanes[i].strip.sync_view_from (_lanes[source_lane].strip);
		}
	}
}

void
ShellWindow::reset_shared_view ()
{
	for (int i = 0; i < kTracks; ++i) {
		_lanes[i].strip.reset_view ();
	}
}

void
ShellWindow::fit_shared_range (float start_sec, float end_sec)
{
	_lanes[0].strip.fit_range (start_sec, end_sec);
	apply_shared_view (0);
	refresh_editor_view_after_nav ();
}

void
ShellWindow::do_fit_session ()
{
	float s = 0.f, e = 0.f;
	if (_engine.is_linked () && _engine.session_extent_sec (&s, &e) && e > s) {
		fit_shared_range (s, e);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	/* Fallback: default 0..8s window. */
	fit_shared_range (0.f, WaveStrip::kDefaultSpanSec);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_fit_selection ()
{
	if (_engine_snap.has_selection
	    && _engine_snap.selection_end_sec > _engine_snap.selection_start_sec) {
		fit_shared_range (_engine_snap.selection_start_sec, _engine_snap.selection_end_sec);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	/* No selection → fit all regions (honest fallback). */
	do_fit_all_regions ();
}

void
ShellWindow::do_locate_selection_start ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok || !_engine_snap.has_selection) {
		return;
	}
	if (_engine.locate_to_selection_start ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_locate_selection_end ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok || !_engine_snap.has_selection) {
		return;
	}
	if (_engine.locate_to_selection_end ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_locate_next_marker ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (_engine.locate_next_marker ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_locate_prev_marker ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (_engine.locate_prev_marker ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_rename_marker_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	wchar_t name[32] = L"mark";
	for (int i = 0; i < _engine_snap.marker_count; ++i) {
		if (std::fabs (_engine_snap.markers[i].sec - _playhead_sec) < 0.35f) {
			wcsncpy_s (name, _engine_snap.markers[i].name, _TRUNCATE);
			break;
		}
	}
	if (!run_text_prompt (_hwnd, L"Rename marker — Airdour", name, 32)) {
		return;
	}
	char utf8[128];
	WideCharToMultiByte (CP_UTF8, 0, name, -1, utf8, sizeof (utf8), nullptr, nullptr);
	if (!_engine.rename_marker_near (_playhead_sec, utf8)) {
		MessageBoxW (_hwnd, L"No marker near playhead to rename.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_marker_lock_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.toggle_marker_locked_near (_playhead_sec)) {
		MessageBoxW (_hwnd, L"No marker near playhead to lock.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_marker_cd_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.toggle_marker_cd_near (_playhead_sec)) {
		MessageBoxW (_hwnd, L"No marker near playhead for CD flag.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_range_marker_from_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		MessageBoxW (_hwnd, L"Select a region first for a range marker.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
		return;
	}
	if (!_engine.add_range_marker_from_selection ()) {
		MessageBoxW (_hwnd, L"Could not create range marker from selection.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_region_sync_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	bool ok;
	if (GetKeyState (VK_SHIFT) & 0x8000) {
		ok = _engine.clear_selection_sync ();
		if (!ok) {
			MessageBoxW (_hwnd, L"Select a region to clear sync.",
			             L"Airdour", MB_OK | MB_ICONWARNING);
		}
	} else {
		ok = _engine.set_selection_sync_at_playhead ();
		if (!ok) {
			MessageBoxW (_hwnd,
			             L"Select a region and put the playhead inside it to set sync.",
			             L"Airdour", MB_OK | MB_ICONWARNING);
		}
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_scene_channel ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.cycle_scene_channel_near (_playhead_sec)) {
		MessageBoxW (_hwnd, L"No scene marker near playhead to cycle channel.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_next_cue ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_next_cue ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_prev_cue ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_prev_cue ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_rename_section_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	wchar_t name[32] = L"section";
	for (int i = 0; i < _engine_snap.marker_count; ++i) {
		if (_engine_snap.markers[i].section
		    && std::fabs (_engine_snap.markers[i].sec - _playhead_sec) < 0.35f) {
			wcsncpy_s (name, _engine_snap.markers[i].name, _TRUNCATE);
			break;
		}
	}
	if (!run_text_prompt (_hwnd, L"Rename section — Airdour", name, 32)) {
		return;
	}
	char utf8[128];
	WideCharToMultiByte (CP_UTF8, 0, name, -1, utf8, sizeof (utf8), nullptr, nullptr);
	if (!_engine.rename_section_near (_playhead_sec, utf8)) {
		MessageBoxW (_hwnd, L"No section marker near playhead to rename.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_punch_in ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_punch_in ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_punch_out ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_punch_out ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_session_start ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_session_start ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_session_end ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_session_end ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_loop_start ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_loop_start ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_locate_loop_end ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.locate_loop_end ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_zoom_to_punch ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (!_engine_snap.punch_range_ok
	    || _engine_snap.punch_end_sec <= _engine_snap.punch_start_sec) {
		return;
	}
	fit_shared_range (_engine_snap.punch_start_sec, _engine_snap.punch_end_sec);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_zoom_to_loop ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (!_engine_snap.loop_active
	    || _engine_snap.loop_end_sec <= _engine_snap.loop_start_sec) {
		return;
	}
	fit_shared_range (_engine_snap.loop_start_sec, _engine_snap.loop_end_sec);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_zoom_to_selection ()
{
	do_fit_selection ();
}

void
ShellWindow::do_nudge_playhead (int direction)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.nudge_playhead (direction)) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_playhead_sec = _engine_snap.transport_sec;
	sync_transport_clock_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_zoom_to_session ()
{
	do_fit_session ();
}

void
ShellWindow::do_zoom_to_regions ()
{
	do_fit_all_regions ();
}

void
ShellWindow::do_center_on_playhead ()
{
	float span = _lanes[0].strip.view_span_sec ();
	if (span < 0.05f) {
		span = WaveStrip::kDefaultSpanSec;
	}
	/* Shell playhead is UI source of truth (async Session locate may lag). */
	float ph = _playhead_sec;
	if (ph < 0.f) {
		ph = 0.f;
	}
	float start = ph - span * 0.5f;
	if (start < 0.f) {
		start = 0.f;
	}
	_lanes[0].strip.set_view_window (start, span);
	apply_shared_view (0);
	refresh_editor_view_after_nav ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_zoom_around_playhead (float factor)
{
	if (factor <= 0.f) {
		return;
	}
	float span = _lanes[0].strip.view_span_sec ();
	if (span < 0.05f) {
		span = WaveStrip::kDefaultSpanSec;
	}
	float start = _lanes[0].strip.view_start_sec ();
	float ph = _playhead_sec;
	if (ph < 0.f) {
		ph = 0.f;
	}
	float norm = (ph - start) / span;
	if (norm < 0.f) {
		norm = 0.f;
	}
	if (norm > 1.f) {
		norm = 1.f;
	}
	_lanes[0].strip.zoom_at (factor, norm);
	apply_shared_view (0);
	refresh_editor_view_after_nav ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_scroll_playhead_into_view ()
{
	float span = _lanes[0].strip.view_span_sec ();
	if (span < 0.05f) {
		span = WaveStrip::kDefaultSpanSec;
	}
	float start = _lanes[0].strip.view_start_sec ();
	float ph = _playhead_sec;
	if (ph < 0.f) {
		ph = 0.f;
	}
	float end = start + span;
	/* Already visible with a little margin — no-op. */
	if (ph >= start + span * 0.05f && ph <= end - span * 0.05f) {
		return;
	}
	float new_start;
	if (ph < start) {
		/* Scroll left: playhead at 25% into the window. */
		new_start = ph - span * 0.25f;
	} else {
		/* Scroll right: playhead at 75% into the window. */
		new_start = ph - span * 0.75f;
	}
	if (new_start < 0.f) {
		new_start = 0.f;
	}
	_lanes[0].strip.set_view_window (new_start, span);
	apply_shared_view (0);
	refresh_editor_view_after_nav ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_clear_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	_engine.clear_region_selection ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_fade_shape ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.cycle_selected_fade_shape ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_cycle_region_layer ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.cycle_selected_region_layer ()) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_region_selection (int direction)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.cycle_region_selection (direction)) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_duplicate_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.duplicate_selected_region ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_ensure_region_envelope ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.ensure_selected_region_envelope ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_loop_from_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.copy_selection_to_loop ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_transport_loop_from_engine ();
		apply_loop_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_lower_selection (bool to_bottom)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.lower_selected_region (to_bottom)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_nudge_region_envelope_mid (float delta_coeff)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.nudge_selected_region_envelope_mid (delta_coeff)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_nudge_selection (float delta_sec)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.nudge_selected_by_sec (delta_sec)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_nudge_selection_gain_db (float delta_db)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.nudge_selection_gain_db (delta_db)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_punch_from_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.copy_selection_to_punch ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_transport_punch_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_raise_selection (bool to_top)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.raise_selected_region (to_top)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_set_region_envelope_mid (float coeff_0_1)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.set_selected_region_envelope_mid (coeff_0_1)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_skip_range_from_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		MessageBoxW (_hwnd, L"Select a region first for a skip range.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
		return;
	}
	if (!_engine.add_skip_range_from_selection ()) {
		MessageBoxW (_hwnd, L"Could not create skip range from selection.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_fade_active ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.toggle_selected_fade_active ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_toggle_region_position_lock ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.toggle_selection_position_locked ()) {
		MessageBoxW (_hwnd, L"Select a region to position-lock.",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_selection_locked ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.toggle_selection_locked ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_toggle_selection_muted ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.toggle_selection_muted ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_toggle_selection_opaque ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.toggle_selection_opaque ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

bool
ShellWindow::envelope_mid_from_y (int lane, float y, float* out_coeff) const
{
	if (!out_coeff || lane < 0 || lane >= kTracks) {
		return false;
	}
	const D2D1_RECT_F& lb = _lanes[lane].bounds;
	float pad = 3.f * _dpi_scale;
	float h = lb.bottom - lb.top - 2.f * pad;
	if (h < 1.f) {
		return false;
	}
	float t = (lb.bottom - pad - y) / h;
	*out_coeff = std::max (0.02f, std::min (1.f, t));
	return true;
}

bool
ShellWindow::hit_region_envelope_mid (float x, float y, int* out_lane) const
{
	if (!_engine_snap.region_envelope_ok || !_engine_snap.has_selection
	    || _engine_snap.selection_end_sec <= _engine_snap.selection_start_sec) {
		return false;
	}
	int lane = _engine_snap.selection_track;
	if (lane < 0 || lane >= kTracks || lane >= n_audio_lanes ()) {
		return false;
	}
	const D2D1_RECT_F& lb = _lanes[lane].bounds;
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = _lanes[0].strip.view_span_sec ();
	if (span < 1e-6f) {
		return false;
	}
	float mid_sec = 0.5f * (_engine_snap.selection_start_sec + _engine_snap.selection_end_sec);
	float nx = (mid_sec - view0) / span;
	if (nx < 0.f || nx > 1.f) {
		return false;
	}
	float mid_gain = _engine.selected_region_envelope_mid ();
	if (mid_gain < 0.f) {
		return false;
	}
	float pad = 3.f * _dpi_scale;
	float lane_w = lb.right - lb.left;
	float lane_h = lb.bottom - lb.top;
	float px = lb.left + nx * lane_w;
	float py = lb.bottom - pad - mid_gain * (lane_h - 2.f * pad);
	float hit = 10.f * _dpi_scale;
	if (std::fabs (x - px) <= hit && std::fabs (y - py) <= hit) {
		if (out_lane) {
			*out_lane = lane;
		}
		return true;
	}
	return false;
}

void
ShellWindow::draw_region_envelope ()
{
	if (!_rt || !_brush_accent || !_engine.is_linked ()) {
		return;
	}
	if (!_engine_snap.region_envelope_ok || !_engine_snap.has_selection
	    || _engine_snap.selection_end_sec <= _engine_snap.selection_start_sec) {
		return;
	}
	int lane = _engine_snap.selection_track;
	if (lane < 0 || lane >= kTracks || lane >= n_audio_lanes ()) {
		return;
	}
	const D2D1_RECT_F& lb = _lanes[lane].bounds;
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = _lanes[0].strip.view_span_sec ();
	if (span < 1e-6f) {
		return;
	}
	float t0 = _engine_snap.selection_start_sec;
	float t1 = _engine_snap.selection_end_sec;
	float n0 = (t0 - view0) / span;
	float n1 = (t1 - view0) / span;
	if (n1 < 0.f || n0 > 1.f) {
		return;
	}
	n0 = std::max (0.f, std::min (1.f, n0));
	n1 = std::max (0.f, std::min (1.f, n1));
	if (n1 - n0 < 1e-4f) {
		return;
	}

	static constexpr size_t kEnvPts = 48;
	float y_0_1[kEnvPts];
	if (!_engine.fill_selected_region_envelope_curve (y_0_1, kEnvPts)) {
		return;
	}

	float pad = 3.f * _dpi_scale;
	float lane_w = lb.right - lb.left;
	float lane_h = lb.bottom - lb.top;
	float usable_h = lane_h - 2.f * pad;
	if (usable_h < 1.f) {
		return;
	}
	float x0 = lb.left + n0 * lane_w;
	float x1 = lb.left + n1 * lane_w;
	float w = x1 - x0;
	if (w < 2.f) {
		return;
	}

	ID2D1PathGeometry* geo = nullptr;
	ID2D1Factory* fac = nullptr;
	_rt->GetFactory (&fac);
	if (!fac || FAILED (fac->CreatePathGeometry (&geo)) || !geo) {
		if (fac) {
			fac->Release ();
		}
		return;
	}
	ID2D1GeometrySink* sink = nullptr;
	if (FAILED (geo->Open (&sink)) || !sink) {
		geo->Release ();
		if (fac) {
			fac->Release ();
		}
		return;
	}
	for (size_t i = 0; i < kEnvPts; ++i) {
		float frac = ((float)i + 0.5f) / (float)kEnvPts;
		float px = x0 + frac * w;
		float gy = std::max (0.f, std::min (1.f, y_0_1[i]));
		float py = lb.bottom - pad - gy * usable_h;
		if (i == 0) {
			sink->BeginFigure (D2D1::Point2F (px, py), D2D1_FIGURE_BEGIN_HOLLOW);
		} else {
			sink->AddLine (D2D1::Point2F (px, py));
		}
	}
	sink->EndFigure (D2D1_FIGURE_END_OPEN);
	sink->Close ();
	sink->Release ();

	_brush_accent->SetOpacity (0.85f);
	_rt->DrawGeometry (geo, _brush_accent, std::max (1.25f, 1.5f * _dpi_scale));
	_brush_accent->SetOpacity (1.f);
	geo->Release ();
	if (fac) {
		fac->Release ();
	}

	/* Mid tip cap — same geometry as hit_region_envelope_mid. */
	float mid_gain = _engine.selected_region_envelope_mid ();
	if (mid_gain < 0.f) {
		return;
	}
	float mid_sec = 0.5f * (t0 + t1);
	float nx = (mid_sec - view0) / span;
	if (nx < 0.f || nx > 1.f) {
		return;
	}
	float px = lb.left + nx * lane_w;
	float py = lb.bottom - pad - mid_gain * usable_h;
	float tip_r = std::max (3.f, 4.5f * _dpi_scale);
	_brush_accent->SetOpacity (0.95f);
	_rt->FillEllipse (D2D1::Ellipse (D2D1::Point2F (px, py), tip_r, tip_r), _brush_accent);
	if (_brush_pane) {
		_rt->DrawEllipse (D2D1::Ellipse (D2D1::Point2F (px, py), tip_r, tip_r),
		                  _brush_pane, std::max (1.f, 1.25f * _dpi_scale));
	}
	_brush_accent->SetOpacity (1.f);
}

bool
ShellWindow::smoke_envelope_mid_handle ()
{
	if (!_engine.is_linked ()) {
		std::fprintf (stderr, "airdour_win_shell: smoke-envelope-mid-handle FAILED (no engine)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (!_engine_snap.session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-envelope-mid-handle FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	if (!_engine.smoke_region_envelope_drag ()) {
		std::fprintf (stderr, "airdour_win_shell: smoke-envelope-mid-handle FAILED (envelope drag)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();

	static constexpr size_t kEnvPts = 32;
	float y_0_1[kEnvPts];
	if (!_engine.fill_selected_region_envelope_curve (y_0_1, kEnvPts)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-envelope-mid-handle FAILED (curve)\n");
		std::fflush (stderr);
		return false;
	}
	float mid_y = y_0_1[kEnvPts / 2];
	float edge_y = 0.5f * (y_0_1[0] + y_0_1[kEnvPts - 1]);
	if (!(mid_y + 0.05f < edge_y)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-envelope-mid-handle FAILED "
		              "(curve not dipped mid=%.3f edge=%.3f)\n",
		              mid_y, edge_y);
		std::fflush (stderr);
		return false;
	}

	/* Fit selection so mid handle is on-screen, then hit-test the drawn tip. */
	int lane = _engine_snap.selection_track;
	if (lane < 0 || lane >= kTracks) {
		std::fprintf (stderr, "airdour_win_shell: smoke-envelope-mid-handle FAILED (lane)\n");
		std::fflush (stderr);
		return false;
	}
	float t0 = _engine_snap.selection_start_sec;
	float t1 = _engine_snap.selection_end_sec;
	for (int i = 0; i < kTracks; ++i) {
		_lanes[i].strip.fit_range (t0, t1, 0.12f);
	}
	layout_panes ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();

	float mid_gain = _engine.selected_region_envelope_mid ();
	if (mid_gain < 0.f) {
		std::fprintf (stderr, "airdour_win_shell: smoke-envelope-mid-handle FAILED (mid gain)\n");
		std::fflush (stderr);
		return false;
	}
	const D2D1_RECT_F& lb = _lanes[lane].bounds;
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = _lanes[0].strip.view_span_sec ();
	float mid_sec = 0.5f * (t0 + t1);
	float nx = (mid_sec - view0) / span;
	float pad = 3.f * _dpi_scale;
	float px = lb.left + nx * (lb.right - lb.left);
	float py = lb.bottom - pad - mid_gain * ((lb.bottom - lb.top) - 2.f * pad);
	int hit_lane = -1;
	if (!hit_region_envelope_mid (px, py, &hit_lane) || hit_lane != lane) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-envelope-mid-handle FAILED "
		              "(hit miss at %.1f,%.1f lane=%d)\n",
		              px, py, hit_lane);
		std::fflush (stderr);
		return false;
	}

	std::fprintf (stderr,
	              "airdour_win_shell: smoke-envelope-mid-handle ok "
	              "(curve dip mid=%.2f edge=%.2f tip hit T%d)\n",
	              mid_y, edge_y, lane + 1);
	std::fflush (stderr);
	return true;
}

void
ShellWindow::prompt_export_selection_wav ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (!_engine_snap.has_selection) {
		std::fprintf (stderr, "airdour_win_shell: export selection — select a region first\n");
		std::fflush (stderr);
		return;
	}
	if (!_engine.start ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}

	wchar_t path[MAX_PATH] = L"airdour_export_selection.wav";
	OPENFILENAMEW ofn = {};
	ofn.lStructSize = sizeof (ofn);
	ofn.hwndOwner = _hwnd;
	ofn.lpstrFilter = L"WAV audio (*.wav)\0*.wav\0All files (*.*)\0*.*\0";
	ofn.lpstrFile = path;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
	ofn.lpstrTitle = L"Airdour — export selection WAV";
	ofn.lpstrDefExt = L"wav";
	if (!GetSaveFileNameW (&ofn)) {
		return;
	}

	char utf8[MAX_PATH * 4];
	WideCharToMultiByte (CP_UTF8, 0, path, -1, utf8, sizeof (utf8), nullptr, nullptr);
	if (_engine.export_selection_wav (utf8)) {
		std::fprintf (stderr, "airdour_win_shell: export selection dialog ok\n");
	} else {
		std::fprintf (stderr, "airdour_win_shell: export selection dialog failed\n");
	}
	std::fflush (stderr);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::prompt_rename_selected_region ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok
	    || !_engine_snap.has_selection) {
		return;
	}
	wchar_t name[EngineSnapshot::kRegionNameLen];
	if (_engine_snap.selection_region_name[0]) {
		wcsncpy_s (name, _engine_snap.selection_region_name, _TRUNCATE);
	} else {
		wcsncpy_s (name, L"Region", _TRUNCATE);
	}
	if (!run_text_prompt (_hwnd, L"Rename region — Airdour", name, EngineSnapshot::kRegionNameLen)) {
		return;
	}
	char utf8[128];
	WideCharToMultiByte (CP_UTF8, 0, name, -1, utf8, sizeof (utf8), nullptr, nullptr);
	if (!_engine.set_selected_region_name (utf8)) {
		MessageBoxW (_hwnd, L"Could not rename region (empty name or no selection).",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_fit_all_regions ()
{
	float s = 0.f, e = 0.f;
	if (_engine.is_linked () && _engine.regions_extent_sec (&s, &e) && e > s) {
		fit_shared_range (s, e);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	do_fit_session ();
}

void
ShellWindow::do_scroll_editor (float delta_widths)
{
	if (delta_widths == 0.f) {
		return;
	}
	_lanes[0].strip.pan_by (delta_widths);
	apply_shared_view (0);
	refresh_editor_view_after_nav ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::refresh_editor_view_after_nav ()
{
	/* Keep snap grid + playhead overlay consistent with the new window. */
	if (_engine.is_linked () && _engine_snap.session_ok && _engine_snap.snap_enabled) {
		_engine.refresh_grid_lines (
			_lanes[0].strip.view_start_sec (),
			_lanes[0].strip.view_span_sec ());
		_engine_snap = _engine.snapshot ();
	}
}

void
ShellWindow::apply_region_selection_to_lanes ()
{
	for (int i = 0; i < kTracks; ++i) {
		bool active = _engine_snap.has_selection && _engine_snap.selection_track == i;
		_lanes[i].strip.set_selection (
			active,
			_engine_snap.selection_start_sec,
			_engine_snap.selection_end_sec,
			active ? _engine_snap.selection_fade_in_sec : 0.f,
			active ? _engine_snap.selection_fade_out_sec : 0.f,
			active ? _engine_snap.selection_muted : false);
		_lanes[i].strip.set_sync_marker (
			active && _engine_snap.selection_sync_marked,
			_engine_snap.selection_sync_sec);
	}
}

bool
ShellWindow::smoke_region_sync_draw ()
{
	if (!_engine.is_linked ()) {
		std::fprintf (stderr, "airdour_win_shell: smoke-region-sync-draw FAILED (no engine)\n");
		return false;
	}
	if (!_engine.snapshot ().session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-region-sync-draw FAILED (no session)\n");
		return false;
	}
	/* Reuse API smoke to place a mid sync, then leave it marked for draw check. */
	if (!_engine.import_test_tone (0, 440.f, 1.2f)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-region-sync-draw FAILED (import)\n");
		return false;
	}
	if (!_engine.select_region_at (0, 0.3f)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-region-sync-draw FAILED (select)\n");
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	float s0 = _engine_snap.selection_start_sec;
	float e0 = _engine_snap.selection_end_sec;
	float mid = s0 + 0.40f;
	if (mid >= e0 - 0.05f) {
		std::fprintf (stderr, "airdour_win_shell: smoke-region-sync-draw FAILED (len)\n");
		return false;
	}
	if (!_engine.set_selection_sync_at (mid)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-region-sync-draw FAILED (set)\n");
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (!_engine_snap.selection_sync_marked
	    || std::fabs (_engine_snap.selection_sync_sec - mid) > 0.05f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-region-sync-draw FAILED "
		              "(sync=%.3f want=%.3f marked=%d)\n",
		              _engine_snap.selection_sync_sec, mid,
		              _engine_snap.selection_sync_marked ? 1 : 0);
		return false;
	}
	apply_region_selection_to_lanes ();
	do_fit_selection ();
	layout_panes ();
	const int lane = _engine_snap.selection_track;
	if (lane < 0 || lane >= kTracks) {
		std::fprintf (stderr, "airdour_win_shell: smoke-region-sync-draw FAILED (lane)\n");
		return false;
	}
	const D2D1_RECT_F& b = _lanes[lane].bounds;
	float sx = _lanes[lane].strip.x_at_time (b, _engine_snap.selection_sync_sec);
	float x0 = _lanes[lane].strip.x_at_time (b, _engine_snap.selection_start_sec);
	float x1 = _lanes[lane].strip.x_at_time (b, _engine_snap.selection_end_sec);
	if (sx < x0 - 1.f || sx > x1 + 1.f || sx < b.left - 1.f || sx > b.right + 1.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-region-sync-draw FAILED "
		              "(sx=%.1f region=[%.1f..%.1f] bounds=[%.1f..%.1f])\n",
		              sx, x0, x1, b.left, b.right);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-region-sync-draw ok "
	              "(sync=%.3fs sx=%.1f in [%.1f..%.1f])\n",
	              _engine_snap.selection_sync_sec, sx, x0, x1);
	return true;
}

void
ShellWindow::apply_loop_to_lanes ()
{
	for (int i = 0; i < kTracks; ++i) {
		_lanes[i].strip.set_loop_range (
			_engine_snap.loop_active,
			_engine_snap.loop_start_sec,
			_engine_snap.loop_end_sec);
	}
}

void
ShellWindow::do_toggle_loop ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_loop_play ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_transport_loop_from_engine ();
	apply_loop_to_lanes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_punch ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_punch_enabled ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_transport_punch_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_count_in ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_count_in ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_transport_tempo_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_click_gain ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.cycle_click_gain ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_transport_tempo_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_click ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_clicking ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_transport_tempo_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_add_audio_track ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	/* Shift+click +A → stereo 2in2out Equal-Power (pan width capable). */
	if (GetKeyState (VK_SHIFT) & 0x8000) {
		_engine.add_stereo_audio_track ();
	} else {
		_engine.add_audio_track ();
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_track_labels_from_engine ();
	sync_mixer_gains_from_engine ();
	sync_mixer_group_from_engine ();
	sync_mixer_arm_from_engine ();
	sync_mixer_mute_solo_from_engine ();
	sync_mixer_pan_from_engine ();
	sync_mixer_send_from_engine ();
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_add_midi_track ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.add_midi_track ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_track_labels_from_engine ();
	sync_mixer_mute_solo_from_engine ();
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_remove_track ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	/* Shift+−T removes MIDI when present; else selected/empty audio. */
	if ((GetKeyState (VK_SHIFT) & 0x8000)
	    && (_engine_snap.midi_track_ok || _engine_snap.midi_track_count > 0)) {
		_engine.remove_midi_track ();
	} else {
		_engine.remove_selected_or_empty_audio_track ();
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_track_labels_from_engine ();
	sync_mixer_gains_from_engine ();
	sync_mixer_arm_from_engine ();
	sync_mixer_mute_solo_from_engine ();
	sync_mixer_pan_from_engine ();
	sync_mixer_send_from_engine ();
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_move_audio_track (int delta)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	int idx = _focus_audio_track;
	if (_engine_snap.has_selection && _engine_snap.selection_track >= 0
	    && _engine_snap.selection_track < n_audio_lanes ()) {
		idx = _engine_snap.selection_track;
	}
	if (idx < 0 || idx >= n_audio_lanes ()) {
		idx = 0;
	}
	if (!_engine.move_audio_track (idx, delta)) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_focus_audio_track = idx + delta;
	if (_focus_audio_track < 0) {
		_focus_audio_track = 0;
	}
	if (_focus_audio_track >= n_audio_lanes ()) {
		_focus_audio_track = n_audio_lanes () - 1;
	}
	sync_track_labels_from_engine ();
	sync_mixer_gains_from_engine ();
	sync_mixer_arm_from_engine ();
	sync_mixer_mute_solo_from_engine ();
	sync_mixer_pan_from_engine ();
	sync_mixer_send_from_engine ();
	apply_region_selection_to_lanes ();
	refresh_automation_lane ();
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

namespace {

struct RenameDlgCtx {
	HWND    edit;
	wchar_t name[128];
	bool    done;
	bool    ok;
};

LRESULT CALLBACK
rename_wnd_proc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	RenameDlgCtx* ctx = reinterpret_cast<RenameDlgCtx*> (
		GetWindowLongPtrW (hwnd, GWLP_USERDATA));
	switch (msg) {
	case WM_CREATE: {
		CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*> (lparam);
		ctx = reinterpret_cast<RenameDlgCtx*> (cs->lpCreateParams);
		SetWindowLongPtrW (hwnd, GWLP_USERDATA, (LONG_PTR)ctx);
		ctx->edit = CreateWindowExW (
			WS_EX_CLIENTEDGE, L"EDIT", ctx->name,
			WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
			12, 12, 260, 24, hwnd, (HMENU)1001, cs->hInstance, nullptr);
		CreateWindowW (L"BUTTON", L"OK",
		               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
		               80, 48, 72, 26, hwnd, (HMENU)IDOK, cs->hInstance, nullptr);
		CreateWindowW (L"BUTTON", L"Cancel",
		               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		               164, 48, 72, 26, hwnd, (HMENU)IDCANCEL, cs->hInstance, nullptr);
		if (ctx->edit) {
			SendMessageW (ctx->edit, EM_SETSEL, 0, -1);
			SetFocus (ctx->edit);
		}
		return 0;
	}
	case WM_COMMAND:
		if (LOWORD (wparam) == IDOK && ctx) {
			GetWindowTextW (ctx->edit, ctx->name, 128);
			ctx->ok = (ctx->name[0] != L'\0');
			ctx->done = true;
			DestroyWindow (hwnd);
			return 0;
		}
		if (LOWORD (wparam) == IDCANCEL && ctx) {
			ctx->ok = false;
			ctx->done = true;
			DestroyWindow (hwnd);
			return 0;
		}
		break;
	case WM_CLOSE:
		if (ctx) {
			ctx->ok = false;
			ctx->done = true;
		}
		DestroyWindow (hwnd);
		return 0;
	default:
		break;
	}
	return DefWindowProcW (hwnd, msg, wparam, lparam);
}

bool
run_text_prompt (HWND parent, const wchar_t* title, wchar_t* name, size_t name_chars)
{
	if (!name || name_chars < 2) {
		return false;
	}
	static const wchar_t* kCls = L"AirdourRenameDlg";
	static bool registered = false;
	HINSTANCE inst = GetModuleHandleW (nullptr);
	if (!registered) {
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof (wc);
		wc.lpfnWndProc = rename_wnd_proc;
		wc.hInstance = inst;
		wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
		wc.lpszClassName = kCls;
		if (!RegisterClassExW (&wc) && GetLastError () != ERROR_CLASS_ALREADY_EXISTS) {
			return false;
		}
		registered = true;
	}

	RenameDlgCtx ctx = {};
	wcsncpy_s (ctx.name, name, _TRUNCATE);
	ctx.done = false;
	ctx.ok = false;

	RECT pr = {};
	if (parent) {
		GetWindowRect (parent, &pr);
	}
	int wx = pr.left + 80;
	int wy = pr.top + 120;
	HWND dlg = CreateWindowExW (
		WS_EX_DLGMODALFRAME, kCls, title ? title : L"Airdour",
		WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
		wx, wy, 300, 120, parent, nullptr, inst, &ctx);
	if (!dlg) {
		return false;
	}
	if (parent) {
		EnableWindow (parent, FALSE);
	}
	MSG msg;
	while (!ctx.done && GetMessageW (&msg, nullptr, 0, 0) > 0) {
		if (!IsDialogMessageW (dlg, &msg)) {
			TranslateMessage (&msg);
			DispatchMessageW (&msg);
		}
	}
	if (parent) {
		EnableWindow (parent, TRUE);
		SetForegroundWindow (parent);
	}
	if (!ctx.ok) {
		return false;
	}
	wcsncpy_s (name, name_chars, ctx.name, _TRUNCATE);
	return name[0] != L'\0';
}

/* Folder picker: modern IFileOpenDialog, classic SHBrowseForFolder fallback. */
bool
pick_folder (HWND owner, const wchar_t* title, std::wstring* out)
{
	if (!out) {
		return false;
	}
	IFileOpenDialog* dlg = nullptr;
	HRESULT hr = CoCreateInstance (CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
	                               IID_PPV_ARGS (&dlg));
	if (FAILED (hr) || !dlg) {
		BROWSEINFOW bi = {};
		bi.hwndOwner = owner;
		bi.lpszTitle = title;
		bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
		LPITEMIDLIST pidl = SHBrowseForFolderW (&bi);
		if (!pidl) {
			return false;
		}
		wchar_t path[MAX_PATH] = {};
		const bool ok = SHGetPathFromIDListW (pidl, path) != FALSE;
		CoTaskMemFree (pidl);
		if (ok) {
			*out = path;
		}
		return ok;
	}

	DWORD opts = 0;
	dlg->GetOptions (&opts);
	dlg->SetOptions (opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
	dlg->SetTitle (title);
	bool ok = false;
	if (SUCCEEDED (dlg->Show (owner))) {
		IShellItem* item = nullptr;
		if (SUCCEEDED (dlg->GetResult (&item)) && item) {
			PWSTR psz = nullptr;
			if (SUCCEEDED (item->GetDisplayName (SIGDN_FILESYSPATH, &psz)) && psz) {
				*out = psz;
				CoTaskMemFree (psz);
				ok = true;
			}
			item->Release ();
		}
	}
	dlg->Release ();
	return ok;
}

std::wstring
utf8_to_wstring (const std::string& s)
{
	if (s.empty ()) {
		return std::wstring ();
	}
	const int n = MultiByteToWideChar (CP_UTF8, 0, s.c_str (), (int)s.size (), nullptr, 0);
	if (n <= 0) {
		return std::wstring ();
	}
	std::wstring out ((size_t)n, L'\0');
	MultiByteToWideChar (CP_UTF8, 0, s.c_str (), (int)s.size (), &out[0], n);
	return out;
}

std::string
wstring_to_utf8 (const std::wstring& s)
{
	if (s.empty ()) {
		return std::string ();
	}
	const int n = WideCharToMultiByte (CP_UTF8, 0, s.c_str (), (int)s.size (), nullptr, 0, nullptr, nullptr);
	if (n <= 0) {
		return std::string ();
	}
	std::string out ((size_t)n, '\0');
	WideCharToMultiByte (CP_UTF8, 0, s.c_str (), (int)s.size (), &out[0], n, nullptr, nullptr);
	return out;
}

} // namespace

void
ShellWindow::prompt_rename_track (int audio_index, bool midi)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	wchar_t name[32];
	if (midi) {
		if (_engine_snap.midi_track_name[0]) {
			wcsncpy_s (name, _engine_snap.midi_track_name, _TRUNCATE);
		} else {
			wcsncpy_s (name, L"MIDI 1", _TRUNCATE);
		}
	} else {
		if (audio_index < 0 || audio_index >= n_audio_lanes ()) {
			return;
		}
		if (_engine_snap.track_name[audio_index][0]) {
			wcsncpy_s (name, _engine_snap.track_name[audio_index], _TRUNCATE);
		} else {
			_snwprintf_s (name, _TRUNCATE, L"Audio %d", audio_index + 1);
		}
	}
	if (!run_text_prompt (_hwnd, L"Rename track — Airdour", name, 32)) {
		return;
	}
	char utf8[128];
	WideCharToMultiByte (CP_UTF8, 0, name, -1, utf8, sizeof (utf8), nullptr, nullptr);
	bool ok = false;
	if (midi) {
		ok = _engine.set_midi_track_name (utf8);
	} else {
		ok = _engine.set_track_name (audio_index, utf8);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_track_labels_from_engine ();
	if (!ok) {
		MessageBoxW (_hwnd, L"Could not rename track (empty name or rec-armed).",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_rename_focused_track ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	int idx = _focus_audio_track;
	if (_engine_snap.has_selection && _engine_snap.selection_track >= 0) {
		idx = _engine_snap.selection_track;
	}
	if (idx < 0 || idx >= n_audio_lanes ()) {
		idx = 0;
	}
	prompt_rename_track (idx, false);
}

void
ShellWindow::do_toggle_group_pick (int audio_index)
{
	if (audio_index < 0 || audio_index >= kTracksMax || audio_index >= n_audio_lanes ()) {
		return;
	}
	_group_pick[audio_index] = !_group_pick[audio_index];
	_focus_audio_track = audio_index;
	sync_mixer_group_from_engine ();
	refresh_automation_lane ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::sync_monitor_from_engine ()
{
	_monitor.set_available (_engine_snap.monitor_ok);
	_monitor.set_dim (_engine_snap.monitor_dim);
	_monitor.set_dim_db (_engine_snap.monitor_dim_db);
	_monitor.set_mono (_engine_snap.monitor_mono);
	_monitor.set_mute (_engine_snap.monitor_cut);
	_monitor.set_solo_boost_db (_engine_snap.monitor_solo_boost_db);
	_monitor.set_aux_ok (_engine_snap.aux_bus_ok);
	_monitor.set_aux_return (_engine_snap.aux_return);
}

void
ShellWindow::do_set_aux_return (float gain_0_1)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.set_aux_return (gain_0_1);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_monitor_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
	_monitor.set_master_lim_ok (_engine_snap.master_fx_ok);
	_monitor.set_master_lim_bypassed (_engine_snap.master_fx_bypassed);
}

void
ShellWindow::do_toggle_monitor_dim ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_monitor_dim ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_monitor_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_monitor_mono ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_monitor_mono ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_monitor_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_monitor_cut ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_monitor_cut ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_monitor_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_clear_all_solos ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (_engine.clear_all_solos ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_mute_solo_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_toggle_master_fx ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.toggle_master_fx_bypass ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_monitor_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_monitor_solo_boost ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.cycle_monitor_solo_boost ();
	_engine.toggle_master_fx_bypass ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_monitor_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_clear_all_mutes ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (_engine.clear_all_mutes ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_mute_solo_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_scroll_mixer (float delta_px)
{
	if (std::fabs (delta_px) < 0.01f) {
		return;
	}
	_mixer_scroll_x += delta_px;
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

bool
ShellWindow::smoke_selection_marquee_tint ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-selection-marquee-tint FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	/* Floors: wash > prior 0.28, edge > wash, muted still below active. */
	if (!(WaveStrip::kSelWashOp > 0.35f
	      && WaveStrip::kSelEdgeOp > WaveStrip::kSelWashOp
	      && WaveStrip::kSelEdgeOp > 0.90f
	      && WaveStrip::kSelMutedWashOp < WaveStrip::kSelWashOp
	      && WaveStrip::kSelMutedEdgeOp < WaveStrip::kSelEdgeOp
	      && WaveStrip::kSelFadeWashOp > 0.50f
	      && kSelection.r < kSelection.g && kSelection.b > 0.90f)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-selection-marquee-tint FAILED "
		              "(wash=%.2f edge=%.2f muted=%.2f/%.2f fade=%.2f)\n",
		              WaveStrip::kSelWashOp, WaveStrip::kSelEdgeOp,
		              WaveStrip::kSelMutedWashOp, WaveStrip::kSelMutedEdgeOp,
		              WaveStrip::kSelFadeWashOp);
		std::fflush (stderr);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-selection-marquee-tint ok "
	              "(wash=%.2f edge=%.2f cyan)\n",
	              WaveStrip::kSelWashOp, WaveStrip::kSelEdgeOp);
	std::fflush (stderr);
	return true;
}


bool
ShellWindow::smoke_selection_mute_wash ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-selection-mute-wash FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	/* Mute cue: wash well below active, edge still strong, hatch present. */
	if (!(WaveStrip::kSelMutedWashOp < 0.16f
	      && WaveStrip::kSelMutedWashOp < WaveStrip::kSelWashOp * 0.45f
	      && WaveStrip::kSelMutedEdgeOp > 0.65f
	      && WaveStrip::kSelMutedEdgeOp > WaveStrip::kSelMutedWashOp
	      && WaveStrip::kSelMutedHatchOp > 0.30f
	      && WaveStrip::kSelMutedHatchPx >= 4.f
	      && WaveStrip::kSelMutedHatchPx <= 8.f)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-selection-mute-wash FAILED "
		              "(wash=%.2f edge=%.2f hatch=%.2f/%.1fpx)\n",
		              WaveStrip::kSelMutedWashOp, WaveStrip::kSelMutedEdgeOp,
		              WaveStrip::kSelMutedHatchOp, WaveStrip::kSelMutedHatchPx);
		std::fflush (stderr);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-selection-mute-wash ok "
	              "(wash=%.2f edge=%.2f hatch=%.2f @%.0fpx)\n",
	              WaveStrip::kSelMutedWashOp, WaveStrip::kSelMutedEdgeOp,
	              WaveStrip::kSelMutedHatchOp, WaveStrip::kSelMutedHatchPx);
	std::fflush (stderr);
	return true;
}


bool
ShellWindow::smoke_midi_outside_ranges_wash ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-outside-ranges-wash FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	/* In-union wash well below idle note opacity so outside selection pops. */
	if (!(kMidiOutsideRangesInWashOp < 0.40f
	      && kMidiOutsideRangesInWashOp < kMidiNoteIdleOp * 0.50f
	      && kMidiNoteIdleOp < kMidiNoteSelOp
	      && kMidiNoteSelOp > 0.85f)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-outside-ranges-wash FAILED "
		              "(wash=%.2f idle=%.2f sel=%.2f)\n",
		              kMidiOutsideRangesInWashOp, kMidiNoteIdleOp, kMidiNoteSelOp);
		std::fflush (stderr);
		return false;
	}
	if (!_engine.smoke_midi_select_outside_ranges ()) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-outside-ranges-wash FAILED (select)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	int sel_out = 0;
	int idle_in = 0;
	for (int i = 0; i < _engine_snap.midi_note_count; ++i) {
		const EngineMidiNoteSnap& n = _engine_snap.midi_notes[i];
		float n1 = n.start_sec + n.length_sec;
		bool in_loop = (n.start_sec < _engine_snap.loop_end_sec
		                && n1 > _engine_snap.loop_start_sec);
		bool in_punch = (n.start_sec < _engine_snap.punch_end_sec
		                 && n1 > _engine_snap.punch_start_sec);
		bool in_union = in_loop || in_punch;
		if (n.selected && !in_union) {
			++sel_out;
		}
		if (!n.selected && in_union) {
			++idle_in;
		}
	}
	if (sel_out < 1 || idle_in < 1) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-outside-ranges-wash FAILED "
		              "(sel_out=%d idle_in=%d)\n",
		              sel_out, idle_in);
		std::fflush (stderr);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-midi-outside-ranges-wash ok "
	              "(wash=%.2f · sel_out=%d idle_in=%d)\n",
	              kMidiOutsideRangesInWashOp, sel_out, idle_in);
	std::fflush (stderr);
	return true;
}


bool
ShellWindow::smoke_mixer_scroll_layout ()
{
	/* Force overflow with wide min strip; scroll to end; strip 0 must leave clip. */
	_mixer_force_min_w = 140.f;
	_mixer_scroll_x = 0.f;
	layout_panes ();
	if (_mixer_scroll_max < 8.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-mixer-scroll FAILED (max=%.1f need overflow)\n",
		              _mixer_scroll_max);
		_mixer_force_min_w = 0.f;
		layout_panes ();
		return false;
	}
	float left0 = _mixers[0].bounds ().left;
	_mixer_scroll_x = _mixer_scroll_max;
	layout_panes ();
	float left1 = _mixers[0].bounds ().left;
	if (left1 >= left0 - 4.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-mixer-scroll FAILED (scroll no move %.1f→%.1f)\n",
		              left0, left1);
		_mixer_force_min_w = 0.f;
		_mixer_scroll_x = 0.f;
		layout_panes ();
		return false;
	}
	if (left1 > _mixer_strips_clip.left - 2.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-mixer-scroll FAILED (strip0 still in clip %.1f)\n",
		              left1);
		_mixer_force_min_w = 0.f;
		_mixer_scroll_x = 0.f;
		layout_panes ();
		return false;
	}
	_mixer_scroll_x = 0.f;
	_mixer_force_min_w = 0.f;
	layout_panes ();
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-mixer-scroll ok (max=%.0f px, shifted %.0f)\n",
	              _mixer_scroll_max, left0 - left1);
	return true;
}

bool
ShellWindow::smoke_meter_clip_led ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-meter-clip-led FAILED (no session)\n");
		return false;
	}
	layout_panes ();
	_mixers[0].set_meter_peak (1.f, true);
	if (!_mixers[0].meter_clipped ()) {
		std::fprintf (stderr, "airdour_win_shell: smoke-meter-clip-led FAILED (clip latch)\n");
		return false;
	}
	const D2D1_RECT_F& mb = _mixers[0].meter ().bounds ();
	float mid_x = 0.5f * (mb.left + mb.right);
	float body_y = mb.top + 0.55f * (mb.bottom - mb.top);
	float led_y = mb.top + 1.f * _dpi_scale;
	if (_mixers[0].hit_meter_clip_led (mid_x, body_y)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-meter-clip-led FAILED (body hit LED)\n");
		return false;
	}
	if (!_mixers[0].hit_meter (mid_x, body_y)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-meter-clip-led FAILED (body miss meter)\n");
		return false;
	}
	/* Body click must not clear (handler requires LED hit). */
	if (!_mixers[0].meter_clipped ()) {
		std::fprintf (stderr, "airdour_win_shell: smoke-meter-clip-led FAILED (preclear)\n");
		return false;
	}
	if (!_mixers[0].hit_meter_clip_led (mid_x, led_y)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-meter-clip-led FAILED (LED miss)\n");
		return false;
	}
	_mixers[0].clear_meter_clip ();
	if (_mixers[0].meter_clipped ()) {
		std::fprintf (stderr, "airdour_win_shell: smoke-meter-clip-led FAILED (still clipped)\n");
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-meter-clip-led ok "
	              "(body keeps latch; LED clears)\n");
	return true;
}

void
ShellWindow::do_apply_or_clear_group ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	/* Shift+Grp clears; else build from picks (≥2) or focus+neighbour. */
	if (GetKeyState (VK_SHIFT) & 0x8000) {
		_engine.clear_route_group ();
		for (int i = 0; i < kTracksMax; ++i) {
			_group_pick[i] = false;
		}
	} else {
		int idxs[EngineSnapshot::kMeters];
		int n = 0;
		for (int i = 0; i < n_audio_lanes () && i < EngineSnapshot::kMeters; ++i) {
			if (_group_pick[i]) {
				idxs[n++] = i;
			}
		}
		if (n < 2) {
			int a = _focus_audio_track;
			if (a < 0 || a >= n_audio_lanes ()) {
				a = 0;
			}
			int b = a + 1;
			if (b >= n_audio_lanes ()) {
				b = a - 1;
			}
			if (b < 0 || b == a) {
				_engine_snap = _engine.snapshot ();
				InvalidateRect (_hwnd, nullptr, FALSE);
				return;
			}
			idxs[0] = a;
			idxs[1] = b;
			n = 2;
		}
		_engine.create_route_group (idxs, n);
		for (int i = 0; i < kTracksMax; ++i) {
			_group_pick[i] = false;
		}
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_group_from_engine ();
	sync_mixer_gains_from_engine ();
	sync_mixer_mute_solo_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_group_relative ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (_engine_snap.group_member_count < 2) {
		return;
	}
	_engine.toggle_route_group_relative ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_group_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_vca_assign ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.ensure_vca_master ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	const bool on = _engine_snap.vca_slave_count < 1;
	_engine.assign_track_to_vca (0, on);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_focused_track_active ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	int track = _focus_audio_track;
	if (track < 0 || track >= n_audio_lanes ()) {
		track = 0;
	}
	_engine.toggle_track_active (track);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_mute_solo_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_group_color ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (_engine_snap.group_member_count < 2) {
		return;
	}
	_engine.cycle_route_group_color ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_group_from_engine ();
	if (!_engine.ensure_vca_master ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	const bool on = _engine_snap.vca_slave_count < 1;
	_engine.assign_track_to_vca (0, on);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_nudge_tempo (float delta)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (GetKeyState (VK_SHIFT) & 0x8000) {
		delta *= 5.f;
	}
	_engine.nudge_tempo_bpm (delta);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_transport_tempo_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}


void
ShellWindow::do_toggle_comp_sidechain ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.toggle_track_comp_sidechain (1, 0)) {
		MessageBoxW (_hwnd, L"Could not toggle Comp sidechain (need ACE Comp on Audio 2).",
		             L"Airdour", MB_OK | MB_ICONWARNING);
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_fx_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::format_marker_ruler_label (const wchar_t* name, wchar_t* out, size_t out_len,
                                        int max_chars)
{
	if (!out || out_len == 0) {
		return;
	}
	out[0] = L'\0';
	if (!name || !name[0] || max_chars < 2 || out_len < 2) {
		return;
	}
	const size_t n = wcslen (name);
	const size_t cap = out_len - 1;
	if ((int)n <= max_chars) {
		wcsncpy_s (out, out_len, name, _TRUNCATE);
		return;
	}
	/* Keep max_chars-1 glyphs + ellipsis (… U+2026). */
	const int keep = max_chars - 1;
	if ((size_t)keep + 1 > cap) {
		wcsncpy_s (out, out_len, name, _TRUNCATE);
		return;
	}
	for (int i = 0; i < keep; ++i) {
		out[i] = name[i];
	}
	out[keep] = L'\x2026';
	out[keep + 1] = L'\0';
}

bool
ShellWindow::smoke_marker_name_ellipsis ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-marker-name-ellipsis FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	wchar_t short_lab[32];
	wchar_t long_lab[32];
	format_marker_ruler_label (L"Intro", short_lab, 32, 8);
	format_marker_ruler_label (L"VeryLongMarkerNameXYZ", long_lab, 32, 8);
	if (wcscmp (short_lab, L"Intro") != 0) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-marker-name-ellipsis FAILED (short=%ls)\n",
		              short_lab);
		std::fflush (stderr);
		return false;
	}
	if (wcslen (long_lab) != 8 || long_lab[7] != L'\x2026'
	    || wcsncmp (long_lab, L"VeryLon", 7) != 0) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-marker-name-ellipsis FAILED (long=%ls)\n",
		              long_lab);
		std::fflush (stderr);
		return false;
	}

	if (!_engine.add_marker_at (1.0f, "Short")
	    || !_engine.add_marker_at (2.0f, "VeryLongMarkerNameXYZ")) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-marker-name-ellipsis FAILED (add)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	bool short_ok = false;
	bool long_ok = false;
	for (int i = 0; i < _engine_snap.marker_count; ++i) {
		wchar_t lab[32];
		format_marker_ruler_label (_engine_snap.markers[i].name, lab, 32, 8);
		if (std::fabs (_engine_snap.markers[i].sec - 1.0f) < 0.05f
		    && wcscmp (lab, L"Short") == 0) {
			short_ok = true;
		}
		if (std::fabs (_engine_snap.markers[i].sec - 2.0f) < 0.05f
		    && wcslen (lab) == 8 && lab[7] == L'\x2026') {
			long_ok = true;
		}
	}
	(void)_engine.remove_marker_near (1.0f, 0.35f);
	(void)_engine.remove_marker_near (2.0f, 0.35f);
	if (!short_ok || !long_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-marker-name-ellipsis FAILED "
		              "(snap short=%d long=%d)\n",
		              short_ok ? 1 : 0, long_ok ? 1 : 0);
		std::fflush (stderr);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-marker-name-ellipsis ok "
	              "(Short · VeryLon…)\n");
	std::fflush (stderr);
	return true;
}

void
ShellWindow::do_add_marker_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.add_marker_at (_playhead_sec);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_remove_marker_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.remove_marker_near (_playhead_sec);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::sync_history_from_engine ()
{
	_history.set_available (_engine_snap.session_ok);
	_history.set_depths (_engine_snap.undo_depth, _engine_snap.redo_depth);
	_history.set_next_names (_engine_snap.next_undo_name, _engine_snap.next_redo_name);
	_history.set_undo_rows (_engine_snap.undo_list_count, _engine_snap.undo_list_name);
}

void
ShellWindow::do_undo ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.undo (1);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();
	sync_history_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_redo ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	_engine.redo (1);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();
	sync_history_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_undo_to_index (int index)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine.undo_to_index (index)) {
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();
	sync_mixer_gains_from_engine ();
	sync_history_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

bool
ShellWindow::do_clear_active_midi_cc_lane ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		return false;
	}
	bool ok = false;
	if (_midi_cc_number == -1) {
		ok = _engine.clear_midi_bend_automation ();
	} else if (_midi_cc_number == -2) {
		ok = _engine.clear_midi_pressure_automation ();
	} else if (_midi_cc_number == -3) {
		ok = _engine.clear_midi_note_pressure_automation ();
	} else if (_midi_cc_number >= 0) {
		ok = _engine.clear_midi_cc_automation (_midi_cc_number);
	}
	if (ok) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		refresh_midi_cc_lane ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
	return ok;
}

void
ShellWindow::do_delete_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	/* Del over the CC lane clears that lane's automation first. */
	POINT pt;
	if (GetCursorPos (&pt) && ScreenToClient (_hwnd, &pt)
	    && _midi_cc_lane.hit (_midi_cc_lane_bounds, (float)pt.x, (float)pt.y)) {
		if (do_clear_active_midi_cc_lane ()) {
			return;
		}
	}
	/* Prefer MIDI note selection when present. */
	if (_engine_snap.midi_selected_note_id >= 0) {
		if (_engine.delete_selected_midi_note ()) {
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return;
		}
	}
	if (!_engine.delete_selected_region ()) {
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	apply_region_selection_to_lanes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

bool
ShellWindow::smoke_midi_cc_clear ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc-clear FAILED (no session)\n");
		return false;
	}
	if (!_engine.ensure_midi_track () || !_engine.ensure_midi_region (16.f)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc-clear FAILED (region)\n");
		return false;
	}
	_midi_cc_number = 7;
	if (!_engine.clear_midi_cc_automation (7)
	    || !_engine.write_midi_cc_automation (0.0f, 0.25f, 7)
	    || !_engine.write_midi_cc_automation (1.5f, 0.80f, 7)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc-clear FAILED (write)\n");
		return false;
	}
	float xn[8], yn[8];
	int n = _engine.fill_midi_cc_automation_points (0.f, 8.f, xn, yn, 8, 7);
	if (n < 2) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc-clear FAILED (pts=%d)\n", n);
		return false;
	}
	if (!do_clear_active_midi_cc_lane ()) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc-clear FAILED (clear)\n");
		return false;
	}
	n = _engine.fill_midi_cc_automation_points (0.f, 8.f, xn, yn, 8, 7);
	if (n != 0) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc-clear FAILED (after=%d)\n", n);
		return false;
	}
	/* Also clear bend via the same shell path. */
	_midi_cc_number = -1;
	if (!_engine.write_midi_bend_automation (0.5f, 0.75f)
	    || !do_clear_active_midi_cc_lane ()) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc-clear FAILED (bend)\n");
		return false;
	}
	n = _engine.fill_midi_bend_automation_points (0.f, 8.f, xn, yn, 8);
	if (n != 0) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc-clear FAILED (bend after=%d)\n", n);
		return false;
	}
	_midi_cc_number = 7;
	refresh_midi_cc_lane ();
	std::fprintf (stderr, "airdour_win_shell: smoke-midi-cc-clear ok (CC7+bend cleared)\n");
	return true;
}

void
ShellWindow::do_split_at_playhead ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	float t = _playhead_sec;
	if (_engine_snap.transport_rolling) {
		t = _engine_snap.transport_sec;
	}
	int track = _engine_snap.has_selection ? _engine_snap.selection_track : 0;
	if (_engine.split_at (t, track)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_apply_crossfade ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.apply_selected_crossfade (0.08f)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_stretch_selection (float time_ratio)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.stretch_selected (time_ratio)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_pitch_shift_selection (float semitones)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.pitch_shift_selected (semitones)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_reverse_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.reverse_selected ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_normalize_selection (float target_dbfs)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.normalize_selected (target_dbfs)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_strip_silence_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.strip_silence_selected ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_consolidate_selection ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (!_engine_snap.has_selection) {
		return;
	}
	if (_engine.consolidate_selected ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::sync_transport_freeze_from_engine ()
{
	int track = _engine_snap.has_selection ? _engine_snap.selection_track
	                                       : _focus_audio_track;
	bool frozen = false;
	if (track >= 0 && track < EngineSnapshot::kMeters) {
		frozen = _engine_snap.track_frozen[track];
	}
	_transport.set_freeze_active (frozen);
}

void
ShellWindow::do_toggle_freeze_track ()
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	int track = _engine_snap.has_selection ? _engine_snap.selection_track
	                                       : _focus_audio_track;
	if (track < 0) {
		track = 0;
	}
	if (_engine.toggle_freeze_track (track)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		sync_transport_freeze_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

void
ShellWindow::do_toggle_snap ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.toggle_snap ();
	_engine_snap = _engine.snapshot ();
	_transport.set_snap_enabled (_engine_snap.snap_enabled);
	_transport.set_snap_density (_engine_snap.snap_density);
	_transport.set_midi_step_enabled (_engine_snap.midi_step_enabled);
	_transport.set_midi_chord_quality (_engine_snap.midi_chord_quality);
	if (_engine_snap.snap_enabled) {
		_engine.refresh_grid_lines (
			_lanes[0].strip.view_start_sec (),
			_lanes[0].strip.view_span_sec ());
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

bool
ShellWindow::smoke_midi_vel_chord ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-vel-chord FAILED (no session)\n");
		return false;
	}
	if (!_engine.ensure_midi_track () || !_engine.ensure_midi_region (16.f)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-vel-chord FAILED (track/region)\n");
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	/* Clear existing notes one-by-one so the chord is alone. */
	for (int guard = 0; guard < 64 && _engine_snap.midi_note_count > 0; ++guard) {
		const EngineMidiNoteSnap& n = _engine_snap.midi_notes[0];
		_engine.select_midi_note_at (n.start_sec + n.length_sec * 0.5f, n.pitch);
		if (!_engine.delete_selected_midi_note ()) {
			break;
		}
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	if (!_engine.add_midi_note (60, 0.0f, 0.5f, 40)
	    || !_engine.add_midi_note (64, 0.0f, 0.5f, 110)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-vel-chord FAILED (add)\n");
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (_engine_snap.midi_note_count < 2) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-vel-chord FAILED (count=%d)\n",
		              _engine_snap.midi_note_count);
		return false;
	}
	layout_panes ();
	const D2D1_RECT_F& lane = _midi_lane;
	const D2D1_RECT_F vel = midi_velocity_rect ();
	float vh = vel.bottom - vel.top;
	if (vh < 8.f || lane.right <= lane.left) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-vel-chord FAILED (layout)\n");
		return false;
	}
	float cx = _lanes[0].strip.x_at_time (lane, 0.25f);
	float quiet_h = vh * (40.f / 127.f);
	float loud_h = vh * (110.f / 127.f);
	float y_loud_only = vel.bottom - (quiet_h + loud_h) * 0.5f;
	float y_inner = vel.bottom - quiet_h * 0.5f;

	int i_loud = pick_midi_velocity_note_at (cx, y_loud_only);
	int i_quiet = pick_midi_velocity_note_at (cx, y_inner);
	if (i_loud < 0 || i_quiet < 0) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-vel-chord FAILED (pick miss)\n");
		return false;
	}
	if (_engine_snap.midi_notes[i_loud].velocity < 100
	    || _engine_snap.midi_notes[i_loud].pitch != 64) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-vel-chord FAILED "
		              "(loud pick p=%u v=%u)\n",
		              (unsigned)_engine_snap.midi_notes[i_loud].pitch,
		              (unsigned)_engine_snap.midi_notes[i_loud].velocity);
		return false;
	}
	if (_engine_snap.midi_notes[i_quiet].velocity > 60
	    || _engine_snap.midi_notes[i_quiet].pitch != 60) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-vel-chord FAILED "
		              "(quiet pick p=%u v=%u)\n",
		              (unsigned)_engine_snap.midi_notes[i_quiet].pitch,
		              (unsigned)_engine_snap.midi_notes[i_quiet].velocity);
		return false;
	}
	/* Apply selection through the same path mouse-down uses. */
	const EngineMidiNoteSnap& loud = _engine_snap.midi_notes[i_loud];
	if (!_engine.select_midi_note_at (loud.start_sec + loud.length_sec * 0.5f, loud.pitch)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-vel-chord FAILED (select)\n");
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (_engine_snap.midi_selected_note_id != loud.id) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-vel-chord FAILED (selected id=%d want=%d)\n",
		              (int)_engine_snap.midi_selected_note_id, (int)loud.id);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-midi-vel-chord ok "
	              "(loud p=64 v=110 / quiet p=60 v=40)\n");
	return true;
}

bool
ShellWindow::smoke_midi_playhead_draw ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-playhead-draw FAILED (no session)\n");
		return false;
	}
	if (!_engine.ensure_midi_track ()) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-playhead-draw FAILED (midi track)\n");
		return false;
	}
	layout_panes ();
	_lanes[0].strip.set_view_window (0.f, 8.f);
	apply_shared_view (0);
	/* Drive UI playhead into mid-view (draw uses strip norm, same as audio lanes). */
	_playhead_sec = 4.f;
	WaveStripParams wp;
	wp.engine_running = _engine_snap.running;
	wp.transport_playing = false;
	wp.sample_rate = _engine_snap.sample_rate ? _engine_snap.sample_rate : 48000;
	wp.period_frames = _engine_snap.period_frames ? _engine_snap.period_frames : 512;
	wp.time_sec = 0.f;
	wp.playhead_sec = _playhead_sec;
	wp.gain = 1.f;
	_lanes[0].strip.update_view (wp);

	float ph = _lanes[0].strip.playhead_norm ();
	if (ph <= 0.02f || ph >= 0.98f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-playhead-draw FAILED (norm=%.3f ph_sec=%.3f)\n",
		              ph, _playhead_sec);
		return false;
	}
	const D2D1_RECT_F& r = _midi_lane;
	float w = r.right - r.left;
	if (w < 20.f) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-playhead-draw FAILED (midi lane)\n");
		return false;
	}
	float px = r.left + ph * w;
	if (px < r.left - 0.5f || px > r.right + 0.5f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-playhead-draw FAILED "
		              "(px=%.1f lane=[%.1f..%.1f])\n",
		              px, r.left, r.right);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-midi-playhead-draw ok "
	              "(norm=%.3f px=%.1f lane=[%.1f..%.1f])\n",
	              ph, px, r.left, r.right);
	return true;
}

bool
ShellWindow::smoke_midi_audition_ghost ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-audition-ghost FAILED (no session)\n");
		return false;
	}
	if (!_engine.ensure_midi_track () || !_engine.ensure_midi_region (8.f)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-audition-ghost FAILED (region)\n");
		return false;
	}
	layout_panes ();
	const D2D1_RECT_F piano = midi_piano_rect ();
	float h = piano.bottom - piano.top;
	if (h < 20.f) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-audition-ghost FAILED (piano)\n");
		return false;
	}
	const uint8_t pitch = 64; /* E4 */
	if (!_engine.audition_midi_note (pitch, 100)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-audition-ghost FAILED (audition)\n");
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (_engine_snap.midi_audition_pitch != (int)pitch) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-audition-ghost FAILED (snap=%d want=%u)\n",
		              _engine_snap.midi_audition_pitch, (unsigned)pitch);
		_engine.silence_midi_audition ();
		return false;
	}
	float row_h = h / (float)kMidiPitchRows;
	int row = (int)(kMidiPitchHi - pitch);
	float y0 = piano.top + (float)row * row_h;
	float y1 = y0 + row_h;
	if (y0 < piano.top - 1.f || y1 > piano.bottom + 1.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-audition-ghost FAILED "
		              "(row y=[%.1f..%.1f] piano=[%.1f..%.1f])\n",
		              y0, y1, piano.top, piano.bottom);
		_engine.silence_midi_audition ();
		return false;
	}
	_engine.silence_midi_audition ();
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (_engine_snap.midi_audition_pitch >= 0) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-audition-ghost FAILED (still live)\n");
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-midi-audition-ghost ok "
	              "(pitch=%u row y=[%.1f..%.1f])\n",
	              (unsigned)pitch, y0, y1);
	return true;
}


void
ShellWindow::do_quantize_midi_notes ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.quantize_midi_notes ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_legato_midi_notes ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.legato_midi_notes ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_force_midi_note_length ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.force_midi_note_length_to_grid ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_vst3_insert (int dir)
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.cycle_vst3_insert (dir)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_fx_from_engine ();
		sync_prefs_from_engine ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_transpose_midi_notes (int semitones)
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.transpose_midi_notes (semitones)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_nudge_midi_velocity (int delta)
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.nudge_midi_velocity (delta)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_force_midi_velocity (int velocity)
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.set_midi_velocity (velocity)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_nudge_midi_length (float delta_sec)
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.nudge_midi_length (delta_sec)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_copy_midi_notes ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.copy_selected_midi_notes ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cut_midi_notes ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.cut_selected_midi_notes ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_paste_midi_notes ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.paste_midi_notes (-1.f)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_duplicate_midi_notes ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.duplicate_selected_midi_notes ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_midi_panic ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.midi_panic ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_nudge_midi_program (int delta)
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.nudge_midi_program (delta)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_nudge_midi_bank (int delta)
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.nudge_midi_bank (delta)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_export_stems ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	int count = 0;
	if (_engine.export_stems (std::string (), &count)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_mute_all_tracks ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.mute_all_tracks ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		for (int i = 0; i < n_mixer_strips (); ++i) {
			_mixers[i].set_muted (true);
		}
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_solo_all_tracks ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.solo_all_tracks ()) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		for (int i = 0; i < n_mixer_strips (); ++i) {
			_mixers[i].set_soloed (true);
		}
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}


void
ShellWindow::do_humanize_midi_notes (float strength)
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_engine.humanize_midi_notes (strength, 0)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

bool
ShellWindow::smoke_midi_step_piano_highlight ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-step-piano-highlight FAILED (no session)\n");
		return false;
	}
	if (!_engine_snap.midi_step_enabled) {
		_engine.toggle_midi_step_enabled ();
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	if (!_engine_snap.midi_step_enabled) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-step-piano-highlight FAILED (step off)\n");
		return false;
	}
	clear_step_piano_held ();
	const uint8_t pitch = 60; /* C4 — in piano range */
	do_step_piano_key_down (pitch);
	if (!_step_piano_held[pitch] || _step_piano_held_count < 1) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-step-piano-highlight FAILED (held)\n");
		clear_step_piano_held ();
		return false;
	}
	layout_panes ();
	const D2D1_RECT_F piano = midi_piano_rect ();
	float h = piano.bottom - piano.top;
	if (h < 20.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-step-piano-highlight FAILED (piano)\n");
		clear_step_piano_held ();
		return false;
	}
	int row = (int)(kMidiPitchHi - pitch);
	float row_h = h / (float)kMidiPitchRows;
	float y0 = piano.top + (float)row * row_h;
	float y1 = y0 + row_h;
	if (y0 < piano.top - 1.f || y1 > piano.bottom + 1.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-step-piano-highlight FAILED "
		              "(row y=[%.1f..%.1f])\n",
		              y0, y1);
		clear_step_piano_held ();
		return false;
	}
	do_step_piano_key_up (pitch);
	if (_step_piano_held[pitch] || _step_piano_held_count != 0) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-step-piano-highlight FAILED (clear)\n");
		clear_step_piano_held ();
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-midi-step-piano-highlight ok "
	              "(pitch=%u row y=[%.1f..%.1f])\n",
	              (unsigned)pitch, y0, y1);
	return true;
}

void
ShellWindow::do_toggle_midi_step ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.toggle_midi_step_enabled ();
	_engine_snap = _engine.snapshot ();
	_transport.set_midi_step_enabled (_engine_snap.midi_step_enabled);
	_transport.set_midi_chord_quality (_engine_snap.midi_chord_quality);
	if (!_engine_snap.midi_step_enabled) {
		clear_step_piano_held ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_midi_chord ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.cycle_midi_chord_quality ();
	_engine_snap = _engine.snapshot ();
	_transport.set_midi_chord_quality (_engine_snap.midi_chord_quality);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_toggle_midi_arp ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.toggle_midi_arp_enabled ();
	_engine_snap = _engine.snapshot ();
	_transport.set_midi_arp_enabled (_engine_snap.midi_arp_enabled);
	_transport.set_midi_arp_mode (_engine_snap.midi_arp_mode);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_midi_arp_mode ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.cycle_midi_arp_mode ();
	_engine_snap = _engine.snapshot ();
	_transport.set_midi_arp_enabled (_engine_snap.midi_arp_enabled);
	_transport.set_midi_arp_mode (_engine_snap.midi_arp_mode);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::sync_scale_to_transport ()
{
	_transport.set_midi_scale_lock (_engine_snap.midi_scale_lock);
	_transport.set_midi_scale_kind (_engine_snap.midi_scale_kind);
	_transport.set_midi_scale_root (_engine_snap.midi_scale_root);
}

void
ShellWindow::do_toggle_midi_scale ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.toggle_midi_scale_lock ();
	_engine_snap = _engine.snapshot ();
	sync_scale_to_transport ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_midi_scale_kind ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.cycle_midi_scale_kind ();
	if (!_engine.midi_scale_lock ()) {
		_engine.set_midi_scale_lock (true);
	}
	_engine_snap = _engine.snapshot ();
	sync_scale_to_transport ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_cycle_midi_scale_root ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.cycle_midi_scale_root ();
	if (!_engine.midi_scale_lock ()) {
		_engine.set_midi_scale_lock (true);
	}
	_engine_snap = _engine.snapshot ();
	sync_scale_to_transport ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::sync_arp_held_from_step_keys ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	if (_step_piano_held_count <= 0) {
		_engine.clear_midi_arp_held_pitches ();
		return;
	}
	uint8_t buf[16];
	int n = 0;
	for (int p = 0; p < 128 && n < 16; ++p) {
		if (_step_piano_held[p]) {
			buf[n++] = (uint8_t)p;
		}
	}
	_engine.set_midi_arp_held_pitches (buf, n);
}

void
ShellWindow::clear_step_piano_held ()
{
	for (int i = 0; i < 128; ++i) {
		_step_piano_held[i] = false;
	}
	_step_piano_held_count = 0;
	if (_engine.is_linked ()) {
		_engine.clear_midi_arp_held_pitches ();
	}
}

void
ShellWindow::do_step_input_pitch (uint8_t pitch, bool advance)
{
	if (!_engine.is_linked () || !_engine_snap.session_ok) {
		return;
	}
	if (_engine.step_input_midi_note (pitch, 100, advance)) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_step_piano_key_down (uint8_t pitch)
{
	if (pitch > 127) {
		return;
	}
	if (_step_piano_held[pitch]) {
		return; /* ignore WM_KEYDOWN autorepeat */
	}
	_step_piano_held[pitch] = true;
	++_step_piano_held_count;
	sync_arp_held_from_step_keys ();
	/* Place without advancing; advance on last key-up (multi-key chord). */
	do_step_input_pitch (pitch, false);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::do_step_piano_key_up (uint8_t pitch)
{
	if (pitch > 127 || !_step_piano_held[pitch]) {
		return;
	}
	_step_piano_held[pitch] = false;
	if (_step_piano_held_count > 0) {
		--_step_piano_held_count;
	}
	sync_arp_held_from_step_keys ();
	if (_step_piano_held_count > 0) {
		return;
	}
	/* Last piano key released → advance one grid step. */
	if (_engine.is_linked () && _engine_snap.session_ok) {
		_engine.step_advance (1);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
}

int
ShellWindow::step_piano_pitch_for_key (WPARAM wparam) const
{
	/* Classic two-octave computer keyboard (Z-row = C4, Q-row = C5). */
	struct Map { WPARAM key; uint8_t pitch; };
	static const Map kMap[] = {
		{ 'Z', 60 }, { 'S', 61 }, { 'X', 62 }, { 'D', 63 }, { 'C', 64 },
		{ 'V', 65 }, { 'G', 66 }, { 'B', 67 }, { 'H', 68 }, { 'N', 69 },
		{ 'J', 70 }, { 'M', 71 },
		{ 'Q', 72 }, { '2', 73 }, { 'W', 74 }, { '3', 75 }, { 'E', 76 },
		{ 'R', 77 }, { '5', 78 }, { 'T', 79 }, { '6', 80 }, { 'Y', 81 },
		{ '7', 82 }, { 'U', 83 },
	};
	WPARAM k = wparam;
	if (k >= 'a' && k <= 'z') {
		k = (WPARAM)(k - 'a' + 'A');
	}
	for (size_t i = 0; i < sizeof (kMap) / sizeof (kMap[0]); ++i) {
		if (kMap[i].key == k) {
			return (int)kMap[i].pitch;
		}
	}
	return -1;
}

void
ShellWindow::do_cycle_snap_density ()
{
	if (!_engine.is_linked ()) {
		return;
	}
	_engine.cycle_snap_density ();
	if (!_engine.snap_enabled ()) {
		_engine.set_snap_enabled (true);
	}
	_engine.refresh_grid_lines (
		_lanes[0].strip.view_start_sec (),
		_lanes[0].strip.view_span_sec ());
	_engine_snap = _engine.snapshot ();
	_transport.set_snap_enabled (_engine_snap.snap_enabled);
	_transport.set_snap_density (_engine_snap.snap_density);
	_transport.set_midi_step_enabled (_engine_snap.midi_step_enabled);
	_transport.set_midi_chord_quality (_engine_snap.midi_chord_quality);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

const wchar_t*
ShellWindow::snap_caption_suffix () const
{
	if (!_engine_snap.snap_enabled) {
		return L" OFF";
	}
	static const wchar_t* suf[] = {
		L" ON·bar", L" ON·beat", L" ON·1/2", L" ON·1/4"
	};
	int d = _engine_snap.snap_density;
	if (d < 0 || d > 3) {
		d = 1;
	}
	return suf[d];
}

bool
ShellWindow::smoke_snap_density_label ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap-density-label FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	static const wchar_t* want_on[] = { L"Bar", L"Beat", L"1/2", L"1/4" };
	static const wchar_t* want_cap[] = {
		L" ON·bar", L" ON·beat", L" ON·1/2", L" ON·1/4"
	};
	_engine.set_snap_enabled (true);
	for (int d = 0; d < 4; ++d) {
		_engine.set_snap_density (d);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_transport.set_snap_enabled (_engine_snap.snap_enabled);
		_transport.set_snap_density (_engine_snap.snap_density);
		if (wcscmp (_transport.snap_btn_label (), want_on[d]) != 0) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-snap-density-label FAILED "
			              "(btn dens=%d got=%ls want=%ls)\n",
			              d, _transport.snap_btn_label (), want_on[d]);
			std::fflush (stderr);
			return false;
		}
		if (wcscmp (snap_caption_suffix (), want_cap[d]) != 0) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-snap-density-label FAILED "
			              "(cap dens=%d got=%ls)\n",
			              d, snap_caption_suffix ());
			std::fflush (stderr);
			return false;
		}
	}
	_engine.set_snap_enabled (false);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_transport.set_snap_enabled (false);
	_transport.set_snap_density (_engine_snap.snap_density);
	if (wcscmp (_transport.snap_btn_label (), L"Snap") != 0) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap-density-label FAILED (off label)\n");
		std::fflush (stderr);
		return false;
	}
	if (wcscmp (snap_caption_suffix (), L" OFF") != 0) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap-density-label FAILED (off cap)\n");
		std::fflush (stderr);
		return false;
	}
	float sx = 0.f;
	float sy = 0.f;
	if (!_transport.snap_btn_center (&sx, &sy)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap-density-label FAILED (center)\n");
		std::fflush (stderr);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-snap-density-label ok "
	              "(Bar/Beat/1/2/1/4 · Snap off)\n");
	std::fflush (stderr);
	return true;
}

void
ShellWindow::count_snap_ruler_ticks (int* out_bar, int* out_subdiv) const
{
	int bars = 0;
	int subdiv = 0;
	if (_engine_snap.snap_enabled && _engine_snap.grid_line_count > 0) {
		float view0 = _lanes[0].strip.view_start_sec ();
		float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
		for (int i = 0; i < _engine_snap.grid_line_count; ++i) {
			float n = (_engine_snap.grid_line_sec[i] - view0) / span;
			if (n < 0.f || n > 1.f) {
				continue;
			}
			if (_engine_snap.grid_line_bar[i]) {
				++bars;
			} else {
				++subdiv;
			}
		}
	}
	if (out_bar) {
		*out_bar = bars;
	}
	if (out_subdiv) {
		*out_subdiv = subdiv;
	}
}

bool
ShellWindow::smoke_snap_ruler_ticks ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap-ruler-ticks FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	layout_panes ();
	/* Fixed view so grid refresh matches what the ruler would paint. */
	for (int i = 0; i < kTracks; ++i) {
		_lanes[i].strip.set_view_window (0.f, 8.f);
	}

	_engine.set_tempo_bpm (120.f);
	_engine.set_snap_enabled (true);
	_engine.set_snap_density (0); /* bars only */
	_engine.refresh_grid_lines (0.f, 8.f);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();

	int bars = 0;
	int subdiv = 0;
	count_snap_ruler_ticks (&bars, &subdiv);
	if (bars < 3 || subdiv != 0) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap-ruler-ticks FAILED "
		              "(bar dens bars=%d subdiv=%d)\n",
		              bars, subdiv);
		std::fflush (stderr);
		return false;
	}

	_engine.set_snap_density (3); /* 1/4 — bars + many subdiv */
	_engine.refresh_grid_lines (0.f, 8.f);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	int bars_q = 0;
	int subdiv_q = 0;
	count_snap_ruler_ticks (&bars_q, &subdiv_q);
	if (bars_q < 1 || subdiv_q <= subdiv || (bars_q + subdiv_q) <= bars) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap-ruler-ticks FAILED "
		              "(1/4 bars=%d subdiv=%d prev_bars=%d)\n",
		              bars_q, subdiv_q, bars);
		std::fflush (stderr);
		return false;
	}

	_engine.set_snap_enabled (false);
	_engine.refresh_grid_lines (0.f, 8.f);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	int bars_off = 0;
	int subdiv_off = 0;
	count_snap_ruler_ticks (&bars_off, &subdiv_off);
	if (bars_off != 0 || subdiv_off != 0) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap-ruler-ticks FAILED "
		              "(off bars=%d subdiv=%d)\n",
		              bars_off, subdiv_off);
		std::fflush (stderr);
		return false;
	}

	std::fprintf (stderr,
	              "airdour_win_shell: smoke-snap-ruler-ticks ok "
	              "(bar=%d · 1/4 bars=%d subdiv=%d · off=0)\n",
	              bars, bars_q, subdiv_q);
	std::fflush (stderr);
	return true;
}

bool
ShellWindow::hit_status_snap (float x, float y) const
{
	const D2D1_RECT_F& r = _status_snap_btn;
	return r.right > r.left && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

bool
ShellWindow::status_snap_center (float* out_x, float* out_y) const
{
	if (!out_x || !out_y) {
		return false;
	}
	const D2D1_RECT_F& r = _status_snap_btn;
	if (r.right <= r.left) {
		return false;
	}
	*out_x = 0.5f * (r.left + r.right);
	*out_y = 0.5f * (r.top + r.bottom);
	return true;
}

bool
ShellWindow::smoke_snap_status_chip ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap-status-chip FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	layout_panes ();
	_engine.set_snap_enabled (false);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_transport.set_snap_enabled (false);
	/* Paint path clears the chip when snap is off — simulate via redraw layout. */
	InvalidateRect (_hwnd, nullptr, FALSE);
	layout_panes ();
	/* Force status chip layout by calling the same geometry used in paint:
	 * clear then paint path sets it; rebuild manually for smoke. */
	_status_snap_btn = D2D1::RectF (0.f, 0.f, 0.f, 0.f);
	if (hit_status_snap (1.f, 1.f) || status_snap_center (nullptr, nullptr)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap-status-chip FAILED (chrome when off)\n");
		std::fflush (stderr);
		return false;
	}

	static const wchar_t* want[] = { L"Bar", L"Beat", L"1/2", L"1/4" };
	_engine.set_snap_enabled (true);
	for (int d = 0; d < 4; ++d) {
		_engine.set_snap_density (d);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_transport.set_snap_enabled (true);
		_transport.set_snap_density (d);
		layout_panes ();
		/* Mirror paint geometry for the chip. */
		{
			const float meter_w = 96.f * _dpi_scale;
			float mid_y = (_status_bar.top + _status_bar.bottom) * 0.5f;
			float bh = 14.f * _dpi_scale;
			float chip_w = 44.f * _dpi_scale;
			float gap = 8.f * _dpi_scale;
			float right = _status_bar.right - 20.f * _dpi_scale - meter_w - gap;
			_status_snap_btn = D2D1::RectF (right - chip_w, mid_y - bh * 0.5f,
			                               right, mid_y + bh * 0.5f);
		}
		if (wcscmp (_transport.snap_btn_label (), want[d]) != 0) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-snap-status-chip FAILED "
			              "(label dens=%d got=%ls)\n",
			              d, _transport.snap_btn_label ());
			std::fflush (stderr);
			return false;
		}
		float sx = 0.f;
		float sy = 0.f;
		if (!status_snap_center (&sx, &sy) || !hit_status_snap (sx, sy)) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-snap-status-chip FAILED "
			              "(hit dens=%d)\n",
			              d);
			std::fflush (stderr);
			return false;
		}
	}

	/* Click chip → cycle Bar→Beat. */
	_engine.set_snap_density (0);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	_transport.set_snap_density (0);
	do_cycle_snap_density ();
	if (_engine_snap.snap_density != 1) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-snap-status-chip FAILED "
		              "(cycle dens=%d want=1)\n",
		              _engine_snap.snap_density);
		std::fflush (stderr);
		return false;
	}

	std::fprintf (stderr,
	              "airdour_win_shell: smoke-snap-status-chip ok "
	              "(Bar/Beat/1/2/1/4 · click cycle · off=no chrome)\n");
	std::fflush (stderr);
	return true;
}

float
ShellWindow::stub_meter_peak (int track, float dt) const
{
	(void)dt;
	if (!_transport_playing) {
		return 0.f;
	}
	float g = _mixers[track].gain ();
	/* Fallback only when Session PeakMeter is unavailable. */
	float t = _playhead_sec * (3.1f + track * 0.7f) + track * 0.4f;
	float pulse = 0.55f + 0.45f * std::sin (t);
	float beat = std::fmod (_playhead_sec * 2.f + track * 0.17f, 1.f);
	if (beat < 0.f) {
		beat += 1.f;
	}
	float hit = (beat < 0.08f) ? (1.f - beat / 0.08f) : 0.f;
	return std::min (1.f, g * (0.35f + 0.4f * pulse + 0.45f * hit));
}

bool
ShellWindow::init_d2d ()
{
	HRESULT hr = D2D1CreateFactory (D2D1_FACTORY_TYPE_SINGLE_THREADED, &_d2d_factory);
	if (FAILED (hr)) {
		return false;
	}
	hr = DWriteCreateFactory (DWRITE_FACTORY_TYPE_SHARED,
	                          __uuidof (IDWriteFactory),
	                          reinterpret_cast<IUnknown**> (&_dwrite_factory));
	if (FAILED (hr)) {
		return false;
	}

	auto make_fmt = [&] (const wchar_t* preferred, const wchar_t* fallback,
	                     DWRITE_FONT_WEIGHT weight, float size, IDWriteTextFormat** out) -> bool {
		HRESULT h = _dwrite_factory->CreateTextFormat (
			preferred, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
			DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out);
		if (FAILED (h)) {
			h = _dwrite_factory->CreateTextFormat (
				fallback, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
				DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out);
		}
		return SUCCEEDED (h);
	};

	/* Brand hero: display face; body/pane: text face; meta: mono for meters/DSP. */
	if (!make_fmt (L"Segoe UI Variable Display", L"Segoe UI", DWRITE_FONT_WEIGHT_BOLD, 44.f, &_fmt_brand)) {
		return false;
	}
	_fmt_brand->SetTextAlignment (DWRITE_TEXT_ALIGNMENT_LEADING);
	_fmt_brand->SetParagraphAlignment (DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
	if (!make_fmt (L"Segoe UI Variable Text", L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 15.f, &_fmt_body)) {
		return false;
	}
	if (!make_fmt (L"Cascadia Mono", L"Consolas", DWRITE_FONT_WEIGHT_MEDIUM, 12.f, &_fmt_meta)) {
		return false;
	}
	if (!make_fmt (L"Segoe UI Variable Text", L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.5f, &_fmt_pane)) {
		return false;
	}
	return true;
}

void
ShellWindow::discard_device_resources ()
{
	_ui.detach ();
	if (_brush_bg) { _brush_bg->Release (); _brush_bg = nullptr; }
	if (_brush_meter_hot) { _brush_meter_hot->Release (); _brush_meter_hot = nullptr; }
	if (_brush_midi) { _brush_midi->Release (); _brush_midi = nullptr; }
	if (_brush_marker) { _brush_marker->Release (); _brush_marker = nullptr; }
	if (_brush_punch) { _brush_punch->Release (); _brush_punch = nullptr; }
	if (_brush_loop) { _brush_loop->Release (); _brush_loop = nullptr; }
	if (_brush_selection) { _brush_selection->Release (); _brush_selection = nullptr; }
	if (_brush_playhead) { _brush_playhead->Release (); _brush_playhead = nullptr; }
	if (_brush_rms) { _brush_rms->Release (); _brush_rms = nullptr; }
	if (_brush_peak) { _brush_peak->Release (); _brush_peak = nullptr; }
	if (_brush_pane_edge) { _brush_pane_edge->Release (); _brush_pane_edge = nullptr; }
	if (_brush_pane) { _brush_pane->Release (); _brush_pane = nullptr; }
	if (_brush_accent) { _brush_accent->Release (); _brush_accent = nullptr; }
	if (_brush_body) { _brush_body->Release (); _brush_body = nullptr; }
	if (_brush_brand) { _brush_brand->Release (); _brush_brand = nullptr; }
	if (_rt) { _rt->Release (); _rt = nullptr; }
}

bool
ShellWindow::ensure_device_resources ()
{
	if (_rt) {
		return true;
	}
	if (!_d2d_factory || !_hwnd) {
		discard_device_resources ();
		return false;
	}

	RECT rc = {};
	GetClientRect (_hwnd, &rc);
	D2D1_SIZE_U size = D2D1::SizeU (
		rc.right > 0 ? (UINT)rc.right : _width,
		rc.bottom > 0 ? (UINT)rc.bottom : _height);

	HRESULT hr = _d2d_factory->CreateHwndRenderTarget (
		D2D1::RenderTargetProperties (),
		D2D1::HwndRenderTargetProperties (_hwnd, size),
		&_rt);
	if (FAILED (hr)) {
		discard_device_resources ();
		return false;
	}

	if (!_ui.attach (_rt, _dwrite_factory, _dpi_scale)) {
		_rt->Release ();
		_rt = nullptr;
		return false;
	}

	_rt->CreateSolidColorBrush (kBrand, &_brush_brand);
	_rt->CreateSolidColorBrush (kBody, &_brush_body);
	_rt->CreateSolidColorBrush (kAccent, &_brush_accent);
	_rt->CreateSolidColorBrush (kPaneFill, &_brush_pane);
	_rt->CreateSolidColorBrush (kPaneEdge, &_brush_pane_edge);
	_rt->CreateSolidColorBrush (kPeak, &_brush_peak);
	_rt->CreateSolidColorBrush (kRms, &_brush_rms);
	_rt->CreateSolidColorBrush (kPlayhead, &_brush_playhead);
	_rt->CreateSolidColorBrush (kSelection, &_brush_selection);
	_rt->CreateSolidColorBrush (kLoop, &_brush_loop);
	_rt->CreateSolidColorBrush (kPunch, &_brush_punch);
	_rt->CreateSolidColorBrush (kMarker, &_brush_marker);
	_rt->CreateSolidColorBrush (kMidi, &_brush_midi);
	_rt->CreateSolidColorBrush (kMeterHot, &_brush_meter_hot);

	ID2D1GradientStopCollection* stops = nullptr;
	D2D1_GRADIENT_STOP gs[2];
	gs[0].position = 0.f; gs[0].color = kBgTop;
	gs[1].position = 1.f; gs[1].color = kBgBottom;
	hr = _rt->CreateGradientStopCollection (gs, 2, &stops);
	if (SUCCEEDED (hr)) {
		_rt->CreateLinearGradientBrush (
			D2D1::LinearGradientBrushProperties (D2D1::Point2F (0, 0), D2D1::Point2F (0, (float)size.height)),
			stops, &_brush_bg);
		stops->Release ();
	}

	if (_brush_brand && _brush_body && _brush_accent && _brush_pane
	    && _brush_peak && _brush_rms && _brush_playhead && _brush_selection
	    && _brush_loop && _brush_punch && _brush_marker && _brush_midi && _brush_meter_hot && _brush_bg) {
		return true;
	}
	discard_device_resources ();
	return false;
}

void
ShellWindow::layout_panes ()
{
	const float w = (float)_width;
	const float h = (float)_height;
	const ui::Theme& th = ui::theme ();
	const float pad = 20.f * _dpi_scale;
	const float brand_h = th.metrics.brand_h * _dpi_scale;
	const float menu_h = 30.f * _dpi_scale;
	const float transport_h = th.metrics.transport_h * _dpi_scale;
	const float status_h = th.metrics.status_h * _dpi_scale;
	const float gap = 16.f * _dpi_scale;

	_brand_bar = D2D1::RectF (0, 0, w, brand_h);
	_menu_bar_bounds = D2D1::RectF (0, brand_h, w, brand_h + menu_h);
	_menu_bar.layout (_menu_bar_bounds, _dpi_scale);
	_transport_bounds = D2D1::RectF (0, _menu_bar_bounds.bottom, w,
	                                 _menu_bar_bounds.bottom + transport_h);
	_transport.layout (_transport_bounds, _dpi_scale, (float)_height);
	_status_bar = D2D1::RectF (0, h - status_h, w, h);

	float body_top = _menu_bar_bounds.bottom + transport_h + pad;
	float body_bot = h - status_h - pad;
	float left = pad;
	float right = w - pad;
	float split = left + (right - left) * 0.64f;

	_editor_pane = D2D1::RectF (left, body_top, split - gap * 0.5f, body_bot);
	_mixer_pane = D2D1::RectF (split + gap * 0.5f, body_top, right, body_bot);

	float inset = 12.f * _dpi_scale;
	float title_h = 32.f * _dpi_scale;
	float ruler_h = 22.f * _dpi_scale;
	float label_w = 64.f * _dpi_scale;
	float lane_gap = 6.f * _dpi_scale;
	float wave_left = _editor_pane.left + inset + label_w;
	float wave_right = _editor_pane.right - inset;
	_editor_ruler = D2D1::RectF (wave_left, _editor_pane.top + title_h,
	                             wave_right, _editor_pane.top + title_h + ruler_h);
	float midi_h = 118.f * _dpi_scale; /* piano-roll + velocity lane */
	float midi_cc_h = 40.f * _dpi_scale; /* CC7 automation under MIDI */
	const bool show_spec = _engine_snap.spectrum_enabled;
	const bool show_phase = _engine_snap.correlation_enabled;
	const bool show_lufs = _engine_snap.loudness_enabled;
	const int meter_n = (show_spec ? 1 : 0) + (show_phase ? 1 : 0) + (show_lufs ? 1 : 0);
	float meter_h = (meter_n > 0) ? (72.f * _dpi_scale) : 0.f;
	float lanes_top = _editor_ruler.bottom + 4.f * _dpi_scale;
	float lanes_bot = _editor_pane.bottom - inset - midi_h - lane_gap
	                  - midi_cc_h - lane_gap
	                  - (meter_h > 0.f ? (meter_h + lane_gap) : 0.f);
	const int n_lanes = std::max (1, n_audio_lanes ());
	const float avail_h = std::max (1.f, lanes_bot - lanes_top);
	/* Lane height: stretch to fill the viewport up to 76dp. When there are
	 * more lanes than ~8 fit, hold a readable floor and scroll instead.
	 * The floor is capped so the default 6-track view still fits at 720p. */
	const float min_lane_h = std::max (28.f * _dpi_scale,
	                                   std::min (76.f * _dpi_scale, avail_h / 9.f));
	_editor_lanes_top = lanes_top;
	_editor_lanes_bot = lanes_top + avail_h;
	const float fit_lane_h = (avail_h - lane_gap * (float)(n_lanes - 1)) / (float)n_lanes;
	const float lane_h = std::max (min_lane_h, fit_lane_h);
	const float content_h = lane_h * (float)n_lanes + lane_gap * (float)(n_lanes - 1);
	_editor_scroll_max = std::max (0.f, content_h - avail_h);
	_auto_lane_track = auto_lane_track ();
	if (_editor_scroll_focus != _focus_audio_track) {
		/* Focus changed (click / add / move): bring that lane into view. */
		_editor_scroll_focus = _focus_audio_track;
		const float ft = (float)std::max (0, std::min (_focus_audio_track, n_lanes - 1))
		                 * (lane_h + lane_gap);
		if (ft < _editor_scroll_y) {
			_editor_scroll_y = ft;
		} else if (ft + lane_h > _editor_scroll_y + avail_h) {
			_editor_scroll_y = ft + lane_h - avail_h;
		}
	}
	_editor_scroll_y = std::max (0.f, std::min (_editor_scroll_max, _editor_scroll_y));
	_editor_scrollbar = D2D1::RectF ();
	if (_editor_scroll_max > 0.5f) {
		_editor_scrollbar = D2D1::RectF (_editor_pane.right - 9.f * _dpi_scale, lanes_top,
		                                 _editor_pane.right - 4.f * _dpi_scale,
		                                 _editor_lanes_bot);
	}

	for (int i = 0; i < kTracksMax; ++i) {
		_lanes[i].bounds = D2D1::RectF ();
	}
	_auto_lane_bounds = D2D1::RectF ();
	_midi_cc_lane_bounds = D2D1::RectF ();
	const float lanes_y0 = lanes_top - _editor_scroll_y;
	for (int i = 0; i < n_lanes; ++i) {
		float y0 = lanes_y0 + (float)i * (lane_h + lane_gap);
		if (i == _auto_lane_track) {
			/* Focused lane: waveform on top, gain/pan/send automation underneath. */
			float auto_h = std::max (30.f * _dpi_scale, lane_h * 0.30f);
			float wave_h = lane_h - auto_h - 3.f * _dpi_scale;
			if (wave_h < 28.f * _dpi_scale) {
				wave_h = lane_h * 0.64f;
				auto_h = lane_h - wave_h - 3.f * _dpi_scale;
			}
			_lanes[i].bounds = D2D1::RectF (wave_left, y0, wave_right, y0 + wave_h);
			_auto_lane_bounds = D2D1::RectF (
				wave_left, y0 + wave_h + 3.f * _dpi_scale, wave_right, y0 + lane_h);
		} else {
			_lanes[i].bounds = D2D1::RectF (wave_left, y0, wave_right, y0 + lane_h);
		}
		_lanes[i].strip.resize_for_width (wave_right - wave_left);
	}
	_midi_lane = D2D1::RectF (wave_left, lanes_bot + lane_gap, wave_right,
	                          lanes_bot + lane_gap + midi_h);
	_midi_cc_lane_bounds = D2D1::RectF (
		wave_left, _midi_lane.bottom + 3.f * _dpi_scale, wave_right,
		_midi_lane.bottom + 3.f * _dpi_scale + midi_cc_h);
	_spectrum_bounds = D2D1::RectF ();
	_correlation_bounds = D2D1::RectF ();
	_loudness_bounds = D2D1::RectF ();
	_spectrum.set_visible (false);
	_correlation.set_visible (false);
	_loudness.set_visible (false);
	if (meter_h > 0.f && meter_n > 0) {
		float meter_top = _midi_cc_lane_bounds.bottom + lane_gap;
		float meter_bot = meter_top + meter_h;
		float gap_m = 6.f * _dpi_scale;
		float span = wave_right - wave_left - gap_m * (float)(meter_n - 1);
		float cell = span / (float)meter_n;
		float x = wave_left;
		if (show_spec) {
			_spectrum_bounds = D2D1::RectF (x, meter_top, x + cell, meter_bot);
			_spectrum.layout (_spectrum_bounds);
			_spectrum.set_visible (true);
			x += cell + gap_m;
		}
		if (show_phase) {
			_correlation_bounds = D2D1::RectF (x, meter_top, x + cell, meter_bot);
			_correlation.layout (_correlation_bounds);
			_correlation.set_visible (true);
			x += cell + gap_m;
		}
		if (show_lufs) {
			_loudness_bounds = D2D1::RectF (x, meter_top, x + cell, meter_bot);
			_loudness.layout (_loudness_bounds);
			_loudness.set_visible (true);
		}
	}

	float mix_inset = 10.f * _dpi_scale;
	float mix_top = _mixer_pane.top + 36.f * _dpi_scale;
	float mix_bot = _mixer_pane.bottom - mix_inset;
	float mix_inner = _mixer_pane.right - _mixer_pane.left - mix_inset * 2.f;
	float strip_gap = 6.f * _dpi_scale;
	const int n_mix = std::max (1, n_mixer_strips ());
	/* Reserve History + Monitor strips on the right. */
	float mon_w = 56.f * _dpi_scale;
	float hist_w = 128.f * _dpi_scale;
	float strips_inner = mix_inner - mon_w - hist_w - strip_gap * 2.f;
	if (strips_inner < 40.f * _dpi_scale) {
		hist_w = 110.f * _dpi_scale;
		mon_w = 48.f * _dpi_scale;
		strips_inner = mix_inner - mon_w - hist_w - strip_gap * 2.f;
	}
	float equal_w = (strips_inner - strip_gap * (float)(n_mix - 1)) / (float)n_mix;
	float narrow_w = 52.f * _dpi_scale;
	float strip_w = (_engine_snap.mixer_narrow && equal_w > narrow_w)
		? narrow_w : equal_w;
	float min_w = _engine_snap.mixer_narrow ? narrow_w : 64.f * _dpi_scale;
	if (_mixer_force_min_w > 1.f) {
		min_w = _mixer_force_min_w * _dpi_scale;
	}
	if (strip_w < min_w) {
		strip_w = min_w;
	}
	float content_w = strip_w * (float)n_mix + strip_gap * (float)(n_mix - 1);
	_mixer_scroll_max = std::max (0.f, content_w - strips_inner);
	if (_mixer_scroll_x > _mixer_scroll_max) {
		_mixer_scroll_x = _mixer_scroll_max;
	}
	if (_mixer_scroll_x < 0.f) {
		_mixer_scroll_x = 0.f;
	}
	float strips_left = _mixer_pane.left + mix_inset;
	_mixer_strips_clip = D2D1::RectF (strips_left, mix_top,
	                                  strips_left + strips_inner, mix_bot);
	for (int i = 0; i < kMixerStripsMax; ++i) {
		_mixers[i].layout (D2D1::RectF (0, 0, 0, 0), _dpi_scale);
		_mixers[i].set_narrow (false);
	}
	for (int i = 0; i < n_mix; ++i) {
		_mixers[i].set_narrow (_engine_snap.mixer_narrow && !_mixers[i].is_midi_strip ());
		float x0 = strips_left + (float)i * (strip_w + strip_gap) - _mixer_scroll_x;
		_mixers[i].layout (D2D1::RectF (x0, mix_top, x0 + strip_w, mix_bot), _dpi_scale);
	}
	float mon_x = _mixer_pane.right - mix_inset - mon_w;
	float hist_x = mon_x - strip_gap - hist_w;
	_history.layout (D2D1::RectF (hist_x, mix_top, hist_x + hist_w, mix_bot), _dpi_scale);
	_monitor.layout (D2D1::RectF (mon_x, mix_top, mon_x + mon_w, mix_bot), _dpi_scale);
}

bool
ShellWindow::create (HINSTANCE instance, int show_cmd)
{
	if (!init_d2d ()) {
		return false;
	}

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof (wc);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = ShellWindow::wnd_proc;
	wc.hInstance = instance;
	wc.hCursor = LoadCursorW (nullptr, IDC_ARROW);
	wc.hbrBackground = nullptr;
	wc.lpszClassName = kClassName;
	wc.hIcon = LoadIconW (nullptr, IDI_APPLICATION);
	if (!RegisterClassExW (&wc) && GetLastError () != ERROR_CLASS_ALREADY_EXISTS) {
		return false;
	}

	DWORD style = WS_OVERLAPPEDWINDOW;
	RECT wr = { 0, 0, (LONG)_width, (LONG)_height };
	AdjustWindowRect (&wr, style, FALSE);

	_hwnd = CreateWindowExW (
		0, kClassName, kWindowTitle, style,
		CW_USEDEFAULT, CW_USEDEFAULT,
		wr.right - wr.left, wr.bottom - wr.top,
		nullptr, nullptr, instance, this);
	if (!_hwnd) {
		return false;
	}

	_dpi = query_dpi_for_hwnd (_hwnd);
	_dpi_scale = (float)_dpi / 96.f;
	layout_panes ();
	ShowWindow (_hwnd, show_cmd);
	UpdateWindow (_hwnd);
	/* 30 Hz UI clock — enough for meters; saves paint vs 60 Hz while playing. */
	SetTimer (_hwnd, 1, 33, nullptr);
	return true;
}

LRESULT CALLBACK
ShellWindow::wnd_proc (HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	ShellWindow* self = nullptr;
	if (msg == WM_NCCREATE) {
		auto* cs = reinterpret_cast<CREATESTRUCTW*> (lparam);
		self = static_cast<ShellWindow*> (cs->lpCreateParams);
		SetWindowLongPtrW (hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (self));
		self->_hwnd = hwnd;
	} else {
		self = reinterpret_cast<ShellWindow*> (GetWindowLongPtrW (hwnd, GWLP_USERDATA));
	}
	if (self) {
		return self->handle_message (msg, wparam, lparam);
	}
	return DefWindowProcW (hwnd, msg, wparam, lparam);
}

LRESULT
ShellWindow::handle_message (UINT msg, WPARAM wparam, LPARAM lparam)
{
	switch (msg) {
	case WM_SIZE:
		on_resize (LOWORD (lparam), HIWORD (lparam));
		return 0;
	case WM_DPICHANGED: {
		UINT dpi = HIWORD (wparam);
		RECT* suggested = reinterpret_cast<RECT*> (lparam);
		on_dpi_changed (dpi, suggested);
		return 0;
	}
	case WM_DISPLAYCHANGE:
		InvalidateRect (_hwnd, nullptr, FALSE);
		return 0;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		BeginPaint (_hwnd, &ps);
		on_paint ();
		EndPaint (_hwnd, &ps);
		return 0;
	}
	case WM_TIMER:
		on_tick ();
		return 0;
	case WM_KEYUP:
		if (_engine_snap.midi_step_enabled
		    && !(GetKeyState (VK_CONTROL) & 0x8000)
		    && !(GetKeyState (VK_MENU) & 0x8000)
		    && step_piano_pitch_for_key (wparam) >= 0) {
			do_step_piano_key_up ((uint8_t)step_piano_pitch_for_key (wparam));
			return 0;
		}
		break;
	case WM_KEYDOWN:
		if (_about.splash_visible ()) {
			/* Any key dismisses splash (smokes use --no-splash / --timeout). */
			dismiss_splash ();
			return 0;
		}
		if (_recovery.visible ()) {
			if (wparam == VK_ESCAPE || wparam == 'N' || wparam == 'n') {
				apply_recovery_hit (RecoveryHit::Discard);
			} else if (wparam == VK_RETURN || wparam == 'Y' || wparam == 'y') {
				apply_recovery_hit (RecoveryHit::Restore);
			}
			return 0;
		}
		/* Early out keeps WM_KEYDOWN else-if chain under MSVC C1061. */
		if ((GetKeyState (VK_CONTROL) & 0x8000)
		    && (GetKeyState (VK_SHIFT) & 0x8000)
		    && !(GetKeyState (VK_MENU) & 0x8000)
		    && (wparam == 'A' || wparam == 'a')) {
			/* Ctrl+Shift+A = select all MIDI notes. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.select_all_midi_notes ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
			return 0;
		}
		if ((GetKeyState (VK_CONTROL) & 0x8000)
		    && (GetKeyState (VK_SHIFT) & 0x8000)
		    && !(GetKeyState (VK_MENU) & 0x8000)
		    && (wparam == 'I' || wparam == 'i')) {
			/* Ctrl+Shift+I = invert MIDI note selection. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.invert_midi_note_selection ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
			return 0;
		}
		if ((GetKeyState (VK_CONTROL) & 0x8000)
		    && (GetKeyState (VK_MENU) & 0x8000)
		    && !(GetKeyState (VK_SHIFT) & 0x8000)
		    && (wparam == 'V' || wparam == 'v')) {
			/* Ctrl+Alt+V = force MIDI velocity to 100. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				do_force_midi_velocity (100);
			}
			return 0;
		}
		if ((GetKeyState (VK_CONTROL) & 0x8000)
		    && (GetKeyState (VK_MENU) & 0x8000)
		    && (GetKeyState (VK_SHIFT) & 0x8000)
		    && (wparam == 'P' || wparam == 'p')) {
			/* Ctrl+Alt+Shift+P = select all notes matching primary pitch. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.select_midi_notes_same_pitch ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
			return 0;
		}
		if ((GetKeyState (VK_CONTROL) & 0x8000)
		    && (GetKeyState (VK_MENU) & 0x8000)
		    && (GetKeyState (VK_SHIFT) & 0x8000)
		    && (wparam == 'V' || wparam == 'v')) {
			/* Ctrl+Alt+Shift+V = select all notes matching primary velocity
			 * (distinct from Ctrl+Alt+V force-velocity). */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.select_midi_notes_same_velocity ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
			return 0;
		}
		if ((GetKeyState (VK_CONTROL) & 0x8000)
		    && (GetKeyState (VK_MENU) & 0x8000)
		    && (GetKeyState (VK_SHIFT) & 0x8000)
		    && (wparam == 'O' || wparam == 'o')) {
			/* Ctrl+Alt+Shift+O = select all notes of same pitch-class (octaves). */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.select_midi_notes_same_pitch_class ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
			return 0;
		}
		if ((GetKeyState (VK_CONTROL) & 0x8000)
		    && (GetKeyState (VK_MENU) & 0x8000)
		    && (GetKeyState (VK_SHIFT) & 0x8000)
		    && (wparam == 'L' || wparam == 'l')) {
			/* Ctrl+Alt+Shift+L = select all notes matching primary length
			 * (distinct from Ctrl+Alt+L region position-lock / zoom-loop). */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.select_midi_notes_same_length ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
			return 0;
		}
		if ((GetKeyState (VK_CONTROL) & 0x8000)
		    && (GetKeyState (VK_MENU) & 0x8000)
		    && (GetKeyState (VK_SHIFT) & 0x8000)
		    && (wparam == 'N' || wparam == 'n')) {
			/* Ctrl+Alt+Shift+N = select all notes matching primary MIDI channel. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.select_midi_notes_same_channel ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
			return 0;
		}
		if ((GetKeyState (VK_CONTROL) & 0x8000)
		    && (GetKeyState (VK_MENU) & 0x8000)
		    && (GetKeyState (VK_SHIFT) & 0x8000)
		    && (wparam == 'I' || wparam == 'i')) {
			/* Ctrl+Alt+Shift+I = select MIDI notes intersecting loop range. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.select_midi_notes_in_loop ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
			return 0;
		}
		if ((GetKeyState (VK_CONTROL) & 0x8000)
		    && (GetKeyState (VK_MENU) & 0x8000)
		    && (GetKeyState (VK_SHIFT) & 0x8000)
		    && (wparam == 'U' || wparam == 'u')) {
			/* Ctrl+Alt+Shift+U = select MIDI notes intersecting punch range. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.select_midi_notes_in_punch ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
			return 0;
		}
		if ((GetKeyState (VK_CONTROL) & 0x8000)
		    && (GetKeyState (VK_MENU) & 0x8000)
		    && (GetKeyState (VK_SHIFT) & 0x8000)
		    && (wparam == 'X' || wparam == 'x')) {
			/* Ctrl+Alt+Shift+X = select MIDI notes outside loop range. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.select_midi_notes_outside_loop ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
			return 0;
		}
		if ((GetKeyState (VK_CONTROL) & 0x8000)
		    && (GetKeyState (VK_MENU) & 0x8000)
		    && (GetKeyState (VK_SHIFT) & 0x8000)
		    && (wparam == 'Y' || wparam == 'y')) {
			/* Ctrl+Alt+Shift+Y = select MIDI notes outside punch range. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.select_midi_notes_outside_punch ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
			return 0;
		}
		if ((GetKeyState (VK_CONTROL) & 0x8000)
		    && (GetKeyState (VK_MENU) & 0x8000)
		    && (GetKeyState (VK_SHIFT) & 0x8000)
		    && (wparam == 'W' || wparam == 'w')) {
			/* Ctrl+Alt+Shift+W = select MIDI notes outside loop∪punch. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.select_midi_notes_outside_ranges ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
			return 0;
		}
		if ((GetKeyState (VK_CONTROL) & 0x8000)
		    && (GetKeyState (VK_MENU) & 0x8000)
		    && (GetKeyState (VK_SHIFT) & 0x8000)
		    && (wparam == 'J' || wparam == 'j')) {
			/* Ctrl+Alt+Shift+J = select MIDI notes in loop∩punch. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.select_midi_notes_in_ranges ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
			return 0;
		}

		/* Early outs keep the WM_KEYDOWN else-if chain under MSVC C1061. */
		if ((GetKeyState (VK_CONTROL) & 0x8000)
		    && (GetKeyState (VK_MENU) & 0x8000)
		    && (GetKeyState (VK_SHIFT) & 0x8000)
		    && (wparam == VK_OEM_COMMA || wparam == VK_OEM_PERIOD)) {
			/* Ctrl+Alt+Shift+,/. = cycle VST3 catalog insert on Audio 2. */
			do_cycle_vst3_insert (wparam == VK_OEM_PERIOD ? 1 : -1);
			return 0;
		}
		if (_help_visible) {
			if (wparam == VK_ESCAPE || wparam == VK_F1 || wparam == VK_RETURN
			    || ((wparam == VK_OEM_2) && (GetKeyState (VK_SHIFT) & 0x8000))) {
				_help_visible = false;
				InvalidateRect (_hwnd, nullptr, FALSE);
				return 0;
			}
			const int page = 10;
			int target = _help_scroll;
			bool nav = true;
			switch (wparam) {
			case VK_PRIOR: case VK_UP:   target -= page; break;
			case VK_NEXT:  case VK_DOWN: target += page; break;
			case VK_HOME:  target = 0; break;
			case VK_END:   target = _help_scroll_max; break;
			default: nav = false; break;
			}
			if (nav && _help_scroll_max > 0) {
				_help_scroll = std::max (0, std::min (_help_scroll_max, target));
				InvalidateRect (_hwnd, nullptr, FALSE);
				return 0;
			}
		}
		/* File lifecycle shortcuts run before the long feature chain. */
		if ((GetKeyState (VK_CONTROL) & 0x8000) && !(GetKeyState (VK_MENU) & 0x8000)) {
			const bool shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
			if (!shift && (wparam == 'N' || wparam == 'n')) {
				do_new_session ();
				return 0;
			}
			if (shift && (wparam == 'S' || wparam == 's')) {
				do_save_session_as ();
				return 0;
			}
			if (!shift && (wparam == 'O' || wparam == 'o')) {
				do_open_session_folder ();
				return 0;
			}
		}
		if (wparam == VK_ESCAPE) {
			if (_transport.overflow_open ()) {
				_transport.set_overflow_open (false);
				InvalidateRect (_hwnd, nullptr, FALSE);
			} else if (_engine_snap.has_selection) {
				/* Esc = clear region selection (quit via second Esc). */
				do_clear_selection ();
			} else if (_recent.visible ()) {
				_recent.set_visible (false);
				InvalidateRect (_hwnd, nullptr, FALSE);
			} else if (_prefs_visible) {
				_prefs_visible = false;
				_prefs.set_visible (false);
				InvalidateRect (_hwnd, nullptr, FALSE);
			} else if (_about.about_visible ()) {
				_about.set_about_visible (false);
				InvalidateRect (_hwnd, nullptr, FALSE);
			} else if (_help_visible) {
				_help_visible = false;
				InvalidateRect (_hwnd, nullptr, FALSE);
			} else {
				/* Esc no longer quits — Alt+F4 / the close button do. */
			}
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'O' || wparam == 'o')
		           && (GetKeyState (VK_SHIFT) & 0x8000)) {
			/* Ctrl+Shift+O — recent sessions menu. */
			toggle_recent_menu ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_OEM_COMMA || wparam == VK_OEM_PERIOD)) {
			/* Ctrl+Alt+,/. = prev/next cue marker. */
			if (wparam == VK_OEM_PERIOD) {
				do_locate_next_cue ();
			} else {
				do_locate_prev_cue ();
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == VK_OEM_COMMA || wparam == VK_OEM_PERIOD)) {
			/* Alt+, /. = nudge playhead by snap unit. */
			do_nudge_playhead (wparam == VK_OEM_PERIOD ? 1 : -1);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_OEM_COMMA) {
			/* Ctrl+, — Preferences. */
			toggle_prefs ();
		} else if (wparam == VK_F3) {
			/* F3 — About Airdour. */
			toggle_about ();
		} else if (wparam == VK_F1
		           || ((wparam == VK_OEM_2 || wparam == VK_DIVIDE)
		               && (GetKeyState (VK_SHIFT) & 0x8000))) {
			/* F1 or Shift+/ (?) — toggle shortcuts help. */
			toggle_help ();
		} else if (wparam == VK_TAB) {
			/* Tab / Shift+Tab = cycle region selection forward/back. */
			do_cycle_region_selection ((GetKeyState (VK_SHIFT) & 0x8000) ? -1 : 1);
		} else if (wparam == VK_SPACE) {
			if (_transport_playing) {
				_engine.midi_panic ();
			}
			transport_toggle ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'D' || wparam == 'd')) {
			if (_engine_snap.has_selection) {
				do_duplicate_selection ();
			} else {
				do_duplicate_midi_notes ();
			}
		} else if (wparam == VK_DELETE || wparam == VK_BACK) {
			do_delete_selection ();
		} else if ((GetKeyState (VK_SHIFT) & 0x8000) && wparam == VK_HOME
		           && !(GetKeyState (VK_CONTROL) & 0x8000)) {
			/* Shift+Home = locate to selection start. */
			do_locate_selection_start ();
		} else if ((GetKeyState (VK_SHIFT) & 0x8000) && wparam == VK_END
		           && !(GetKeyState (VK_CONTROL) & 0x8000)) {
			/* Shift+End = locate to selection end. */
			do_locate_selection_end ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == '0' || wparam == VK_NUMPAD0)) {
			/* Ctrl+Alt+0 = center editor view on playhead (keep zoom). */
			do_center_on_playhead ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && (wparam == '0' || wparam == VK_NUMPAD0)) {
			/* Ctrl+Shift+0 = scroll view to keep playhead visible (keep zoom). */
			do_scroll_playhead_into_view ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_HOME) {
			/* Ctrl+Alt+Home = zoom editor to session extent. */
			do_zoom_to_session ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_HOME) {
			/* Ctrl+Home = locate session start. */
			do_locate_session_start ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_END) {
			/* Ctrl+End = locate session end. */
			do_locate_session_end ();
		} else if (wparam == VK_HOME || wparam == '0') {
			reset_shared_view ();
			refresh_editor_view_after_nav ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		} else if (wparam == VK_F4) {
			/* F4 — toggle MIDI step-record. */
			do_toggle_midi_step ();
		} else if (wparam == VK_F5) {
			/* F5 — cycle MIDI chord quality Off → Maj → Min. */
			do_cycle_midi_chord ();
		} else if (wparam == VK_F6) {
			/* F6 — toggle arp; Shift+F6 cycle Up/Down/UpDown. */
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_cycle_midi_arp_mode ();
			} else {
				do_toggle_midi_arp ();
			}
		} else if (wparam == VK_F7) {
			/* F7 — scale lock; Shift=kind; Ctrl=root. */
			if (GetKeyState (VK_CONTROL) & 0x8000) {
				do_cycle_midi_scale_root ();
			} else if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_cycle_midi_scale_kind ();
			} else {
				do_toggle_midi_scale ();
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'A' || wparam == 'a')) {
			/* Alt+Shift+A — cycle editor auto lane Gain → Pan → Send. */
			cycle_auto_lane_kind ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'Y' || wparam == 'y'
		               || wparam == 'P' || wparam == 'p'
		               || wparam == 'Q' || wparam == 'q')) {
			/* Alt+Shift+Y — cycle AutoState for current lane (P/Q redirect). */
			do_cycle_auto_state ();
		} else if (wparam == VK_F8) {
			/* F8 — cycle MIDI CC lane controller Volume→Mod→Expr. */
			cycle_midi_cc_controller ();
		} else if (wparam == VK_F9) {
			/* F9 — MIDI panic. */
			do_midi_panic ();
		} else if (_engine_snap.midi_step_enabled
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_LEFT || wparam == VK_RIGHT)) {
			/* Step mode: ←/→ = rest back/forward one grid. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				clear_step_piano_held ();
				_engine.step_advance (wparam == VK_RIGHT ? 1 : -1);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				_playhead_sec = _engine_snap.transport_sec;
				sync_transport_clock_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (_engine_snap.midi_step_enabled
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && step_piano_pitch_for_key (wparam) >= 0) {
			do_step_piano_key_down ((uint8_t)step_piano_pitch_for_key (wparam));
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_LEFT || wparam == VK_RIGHT)
		           && _engine_snap.has_selection) {
			/* Ctrl+←/→ = nudge selected region by snap unit (Shift = 4×). */
			float unit = _engine.snap_unit_sec ();
			if (!_engine_snap.snap_enabled) {
				unit = 0.05f;
			}
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				unit *= 4.f;
			}
			do_nudge_selection (wparam == VK_RIGHT ? unit : -unit);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_LEFT || wparam == VK_RIGHT)) {
			/* Ctrl+Alt+←/→ = previous / next marker. */
			if (wparam == VK_RIGHT) {
				do_locate_next_marker ();
			} else {
				do_locate_prev_marker ();
			}
		} else if (wparam == VK_LEFT) {
			float step = (GetKeyState (VK_SHIFT) & 0x8000) ? 0.5f : 0.15f;
			do_scroll_editor (-step);
		} else if (wparam == VK_RIGHT) {
			float step = (GetKeyState (VK_SHIFT) & 0x8000) ? 0.5f : 0.15f;
			do_scroll_editor (step);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'A' || wparam == 'a')) {
			/* Ctrl+Alt+A = zoom editor to all regions. */
			do_zoom_to_regions ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'A' || wparam == 'a')) {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_fit_session ();
			} else {
				do_fit_all_regions ();
			}
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'E' || wparam == 'e')) {
			/* Ctrl+Alt+E = zoom editor to selection. */
			do_zoom_to_selection ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'E' || wparam == 'e')) {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				/* Alt+Shift+E = export per-track stems. */
				do_export_stems ();
			} else {
				/* Alt+E = export selection range WAV. */
				prompt_export_selection_wav ();
			}
		} else if ((GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'E' || wparam == 'e')) {
			/* Shift+E = activate region gain envelope. */
			do_ensure_region_envelope ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'E' || wparam == 'e')
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && !(GetKeyState (VK_SHIFT) & 0x8000)) {
			/* E = fit selection (Ctrl+E remains export). */
			do_fit_selection ();
		} else if (wparam == 'L' || wparam == 'l') {
			if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && (GetKeyState (VK_SHIFT) & 0x8000)
			    && !(GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+Shift+L = loop range ← selection. */
				do_loop_from_selection ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && (GetKeyState (VK_MENU) & 0x8000)
			    && _engine_snap.has_selection) {
				/* Ctrl+Alt+L = position-lock selected region. */
				do_toggle_region_position_lock ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && (GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+Alt+L = zoom editor to loop range. */
				do_zoom_to_loop ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000)
			           && !(GetKeyState (VK_SHIFT) & 0x8000)
			           && !(GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+L = lock / unlock selected region (blocks move/trim). */
				do_toggle_selection_locked ();
			} else {
				do_toggle_loop ();
			}
		} else if (wparam == 'O' || wparam == 'o') {
			if ((GetKeyState (VK_MENU) & 0x8000)
			    && !(GetKeyState (VK_CONTROL) & 0x8000)) {
				/* Alt+O = toggle region opaque / transparent. */
				do_toggle_selection_opaque ();
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'L' || wparam == 'l')) {
			/* Alt+Shift+L — cycle Solo → PFL → AFL → Solo. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.cycle_listen_mode ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_listen_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (!(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'L' || wparam == 'l')) {
			do_toggle_loop ();
		} else if (wparam == 'P' || wparam == 'p') {
			if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && (GetKeyState (VK_SHIFT) & 0x8000)
			    && !(GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+Shift+P = punch range ← selection. */
				do_punch_from_selection ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && (GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+Alt+P = zoom editor to punch range. */
				do_zoom_to_punch ();
			} else if (!(GetKeyState (VK_CONTROL) & 0x8000)) {
				do_toggle_punch ();
			}
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_OEM_4) {
			/* Ctrl+Alt+[ = locate punch in. */
			do_locate_punch_in ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_OEM_6) {
			/* Ctrl+Alt+] = locate punch out. */
			do_locate_punch_out ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && wparam == VK_OEM_6) {
			/* Ctrl+Alt+Shift+] = cycle region layer top↔bottom. */
			do_cycle_region_layer ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && _engine_snap.has_selection
		           && (wparam == VK_OEM_4 || wparam == VK_OEM_6)) {
			/* Ctrl+[ / ] = lower / raise selected region (Shift = bottom / top). */
			bool extreme = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
			if (wparam == VK_OEM_6) {
				do_raise_selection (extreme);
			} else {
				do_lower_selection (extreme);
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && _engine_snap.has_selection
		           && (wparam == VK_OEM_4 || wparam == VK_OEM_6)) {
			/* Alt+[ / ] = nudge fade-in ±10ms; Alt+Shift = fade-out. */
			float delta = (wparam == VK_OEM_6 ? 0.010f : -0.010f);
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				_engine.nudge_selected_fade_out_sec (delta);
			} else {
				_engine.nudge_selected_fade_in_sec (delta);
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			apply_region_selection_to_lanes ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_OEM_4) {
			/* Ctrl+[ = locate loop start. */
			do_locate_loop_start ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && wparam == VK_OEM_6) {
			/* Ctrl+] = locate loop end. */
			do_locate_loop_end ();
		} else if (wparam == VK_OEM_4) { /* [ */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				if (GetKeyState (VK_SHIFT) & 0x8000) {
					_engine.set_punch_start (_playhead_sec);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_transport_punch_from_engine ();
				} else {
					_engine.set_loop_start (_playhead_sec);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_transport_loop_from_engine ();
					apply_loop_to_lanes ();
				}
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (wparam == VK_OEM_6) { /* ] */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				if (GetKeyState (VK_SHIFT) & 0x8000) {
					_engine.set_punch_end (_playhead_sec);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_transport_punch_from_engine ();
				} else {
					_engine.set_loop_end (_playhead_sec);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_transport_loop_from_engine ();
					apply_loop_to_lanes ();
				}
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (wparam == 'M' || wparam == 'm') {
			if ((GetKeyState (VK_MENU) & 0x8000) && (GetKeyState (VK_SHIFT) & 0x8000)) {
				/* Alt+Shift+M = mute all audio tracks + MIDI. */
				do_mute_all_tracks ();
			} else if ((GetKeyState (VK_MENU) & 0x8000)
			           && !(GetKeyState (VK_SHIFT) & 0x8000)
			           && !(GetKeyState (VK_CONTROL) & 0x8000)) {
				/* Alt+M = clear all mutes. */
				do_clear_all_mutes ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && !(GetKeyState (VK_SHIFT) & 0x8000)
			    && !(GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+M = mute / unmute selected region. */
				do_toggle_selection_muted ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && (GetKeyState (VK_SHIFT) & 0x8000)) {
				/* Ctrl+Shift+M = lock/unlock marker at playhead. */
				do_toggle_marker_lock_at_playhead ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000)
			           && (GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+Alt+M = toggle CD flag on marker at playhead. */
				do_toggle_marker_cd_at_playhead ();
			} else if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_remove_marker_at_playhead ();
			} else {
				do_add_marker_at_playhead ();
			}
		
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'Z' || wparam == 'z')) {
			do_undo ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'Y' || wparam == 'y')) {
			/* Ctrl+Alt+Y = sync at playhead; Shift clears. */
			do_region_sync_at_playhead ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'Y' || wparam == 'y')) {
			do_redo ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && (wparam == 'C' || wparam == 'c')) {
			/* Ctrl+Alt+Shift+C = cycle scene MIDI channel. */
			do_cycle_scene_channel ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'C' || wparam == 'c')) {
			do_copy_midi_notes ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'X' || wparam == 'x')) {
			do_cut_midi_notes ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'V' || wparam == 'v')) {
			do_paste_midi_notes ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && (wparam == 'S' || wparam == 's')) {
			/* Alt+Shift+S = rename nearest section marker. */
			do_rename_section_at_playhead ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'S' || wparam == 's')) {
			do_save_session ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'S' || wparam == 's')) {
			do_split_at_playhead ();
		} else if (wparam == 'F' || wparam == 'f') {
			if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && !(GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+F = toggle fade in/out active. */
				do_toggle_fade_active ();
			} else if (GetKeyState (VK_SHIFT) & 0x8000) {
				/* Shift+F = cycle fade shape (Linear→EqualPower→Fast→Slow→Symmetric). */
				do_cycle_fade_shape ();
			} else if (!(GetKeyState (VK_CONTROL) & 0x8000)) {
				do_apply_crossfade ();
			}
			} else if ((GetKeyState (VK_MENU) & 0x8000)
			           && (GetKeyState (VK_SHIFT) & 0x8000)
			           && !(GetKeyState (VK_CONTROL) & 0x8000)
			           && (wparam == 'T' || wparam == 't')) {
				/* Alt+Shift+T — reset focused strip trim to unity. */
				if (_engine.is_linked () && _engine_snap.session_ok) {
					int ti = _focus_audio_track;
					if (ti < 0) {
						ti = 0;
					}
					if (ti < n_audio_lanes ()) {
						_engine.set_track_trim (ti, 1.f);
						_engine.poll ();
						_engine_snap = _engine.snapshot ();
						sync_mixer_trim_from_engine ();
						InvalidateRect (_hwnd, nullptr, FALSE);
					}
				}
			} else if (!(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'T' || wparam == 't')) {
			/* T = stretch 1.5×; Shift+T = compress 0.75× (pitch unchanged). */
			do_stretch_selection ((GetKeyState (VK_SHIFT) & 0x8000) ? 0.75f : 1.5f);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'R' || wparam == 'r')) {
			/* Ctrl+Shift+R = range marker ← selection. */
			do_range_marker_from_selection ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'R' || wparam == 'r')) {
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'R' || wparam == 'r')) {
			/* Alt+Shift+R — reset focused strip fader/pan/send. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				int ti = _focus_audio_track;
				if (ti < 0) {
					ti = 0;
				}
				if (ti < n_audio_lanes ()) {
					_engine.reset_strip_mixer (ti);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_mixer_gains_from_engine ();
					sync_mixer_pan_from_engine ();
					sync_mixer_send_from_engine ();
					InvalidateRect (_hwnd, nullptr, FALSE);
				}
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'R' || wparam == 'r')) {
			/* R = pitch +1 st; Shift+R = −1 st (duration held). */
			do_pitch_shift_selection ((GetKeyState (VK_SHIFT) & 0x8000) ? -1.f : 1.f);
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'V' || wparam == 'v')) {
			/* Alt+Shift+V — ensure VCA + toggle Audio 1 assign. */
			do_toggle_vca_assign ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'V' || wparam == 'v')
		           && !(GetKeyState (VK_MENU) & 0x8000)) {
			/* V = reverse selected audio region. */
			do_reverse_selection ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'N' || wparam == 'n')) {
			/* Alt+Shift+N — toggle Route comment ("note") on focused strip. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				int ti = _focus_audio_track;
				if (ti < 0) {
					ti = 0;
				}
				if (ti < n_audio_lanes ()) {
					_engine.toggle_track_comment (ti);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_mixer_comment_from_engine ();
					InvalidateRect (_hwnd, nullptr, FALSE);
				}
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'W' || wparam == 'w')) {
			/* Alt+Shift+W — toggle narrow mixer strips. */
			_engine.toggle_mixer_narrow ();
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			layout_panes ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'D' || wparam == 'd')) {
			/* Alt+Shift+D — cycle monitor dim depth −20/−12/−6. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.cycle_monitor_dim_level ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_monitor_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'N' || wparam == 'n')) {
			/* N = normalize selection to −1 dBFS. */
			do_normalize_selection (-1.f);
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'I' || wparam == 'i')) {
			/* Alt+Shift+I — toggle polarity invert on focused strip. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				int ti = _focus_audio_track;
				if (ti < 0) {
					ti = 0;
				}
				if (ti < n_audio_lanes ()) {
					_engine.toggle_track_phase_invert (ti);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_mixer_phase_from_engine ();
					InvalidateRect (_hwnd, nullptr, FALSE);
				}
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'I' || wparam == 'i')) {
			/* I = strip silence on selection (−40 dBFS threshold). */
			do_strip_silence_selection ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'B' || wparam == 'b')) {
			/* Alt+Shift+B — activate every shell ACE insert (Reverb…Gate). */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.set_all_fx_bypassed (false);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_fx_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && !(GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'B' || wparam == 'b')) {
			/* Alt+B — bypass every shell ACE insert (Reverb…Gate). */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.set_all_fx_bypassed (true);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_fx_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'B' || wparam == 'b')) {
			/* B = consolidate / bounce selection in place. */
			do_consolidate_selection ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'Z' || wparam == 'z')) {
			/* Z = freeze / unfreeze focused (or selected) audio track. */
			do_toggle_freeze_track ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'G' || wparam == 'g')) {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_cycle_snap_density ();
			} else {
				do_toggle_snap ();
			}
		} else if ((wparam == 'Q' || wparam == 'q')) {
			if ((GetKeyState (VK_CONTROL) & 0x8000) && (GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+Alt+Q = legato selected (or all) MIDI notes. */
				do_legato_midi_notes ();
			} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (GetKeyState (VK_SHIFT) & 0x8000)) {
				/* Shift+Q = force note length to snap unit. */
				do_force_midi_note_length ();
			} else if (!(GetKeyState (VK_CONTROL) & 0x8000)) {
				/* Q = quantize selected (or all) MIDI notes to snap grid. */
				do_quantize_midi_notes ();
			}
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'H' || wparam == 'h')) {
			/* Ctrl+Alt+H = toggle Comp SC Audio1→Audio2. */
			do_toggle_comp_sidechain ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'H' || wparam == 'h')) {
			/* H = humanize; Shift+H = 2× strength. */
			do_humanize_midi_notes ((GetKeyState (VK_SHIFT) & 0x8000) ? 2.f : 1.f);
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000)

		           && (wparam == VK_OEM_PERIOD || wparam == VK_OEM_COMMA)) {
			/* . / , = MIDI transpose +1 / −1; Shift = ±12 (octave). */
			int st = (wparam == VK_OEM_PERIOD) ? 1 : -1;
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				st *= 12;
			}
			do_transpose_midi_notes (st);
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'C' || wparam == 'c')) {
			/* Alt+C — cycle PresentationInfo color on focused audio track. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				int idx = _focus_audio_track;
				if (_engine_snap.has_selection && _engine_snap.selection_track >= 0) {
					idx = _engine_snap.selection_track;
				}
				if (idx < 0 || idx >= n_audio_lanes ()) {
					idx = 0;
				}
				if (_engine.cycle_track_color (idx)) {
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_track_labels_from_engine ();
					InvalidateRect (_hwnd, nullptr, FALSE);
				}
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'C' || wparam == 'c')) {
			do_toggle_count_in ();
		} else if (wparam == 'K' || wparam == 'k') {
			if ((GetKeyState (VK_CONTROL) & 0x8000)
			    && (GetKeyState (VK_SHIFT) & 0x8000)
			    && !(GetKeyState (VK_MENU) & 0x8000)) {
				/* Ctrl+Shift+K = skip range ← selection. */
				do_skip_range_from_selection ();
			} else if (!(GetKeyState (VK_CONTROL) & 0x8000)) {
				do_toggle_click ();
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'K' || wparam == 'k')) {
			/* Alt+Shift+K — cycle monitor solo boost 0/+3/+6. */
			do_cycle_monitor_solo_boost ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == 'K' || wparam == 'k')) {
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'H' || wparam == 'h')) {
			/* Alt+Shift+H — cycle metronome click gain. */
			do_cycle_click_gain ();
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'K' || wparam == 'k')) {
			do_toggle_click ();
		} else if (wparam == VK_OEM_MINUS || wparam == VK_SUBTRACT) {
			do_nudge_tempo (-1.f);
		} else if (wparam == VK_OEM_PLUS || wparam == VK_ADD) {
			do_nudge_tempo (1.f);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'O' || wparam == 'o')
		           && !(GetKeyState (VK_SHIFT) & 0x8000)) {
			do_open_session_folder (); /* Ctrl+O folder; Ctrl+Shift+O = recent menu */
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'E' || wparam == 'e')) {
			prompt_export_wav ();
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_OEM_PLUS || wparam == VK_ADD)) {
			/* Ctrl+Alt+= = zoom in around playhead. */
			do_zoom_around_playhead (1.15f);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_OEM_MINUS || wparam == VK_SUBTRACT)) {
			/* Ctrl+Alt+− = zoom out around playhead. */
			do_zoom_around_playhead (1.f / 1.15f);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == VK_OEM_PLUS || wparam == VK_ADD)) {
			_lanes[0].strip.zoom_at (1.15f, 0.5f);
			apply_shared_view (0);
			refresh_editor_view_after_nav ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000) && (wparam == VK_OEM_MINUS || wparam == VK_SUBTRACT)) {
			_lanes[0].strip.zoom_at (1.f / 1.15f, 0.5f);
			apply_shared_view (0);
			refresh_editor_view_after_nav ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		} else if (wparam == VK_F2) {
			if ((GetKeyState (VK_MENU) & 0x8000)
			    && (GetKeyState (VK_SHIFT) & 0x8000)) {
				/* Alt+Shift+F2 = rename marker at playhead. */
				do_rename_marker_at_playhead ();
			} else if (GetKeyState (VK_SHIFT) & 0x8000) {
				/* Shift+F2 = rename selected region. */
				prompt_rename_selected_region ();
			} else {
				do_rename_focused_track ();
			}
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_UP || wparam == VK_DOWN)
		           && _engine_snap.has_selection) {
			/* Ctrl+Shift+↑/↓ = nudge selected region gain ±1 dB. */
			do_nudge_selection_gain_db (wparam == VK_UP ? 1.0f : -1.0f);
		} else if ((GetKeyState (VK_CONTROL) & 0x8000)
		           && (GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_UP || wparam == VK_DOWN)
		           && _engine_snap.has_selection) {
			/* Ctrl+Alt+↑/↓ = nudge region envelope mid gain. */
			do_nudge_region_envelope_mid (wparam == VK_UP ? 0.08f : -0.08f);
		} else if ((GetKeyState (VK_MENU) & 0x8000) && wparam == VK_UP) {
			do_move_audio_track (-1);
		} else if ((GetKeyState (VK_MENU) & 0x8000) && wparam == VK_DOWN) {
			do_move_audio_track (+1);
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_LEFT || wparam == VK_RIGHT)) {
			/* Alt+←/→ = MIDI note length ± (snap unit when snap on; else 50ms). */
			if (_engine.is_linked () && _engine_snap.midi_note_count > 0) {
				float unit = _engine.snap_unit_sec ();
				if (!_engine_snap.snap_enabled) {
					unit = 0.05f;
				}
				if (GetKeyState (VK_SHIFT) & 0x8000) {
					unit *= 4.f;
				}
				do_nudge_midi_length (wparam == VK_RIGHT ? unit : -unit);
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000)
		           && !(GetKeyState (VK_MENU) & 0x8000)
		           && (wparam == VK_UP || wparam == VK_DOWN)) {
			/* ↑/↓ = MIDI velocity ±1 (Shift=±8) when notes exist. */
			if (_engine.is_linked () && _engine_snap.midi_note_count > 0) {
				int d = (wparam == VK_UP) ? 1 : -1;
				if (GetKeyState (VK_SHIFT) & 0x8000) {
					d *= 8;
				}
				do_nudge_midi_velocity (d);
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'Y' || wparam == 'y')) {
			if (_engine.is_linked ()) {
				if (GetKeyState (VK_SHIFT) & 0x8000) {
					_engine.toggle_spectrum_master ();
				} else {
					_engine.toggle_spectrum_enabled ();
				}
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				layout_panes ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'X' || wparam == 'x')) {
			if (_engine.is_linked ()) {
				if (GetKeyState (VK_SHIFT) & 0x8000) {
					_engine.toggle_correlation_master ();
				} else {
					_engine.toggle_correlation_enabled ();
				}
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				layout_panes ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'J' || wparam == 'j')) {
			if (_engine.is_linked ()) {
				if (GetKeyState (VK_SHIFT) & 0x8000) {
					_engine.toggle_loudness_master ();
				} else {
					_engine.toggle_loudness_enabled ();
				}
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				layout_panes ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		} else if (wparam == 'U' || wparam == 'u') {
			if ((GetKeyState (VK_MENU) & 0x8000) && (GetKeyState (VK_SHIFT) & 0x8000)) {
				/* Alt+Shift+U = solo all audio tracks + MIDI. */
				do_solo_all_tracks ();
			} else if ((GetKeyState (VK_MENU) & 0x8000)
			           && !(GetKeyState (VK_SHIFT) & 0x8000)
			           && !(GetKeyState (VK_CONTROL) & 0x8000)) {
				/* Alt+U = clear all solos. */
				do_clear_all_solos ();
			} else if (!(GetKeyState (VK_CONTROL) & 0x8000)) {
				do_apply_or_clear_group ();
			}
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'G' || wparam == 'g')) {
			/* Alt+Shift+G — toggle RouteGroup relative gain. */
			do_toggle_group_relative ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == VK_LEFT || wparam == VK_RIGHT)) {
			/* Alt+Shift+←/→ — scroll mixer strips when overflowing. */
			float step = 64.f * _dpi_scale;
			do_scroll_mixer (wparam == VK_RIGHT ? step : -step);
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'C' || wparam == 'c')) {
			/* Alt+Shift+C — cycle RouteGroup color palette. */
			do_cycle_group_color ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'O' || wparam == 'o')) {
			/* Alt+Shift+O — cycle focused strip monitor Auto→In→Disk. */
			if (_engine.is_linked () && _engine_snap.session_ok) {
				int ti = _focus_audio_track;
				if (ti < 0) {
					ti = 0;
				}
				if (ti < n_audio_lanes ()) {
					_engine.cycle_track_monitor_choice (ti);
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_mixer_monitor_from_engine ();
					InvalidateRect (_hwnd, nullptr, FALSE);
				}
			}
		} else if (!(GetKeyState (VK_CONTROL) & 0x8000) && (wparam == 'U' || wparam == 'u')) {
			do_apply_or_clear_group ();
		} else if ((GetKeyState (VK_MENU) & 0x8000)
		           && (GetKeyState (VK_SHIFT) & 0x8000)
		           && !(GetKeyState (VK_CONTROL) & 0x8000)
		           && (wparam == 'E' || wparam == 'e')) {
			/* Alt+Shift+E — toggle focused track Route::active (group shares). */
			do_toggle_focused_track_active ();
		}
		
		return 0;
	case WM_MOUSEWHEEL:
		if (on_mouse_wheel (wparam, lparam)) {
			return 0;
		}
		break;
	case WM_LBUTTONDOWN:
		if (on_mouse_down (lparam)) {
			return 0;
		}
		break;
	case WM_RBUTTONDOWN:
		if (on_mouse_down_right (lparam)) {
			return 0;
		}
		break;
	case WM_LBUTTONDBLCLK: {
		float x, y;
		client_from_lparam (lparam, &x, &y);
		/* Alt+dbl-click fader/pan/send → reset that strip (unity/center/0). */
		if ((GetKeyState (VK_MENU) & 0x8000) && _engine.is_linked ()
		    && _engine_snap.session_ok) {
			int si = hit_mixer_fader (x, y);
			if (si < 0) {
				si = hit_mixer_pan (x, y);
			}
			if (si < 0) {
				si = hit_mixer_send (x, y);
			}
			if (si >= 0 && si < n_audio_lanes ()) {
				_focus_audio_track = si;
				_engine.reset_strip_mixer (si);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_gains_from_engine ();
				sync_mixer_pan_from_engine ();
				sync_mixer_send_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
				return 0;
			}
		}
		/* Double-click FX param → insert default (before rename). */
		if (_engine.is_linked () && _engine_snap.session_ok) {
			int which = 0;
			int fxi = -1;
			if ((fxi = hit_mixer_fx_param3 (x, y)) >= 0
			    && _mixers[fxi].has_fx () && _mixers[fxi].fx_param_count () >= 3) {
				which = 3;
			} else if ((fxi = hit_mixer_fx_param2 (x, y)) >= 0
			           && _mixers[fxi].has_fx () && _mixers[fxi].fx_param_count () >= 2) {
				which = 2;
			} else if ((fxi = hit_mixer_fx_param (x, y)) >= 0 && _mixers[fxi].has_fx ()) {
				which = 1;
			}
			if (which > 0 && fxi >= 0 && fxi < n_audio_lanes ()) {
				bool ok = false;
				/* Audio 2 + live VST3 → plugin default; else ACE insert defaults. */
				if (fxi == 1 && _engine_snap.vst3_ok) {
					ok = _engine.reset_vst3_param (which - 1);
				} else {
					ok = _engine.reset_track_fx_param (fxi, which);
				}
				if (ok) {
					_engine.poll ();
					_engine_snap = _engine.snapshot ();
					sync_mixer_fx_from_engine ();
					InvalidateRect (_hwnd, nullptr, FALSE);
				}
				return 0;
			}
		}
		int trim_i = hit_mixer_trim (x, y);
		if (trim_i >= 0 && _engine.is_linked () && _engine_snap.session_ok) {
			/* Double-click trim → unity (0 dB). */
			_engine.set_track_trim (trim_i, 1.f);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_trim_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return 0;
		}
		if (_monitor.hit_aux_return (x, y)) {
			do_set_aux_return (1.f); /* unity */
			return 0;
		}
		/* Double-click fader → unity; pan → center (before rename hit). */
		int fi = hit_mixer_fader (x, y);
		if (fi >= 0 && _engine.is_linked () && _engine_snap.session_ok) {
			_mixers[fi].set_gain (1.f);
			if (fi == midi_mixer_index ()) {
				_engine.set_midi_gain (1.f);
			} else {
				_engine.set_track_gain (fi, 1.f);
				if (fi == 0) {
					_engine_snap.gain = 1.f;
				}
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_gains_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return 0;
		}
		int pi = hit_mixer_pan (x, y);
		if (pi >= 0 && _engine.is_linked () && _engine_snap.session_ok) {
			_mixers[pi].set_pan (0.5f);
			_engine.set_track_pan (pi, 0.5f);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_pan_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return 0;
		}
		/* Double-click aux send → 0 (before rename hit). */
		int si = hit_mixer_send (x, y);
		if (si >= 0 && _engine.is_linked () && _engine_snap.session_ok) {
			_mixers[si].set_send (0.f);
			_engine.set_track_send (si, 0.f);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_send_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return 0;
		}
		int lab = hit_mixer_label (x, y);
		if (lab >= 0) {
			const int mi = midi_mixer_index ();
			if (lab == mi) {
				prompt_rename_track (0, true);
			} else if (lab < n_audio_lanes ()) {
				_focus_audio_track = lab;
				refresh_automation_lane ();
				prompt_rename_track (lab, false);
			}
			return 0;
		}
		int ed = hit_editor_lane_label (x, y);
		if (ed >= 0) {
			_focus_audio_track = ed;
			refresh_automation_lane ();
			prompt_rename_track (ed, false);
			return 0;
		}
		break;
	}
	case WM_MOUSEMOVE:
		if (on_mouse_move (lparam)) {
			return 0;
		}
		break;
	case WM_MOUSELEAVE:
		_mouse_tracking = false;
		if (_transport.clear_hover ()) {
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return 0;
	case WM_LBUTTONUP:
	case WM_CAPTURECHANGED:
		on_mouse_up ();
		return 0;
	case WM_DESTROY:
		KillTimer (_hwnd, 1);
		_engine.stop ();
		discard_device_resources ();
		PostQuitMessage (0);
		return 0;
	default:
		return DefWindowProcW (_hwnd, msg, wparam, lparam);
	}
	return DefWindowProcW (_hwnd, msg, wparam, lparam);
}

void
ShellWindow::client_from_lparam (LPARAM lparam, float* out_x, float* out_y) const
{
	*out_x = (float)(short)LOWORD (lparam);
	*out_y = (float)(short)HIWORD (lparam);
}

bool
ShellWindow::hit_editor_ruler (float x, float y) const
{
	const D2D1_RECT_F& r = _editor_ruler;
	return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

D2D1_RECT_F
ShellWindow::midi_piano_rect () const
{
	const D2D1_RECT_F& r = _midi_lane;
	float vel_h = 28.f * _dpi_scale;
	float h = r.bottom - r.top;
	if (h < vel_h + 24.f * _dpi_scale) {
		vel_h = std::max (12.f * _dpi_scale, h * 0.28f);
	}
	return D2D1::RectF (r.left, r.top, r.right, r.bottom - vel_h);
}

D2D1_RECT_F
ShellWindow::midi_velocity_rect () const
{
	const D2D1_RECT_F& r = _midi_lane;
	D2D1_RECT_F piano = midi_piano_rect ();
	return D2D1::RectF (r.left, piano.bottom, r.right, r.bottom);
}

bool
ShellWindow::hit_midi_lane (float x, float y) const
{
	const D2D1_RECT_F& r = _midi_lane;
	return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

bool
ShellWindow::hit_midi_velocity_lane (float x, float y) const
{
	const D2D1_RECT_F vr = midi_velocity_rect ();
	return x >= vr.left && x <= vr.right && y >= vr.top && y <= vr.bottom;
}

int
ShellWindow::pick_midi_velocity_note_at (float x, float y) const
{
	const D2D1_RECT_F vr = midi_velocity_rect ();
	float vh = vr.bottom - vr.top;
	if (vh <= 1.f) {
		return -1;
	}
	const float time_sec = midi_time_at_x (x);
	int best_contain = -1;
	float best_contain_h = 1.e9f;
	int best_tall = -1;
	int best_tall_v = -1;
	for (int i = 0; i < _engine_snap.midi_note_count; ++i) {
		const EngineMidiNoteSnap& n = _engine_snap.midi_notes[i];
		float t0 = n.start_sec;
		float t1 = n.start_sec + n.length_sec;
		if (time_sec < t0 - 0.02f || time_sec > t1 + 0.02f) {
			continue;
		}
		float bar_h = vh * ((float)n.velocity / 127.f);
		if (bar_h < 1.f * _dpi_scale && n.velocity > 0) {
			bar_h = 1.f * _dpi_scale;
		}
		float bar_top = vr.bottom - bar_h;
		if (y >= bar_top - 0.5f && y <= vr.bottom + 0.5f) {
			/* Innermost (shortest) containing bar — chord stack tip. */
			if (best_contain < 0 || bar_h < best_contain_h) {
				best_contain = i;
				best_contain_h = bar_h;
			}
		}
		if ((int)n.velocity > best_tall_v) {
			best_tall = i;
			best_tall_v = (int)n.velocity;
		}
	}
	if (best_contain >= 0) {
		return best_contain;
	}
	return best_tall;
}

uint8_t
ShellWindow::midi_pitch_at_y (float y) const
{
	const D2D1_RECT_F r = midi_piano_rect ();
	float h = r.bottom - r.top;
	if (h <= 1.f) {
		return 60;
	}
	float t = (y - r.top) / h;
	if (t < 0.f) {
		t = 0.f;
	}
	if (t > 0.999f) {
		t = 0.999f;
	}
	/* Top of lane = high pitch (piano-roll convention). */
	int row = (int)(t * (float)kMidiPitchRows);
	if (row < 0) {
		row = 0;
	}
	if (row >= kMidiPitchRows) {
		row = kMidiPitchRows - 1;
	}
	return (uint8_t)(kMidiPitchHi - row);
}

int
ShellWindow::midi_velocity_at_y (float y) const
{
	const D2D1_RECT_F vr = midi_velocity_rect ();
	float h = vr.bottom - vr.top;
	if (h <= 1.f) {
		return 100;
	}
	/* Bottom of strip = 1, top = 127. */
	float t = (vr.bottom - y) / h;
	if (t < 0.f) {
		t = 0.f;
	}
	if (t > 1.f) {
		t = 1.f;
	}
	int v = 1 + (int)std::lround (t * 126.f);
	if (v < 1) {
		v = 1;
	}
	if (v > 127) {
		v = 127;
	}
	return v;
}

float
ShellWindow::midi_time_at_x (float x) const
{
	return _lanes[0].strip.time_at_x (_midi_lane, x);
}

int
ShellWindow::hit_editor_lane (float x, float y) const
{
	if (y < _editor_lanes_top || y > _editor_lanes_bot) {
		return -1;
	}
	const int n = n_audio_lanes ();
	for (int i = 0; i < n; ++i) {
		const D2D1_RECT_F& r = _lanes[i].bounds;
		if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_arm (float x, float y) const
{
	const int n = n_mixer_strips ();
	for (int i = 0; i < n; ++i) {
		if (_mixers[i].hit_arm (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_mon (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_mon (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_mute (float x, float y) const
{
	const int n = n_mixer_strips ();
	for (int i = 0; i < n; ++i) {
		if (_mixers[i].hit_mute (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_solo (float x, float y) const
{
	const int n = n_mixer_strips ();
	for (int i = 0; i < n; ++i) {
		if (_mixers[i].hit_solo (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_trim (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_trim (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_pan (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_pan (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_send (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_send (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_meter (float x, float y) const
{
	const int n = n_mixer_strips ();
	for (int i = 0; i < n; ++i) {
		if (_mixers[i].hit_meter (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_fx (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_fx (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_label (float x, float y) const
{
	const int n = n_mixer_strips ();
	for (int i = 0; i < n; ++i) {
		if (_mixers[i].hit_label (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_editor_lane_label (float x, float y) const
{
	if (y < _editor_lanes_top || y > _editor_lanes_bot) {
		return -1;
	}
	const int n = n_audio_lanes ();
	float inset = 12.f * _dpi_scale;
	for (int i = 0; i < n; ++i) {
		const D2D1_RECT_F& lb = _lanes[i].bounds;
		if (lb.right <= lb.left) {
			continue;
		}
		float label_bot = lb.bottom;
		if (i == _auto_lane_track && _auto_lane_bounds.bottom > label_bot) {
			label_bot = _auto_lane_bounds.bottom;
		}
		D2D1_RECT_F label_rc = D2D1::RectF (
			_editor_pane.left + inset, lb.top, lb.left - 4.f * _dpi_scale, label_bot);
		if (x >= label_rc.left && x <= label_rc.right
		    && y >= label_rc.top && y <= label_rc.bottom) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_fx_param (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_fx_param (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_fx_param2 (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_fx_param2 (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_fx_param3 (float x, float y) const
{
	const int na = n_audio_lanes ();
	for (int i = 0; i < na; ++i) {
		if (_mixers[i].hit_fx_param3 (x, y)) {
			return i;
		}
	}
	return -1;
}

int
ShellWindow::hit_mixer_fader (float x, float y) const
{
	const int n = n_mixer_strips ();
	for (int i = 0; i < n; ++i) {
		if (_mixers[i].hit_fader (x, y)) {
			return i;
		}
	}
	return -1;
}

bool
ShellWindow::on_mouse_wheel (WPARAM wparam, LPARAM lparam)
{
	POINT pt;
	pt.x = (short)LOWORD (lparam);
	pt.y = (short)HIWORD (lparam);
	ScreenToClient (_hwnd, &pt);
	float x = (float)pt.x;
	float y = (float)pt.y;
	/* Modal help owns the wheel while open (never zoom the hidden editor). */
	if (_help_visible) {
		const int delta = GET_WHEEL_DELTA_WPARAM (wparam);
		if (delta != 0 && _help_scroll_max > 0) {
			const int step = (GetKeyState (VK_SHIFT) & 0x8000) ? 9 : 3;
			_help_scroll += (delta > 0) ? -step : step;
			_help_scroll = std::max (0, std::min (_help_scroll_max, _help_scroll));
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}
	/* Wheel over mixer strip area → horizontal strip scroll when overflowing. */
	if (x >= _mixer_strips_clip.left && x <= _mixer_strips_clip.right
	    && y >= _mixer_strips_clip.top && y <= _mixer_strips_clip.bottom
	    && _mixer_scroll_max > 1.f) {
		int delta = GET_WHEEL_DELTA_WPARAM (wparam);
		float step = 48.f * _dpi_scale;
		if (GetKeyState (VK_SHIFT) & 0x8000) {
			step *= 2.f;
		}
		do_scroll_mixer ((delta > 0) ? -step : step);
		return true;
	}
	int delta = GET_WHEEL_DELTA_WPARAM (wparam);
	/* Wheel over the lane scrollbar → vertical scroll (no Alt needed). The
	 * scrollbar sits outside the waveform, so it must be tested before the
	 * editor hit-test gate below. */
	if (_editor_scroll_max > 0.5f
	    && y >= _editor_lanes_top && y <= _editor_lanes_bot
	    && x >= _editor_scrollbar.left - _ui.dp (4.f)
	    && x <= _editor_scrollbar.right + _ui.dp (4.f)) {
		float step = 48.f * _dpi_scale;
		if (GetKeyState (VK_CONTROL) & 0x8000) {
			step *= 2.f;
		}
		_editor_scroll_y += (delta > 0) ? -step : step;
		_editor_scroll_y = std::max (0.f, std::min (_editor_scroll_max, _editor_scroll_y));
		_editor_scroll_focus = _focus_audio_track;
		layout_panes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	int lane = hit_editor_lane (x, y);
	bool over_editor = (lane >= 0) || hit_editor_ruler (x, y) || hit_midi_lane (x, y)
	                   || _midi_cc_lane.hit (_midi_cc_lane_bounds, x, y);
	if (!over_editor) {
		return false;
	}
	if (lane < 0) {
		lane = 0;
	}

	/* Alt+wheel → vertical lane scroll. */
	if (_editor_scroll_max > 0.5f && (GetKeyState (VK_MENU) & 0x8000)) {
		float step = 48.f * _dpi_scale;
		if (GetKeyState (VK_CONTROL) & 0x8000) {
			step *= 2.f;
		}
		_editor_scroll_y += (delta > 0) ? -step : step;
		_editor_scroll_y = std::max (0.f, std::min (_editor_scroll_max, _editor_scroll_y));
		_editor_scroll_focus = _focus_audio_track;
		layout_panes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	/* Shift+wheel → horizontal scroll (keep zoom / snap / playhead). */
	if (GetKeyState (VK_SHIFT) & 0x8000) {
		float widths = (delta > 0) ? -0.12f : 0.12f;
		if (GetKeyState (VK_CONTROL) & 0x8000) {
			widths *= 0.35f;
		}
		do_scroll_editor (widths);
		return true;
	}

	const D2D1_RECT_F& r = _lanes[lane].bounds;
	float w = r.right - r.left;
	float norm_x = w > 1.f ? (x - r.left) / w : 0.5f;
	float step = (GetKeyState (VK_CONTROL) & 0x8000) ? 1.05f : 1.12f;
	float factor = (delta > 0) ? step : (1.f / step);
	_lanes[lane].strip.zoom_at (factor, norm_x);
	apply_shared_view (lane);
	refresh_editor_view_after_nav ();
	InvalidateRect (_hwnd, nullptr, FALSE);
	return true;
}

bool
ShellWindow::on_mouse_down (LPARAM lparam)
{
	float x, y;
	client_from_lparam (lparam, &x, &y);

	if (_about.splash_visible ()) {
		dismiss_splash ();
		return true;
	}

	if (_recovery.visible ()) {
		apply_recovery_hit (_recovery.hit_test (x, y));
		return true;
	}

	if (_recent.visible ()) {
		apply_recent_hit (_recent.hit_test (x, y));
		return true;
	}

	if (_prefs_visible) {
		int cat = _prefs.hit_vst3_catalog_index (x, y);
		if (cat >= 0) {
			apply_prefs_vst3_catalog_click (cat);
			return true;
		}
		int pri = _prefs.hit_vst3_param_reset (x, y);
		if (pri >= 0) {
			apply_vst3_param_reset (pri);
			return true;
		}
		PrefsHit ph = _prefs.hit_test (x, y);
		apply_prefs_hit (ph);
		return true;
	}

	if (_about.about_visible ()) {
		if (_about.hit_dismiss_about (x, y)) {
			_about.set_about_visible (false);
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_help_visible) {
		/* Any click dismisses the shortcuts overlay. */
		_help_visible = false;
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_menu_bar.open ()) {
		const MenuCommand mc = _menu_bar.item_at (x, y);
		const int over_title = _menu_bar.menu_at (x, y);
		if (mc != MenuCommand::None) {
			_menu_bar.close ();
			apply_menu_command (mc);
		} else if (over_title >= 0) {
			if (over_title == 5) {
				_menu_bar.close ();
				toggle_help ();
			} else {
				_menu_bar.toggle_menu (over_title);
			}
		} else if (_menu_bar.overlay_contains (x, y)) {
			/* Separator or padding: consume without an action. */
			_menu_bar.close ();
		} else {
			_menu_bar.close ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	{
		const int mi = _menu_bar.menu_at (x, y);
		if (mi >= 0) {
			if (mi == 5) {
				toggle_help ();
			} else {
				_menu_bar.toggle_menu (mi);
			}
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
	}
	if (hit_status_vst3_bypass (x, y)) {
		if (_engine.is_linked ()) {
			(void)_engine.toggle_vst3_bypass ();
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
			sync_prefs_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}
	{
		const int sri = hit_status_vst3_param_reset (x, y);
		if (sri >= 0) {
			if (_engine.is_linked ()) {
				(void)_engine.reset_vst3_param (sri);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_fx_from_engine ();
				sync_prefs_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
			return true;
		}
	}
	if (hit_status_vst3_unload (x, y)) {
		if (_engine.is_linked ()) {
			(void)_engine.unload_vst3 ();
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
			sync_prefs_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}
	if (hit_status_snap (x, y)) {
		do_cycle_snap_density ();
		return true;
	}

	TransportHit th = _transport.hit_test (x, y);
	if (_transport.overflow_open ()) {
		if (th == TransportHit::Overflow) {
			_transport.set_overflow_open (false);
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		_transport.set_overflow_open (false);
		if (th == TransportHit::None) {
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
	} else if (th == TransportHit::Overflow) {
		_transport.set_overflow_open (true);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (th != TransportHit::None) {
		_transport.set_pressed (th);
	}
	if (th == TransportHit::Help) {
		toggle_help ();
		return true;
	}
	if (th == TransportHit::Prefs) {
		toggle_prefs ();
		return true;
	}
	if (th == TransportHit::Spectrum) {
		if (_engine.is_linked ()) {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				_engine.toggle_spectrum_master ();
			} else {
				_engine.toggle_spectrum_enabled ();
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			layout_panes ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (th == TransportHit::Phase) {
		if (_engine.is_linked ()) {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				_engine.toggle_correlation_master ();
			} else {
				_engine.toggle_correlation_enabled ();
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			layout_panes ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (th == TransportHit::Loudness) {
		if (_engine.is_linked ()) {
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				_engine.toggle_loudness_master ();
			} else {
				_engine.toggle_loudness_enabled ();
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			layout_panes ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (th == TransportHit::About) {
		toggle_about ();
		return true;
	}
	if (th == TransportHit::Play) {
		transport_play ();
		return true;
	}
	if (th == TransportHit::Stop) {
		transport_stop ();
		return true;
	}
	if (th == TransportHit::Rec) {
		transport_toggle_record ();
		return true;
	}
	if (th == TransportHit::Import) {
		prompt_import_audio ();
		return true;
	}
	if (th == TransportHit::Save) {
		do_save_session ();
		return true;
	}
	if (th == TransportHit::Open) {
		toggle_recent_menu ();
		return true;
	}
	if (th == TransportHit::Export) {
		prompt_export_wav ();
		return true;
	}
	if (th == TransportHit::Loop) {
		do_toggle_loop ();
		return true;
	}
	if (th == TransportHit::Punch) {
		do_toggle_punch ();
		return true;
	}
	if (th == TransportHit::Mark) {
		do_add_marker_at_playhead ();
		return true;
	}
	if (th == TransportHit::Split) {
		do_split_at_playhead ();
		return true;
	}
	if (th == TransportHit::Fade) {
		do_apply_crossfade ();
		return true;
	}
	if (th == TransportHit::Stretch) {
		do_stretch_selection ((GetKeyState (VK_SHIFT) & 0x8000) ? 0.75f : 1.5f);
		return true;
	}
	if (th == TransportHit::PitchUp) {
		do_pitch_shift_selection (1.f);
		return true;
	}
	if (th == TransportHit::PitchDown) {
		do_pitch_shift_selection (-1.f);
		return true;
	}
	if (th == TransportHit::Reverse) {
		do_reverse_selection ();
		return true;
	}
	if (th == TransportHit::Normalize) {
		do_normalize_selection (-1.f);
		return true;
	}
	if (th == TransportHit::StripSilence) {
		do_strip_silence_selection ();
		return true;
	}
	if (th == TransportHit::Consolidate) {
		do_consolidate_selection ();
		return true;
	}
	if (th == TransportHit::Freeze) {
		do_toggle_freeze_track ();
		return true;
	}
	if (th == TransportHit::Snap) {
		if (GetKeyState (VK_SHIFT) & 0x8000) {
			do_cycle_snap_density ();
		} else {
			do_toggle_snap ();
		}
		return true;
	}
	if (th == TransportHit::Quantize) {
		do_quantize_midi_notes ();
		return true;
	}
	if (th == TransportHit::MidiTransUp) {
		do_transpose_midi_notes ((GetKeyState (VK_SHIFT) & 0x8000) ? 12 : 1);
		return true;
	}
	if (th == TransportHit::MidiTransDown) {
		do_transpose_midi_notes ((GetKeyState (VK_SHIFT) & 0x8000) ? -12 : -1);
		return true;
	}
	if (th == TransportHit::MidiVelUp) {
		if (GetKeyState (VK_MENU) & 0x8000) {
			/* Alt+click V+ = force velocity 100. */
			do_force_midi_velocity (100);
		} else {
			do_nudge_midi_velocity ((GetKeyState (VK_SHIFT) & 0x8000) ? 8 : 1);
		}
		return true;
	}
	if (th == TransportHit::MidiVelDown) {
		if (GetKeyState (VK_MENU) & 0x8000) {
			/* Alt+click V− = force velocity 64. */
			do_force_midi_velocity (64);
		} else {
			do_nudge_midi_velocity ((GetKeyState (VK_SHIFT) & 0x8000) ? -8 : -1);
		}
		return true;
	}
	if (th == TransportHit::MidiLenUp || th == TransportHit::MidiLenDown) {
		float unit = _engine.is_linked () ? _engine.snap_unit_sec () : 0.5f;
		if (!_engine_snap.snap_enabled) {
			unit = 0.05f;
		}
		if (GetKeyState (VK_SHIFT) & 0x8000) {
			unit *= 4.f;
		}
		do_nudge_midi_length (th == TransportHit::MidiLenUp ? unit : -unit);
		return true;
	}
	if (th == TransportHit::MidiHumanize) {
		do_humanize_midi_notes ((GetKeyState (VK_SHIFT) & 0x8000) ? 2.f : 1.f);
		return true;
	}
	if (th == TransportHit::MidiStep) {
		do_toggle_midi_step ();
		return true;
	}
	if (th == TransportHit::MidiChord) {
		do_cycle_midi_chord ();
		return true;
	}
	if (th == TransportHit::MidiArp) {
		if (GetKeyState (VK_SHIFT) & 0x8000) {
			do_cycle_midi_arp_mode ();
		} else {
			do_toggle_midi_arp ();
		}
		return true;
	}
	if (th == TransportHit::MidiScale) {
		if (GetKeyState (VK_CONTROL) & 0x8000) {
			do_cycle_midi_scale_root ();
		} else if (GetKeyState (VK_SHIFT) & 0x8000) {
			do_cycle_midi_scale_kind ();
		} else {
			do_toggle_midi_scale ();
		}
		return true;
	}
	if (th == TransportHit::MidiPanic) {
		do_midi_panic ();
		return true;
	}
	if (th == TransportHit::MidiPgmUp) {
		do_nudge_midi_program (1);
		return true;
	}
	if (th == TransportHit::MidiPgmDown) {
		do_nudge_midi_program (-1);
		return true;
	}
	if (th == TransportHit::MidiBankUp) {
		do_nudge_midi_bank (1);
		return true;
	}
	if (th == TransportHit::MidiBankDown) {
		do_nudge_midi_bank (-1);
		return true;
	}
	if (th == TransportHit::CountIn) {
		do_toggle_count_in ();
		return true;
	}
	if (th == TransportHit::Click) {
		if (GetKeyState (VK_SHIFT) & 0x8000) {
			/* Shift+click Click → cycle metronome level −12/−6/0/+6. */
			do_cycle_click_gain ();
		} else {
			do_toggle_click ();
		}
		return true;
	}
	if (th == TransportHit::TempoDown) {
		do_nudge_tempo (-1.f);
		return true;
	}
	if (th == TransportHit::TempoUp) {
		do_nudge_tempo (1.f);
		return true;
	}
	if (th == TransportHit::AddAudio) {
		do_add_audio_track ();
		return true;
	}
	if (th == TransportHit::AddMidi) {
		do_add_midi_track ();
		return true;
	}
	if (th == TransportHit::RemoveTrack) {
		do_remove_track ();
		return true;
	}
	if (th == TransportHit::TrackUp) {
		do_move_audio_track (-1);
		return true;
	}
	if (th == TransportHit::TrackDown) {
		do_move_audio_track (+1);
		return true;
	}
	if (th == TransportHit::Group) {
		if ((GetKeyState (VK_MENU) & 0x8000)
		    && !(GetKeyState (VK_SHIFT) & 0x8000)) {
			/* Alt+click Grp → toggle relative gain linking. */
			do_toggle_group_relative ();
		} else {
			do_apply_or_clear_group ();
		}
		return true;
	}
	if (th == TransportHit::FitSession) {
		do_fit_session ();
		return true;
	}
	if (th == TransportHit::FitSelection) {
		do_fit_selection ();
		return true;
	}
	if (th == TransportHit::FitRegions) {
		do_fit_all_regions ();
		return true;
	}
	if (th == TransportHit::Shuttle) {
		_drag = DragMode::Shuttle;
		_drag_index = -1;
		_pan_down_x = x;
		_pan_down_y = y;
		_pan_last_x = x;
		float speed = _transport.shuttle_speed_at_x (x);
		float norm = 0.f;
		float w = _transport.shuttle_btn ().right - _transport.shuttle_btn ().left;
		if (w > 1.f) {
			norm = ((x - _transport.shuttle_btn ().left) / w - 0.5f) * 2.f;
			norm = std::max (-1.f, std::min (1.f, norm));
		}
		_transport.set_shuttle_norm (norm);
		if (_engine.is_linked () && _engine_snap.session_ok) {
			_engine.set_transport_speed ((double)speed);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			_playhead_sec = _engine_snap.transport_sec;
			_transport_playing = _engine_snap.transport_rolling;
			_transport.set_playing (_transport_playing);
			sync_transport_clock_from_engine ();
		}
		SetCapture (_hwnd);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int hist_row = -1;
	HistoryHit hh = _history.hit_test (x, y, &hist_row);
	if (hh == HistoryHit::UndoBtn) {
		do_undo ();
		return true;
	}
	if (hh == HistoryHit::RedoBtn || hh == HistoryHit::RedoNext) {
		do_redo ();
		return true;
	}
	if (hh == HistoryHit::UndoRow && hist_row >= 0) {
		do_undo_to_index (hist_row);
		return true;
	}

	MonitorHit mh = _monitor.hit_test (x, y);
	if (mh == MonitorHit::Dim) {
		if ((GetKeyState (VK_SHIFT) & 0x8000)
		    && _engine.is_linked () && _engine_snap.session_ok) {
			/* Shift+click Dim → cycle dim depth −20/−12/−6. */
			_engine.cycle_monitor_dim_level ();
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_monitor_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		} else {
			do_toggle_monitor_dim ();
		}
		return true;
	}
	if (mh == MonitorHit::Mono) {
		do_toggle_monitor_mono ();
		return true;
	}
	if (mh == MonitorHit::Mute) {
		do_toggle_monitor_cut ();
		return true;
	}
	if (mh == MonitorHit::Boost) {
		do_cycle_monitor_solo_boost ();
		return true;
	}
	if (mh == MonitorHit::AuxReturn) {
		_drag = DragMode::AuxReturn;
		_drag_index = -1;
		if (_monitor.drag_aux_return_to (y)) {
			do_set_aux_return (_monitor.aux_return ());
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZENS));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (mh == MonitorHit::MasterLim) {
		do_toggle_master_fx ();
		return true;
	}

	if (_editor_scroll_max > 0.5f
	    && x >= _editor_scrollbar.left - _ui.dp (3.f)
	    && x <= _editor_scrollbar.right + _ui.dp (3.f)
	    && y >= _editor_scrollbar.top && y <= _editor_scrollbar.bottom) {
		const float track_h = _editor_scrollbar.bottom - _editor_scrollbar.top;
		const float view = std::max (1.f, _editor_lanes_bot - _editor_lanes_top);
		float thumb_h = std::max (24.f * _dpi_scale,
		                          view * (view / (view + _editor_scroll_max)));
		thumb_h = std::min (thumb_h, track_h);
		const float travel = std::max (1.f, track_h - thumb_h);
		const float norm = std::max (0.f, std::min (1.f,
		                          (y - _editor_scrollbar.top - thumb_h * 0.5f) / travel));
		_editor_scroll_y = norm * _editor_scroll_max;
		_editor_scroll_focus = _focus_audio_track;
		_drag = DragMode::EditorScroll;
		_drag_index = -1;
		_pan_down_y = y;
		_pan_last_y = y;
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZENS));
		layout_panes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (hit_editor_ruler (x, y) && _engine.is_linked () && _engine_snap.session_ok) {
		float time_sec = _lanes[0].strip.time_at_x (_editor_ruler, x);
		_drag = DragMode::RulerScrub;
		_drag_index = -1;
		_pan_down_x = x;
		_pan_down_y = y;
		_pan_last_x = x;
		/* Click locate with mark snap; drag continues via scrub_to. */
		_engine.locate_to (time_sec, true /* snap to marker */, 0.4f);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		SetCapture (_hwnd);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (hit_midi_lane (x, y) && _engine.is_linked () && _engine_snap.session_ok) {
		_engine.ensure_midi_region ();
		float time_sec = midi_time_at_x (x);
		if (hit_midi_velocity_lane (x, y)) {
			/* Velocity lane: chord-aware pick by bar height under (x,y); Shift adds. */
			const bool additive = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
			int pi = pick_midi_velocity_note_at (x, y);
			if (pi >= 0 && pi < _engine_snap.midi_note_count) {
				const EngineMidiNoteSnap& n = _engine_snap.midi_notes[pi];
				_engine.select_midi_note_at (n.start_sec + n.length_sec * 0.5f, n.pitch, 0.f, additive);
			} else if (_engine_snap.midi_selected_note_id < 0
			           && _engine_snap.midi_note_count > 0) {
				/* No selection → edit all on drag apply. */
			}
			(void)time_sec;
			_drag = DragMode::MidiVelocity;
			_drag_index = -1;
			_midi_vel_drag = midi_velocity_at_y (y);
			_pan_down_x = x;
			_pan_down_y = y;
			_pan_last_x = x;
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			SetCapture (_hwnd);
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		uint8_t pitch = midi_pitch_at_y (y);
		/* Prefer trim handles on note edges (same pad as audio region trim). */
		{
			const float pad = std::max (7.f, 9.f * _dpi_scale);
			int best_edge = -1; /* 0=start 1=end */
			int best_i = -1;
			float best_d = pad + 1.f;
			for (int i = 0; i < _engine_snap.midi_note_count; ++i) {
				const EngineMidiNoteSnap& n = _engine_snap.midi_notes[i];
				if (n.pitch != pitch) {
					continue;
				}
				float x0 = _lanes[0].strip.x_at_time (_midi_lane, n.start_sec);
				float x1 = _lanes[0].strip.x_at_time (_midi_lane, n.start_sec + n.length_sec);
				if (x1 - x0 < 2.f) {
					x1 = x0 + 2.f;
				}
				float ds = std::fabs (x - x0);
				float de = std::fabs (x - x1);
				if (ds <= pad && ds <= best_d) {
					best_d = ds;
					best_edge = 0;
					best_i = i;
				}
				if (de <= pad && de < best_d) {
					best_d = de;
					best_edge = 1;
					best_i = i;
				}
			}
			if (best_i >= 0 && best_edge >= 0) {
				const EngineMidiNoteSnap& n = _engine_snap.midi_notes[best_i];
				/* Trim always promotes the edged note as primary (no toggle). */
				_engine.select_midi_note_at (n.start_sec + n.length_sec * 0.5f, n.pitch,
				                            0.f, false);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				_drag = (best_edge == 0) ? DragMode::MidiTrimStart : DragMode::MidiTrimEnd;
				_drag_index = -1;
				_midi_trim_start_sec = n.start_sec;
				_midi_trim_end_sec = n.start_sec + n.length_sec;
				_pan_down_x = x;
				_pan_down_y = y;
				_pan_last_x = x;
				SetCapture (_hwnd);
				SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
				InvalidateRect (_hwnd, nullptr, FALSE);
				return true;
			}
		}
		{
			const bool additive = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
			if (_engine.select_midi_note_at (time_sec, pitch, 0.f, additive)) {
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				if (additive) {
					InvalidateRect (_hwnd, nullptr, FALSE);
					return true;
				}
				/* Body drag relocates selected note in time + pitch. */
				float origin_sec = time_sec;
				int origin_pitch = (int)pitch;
				for (int i = 0; i < _engine_snap.midi_note_count; ++i) {
					const EngineMidiNoteSnap& n = _engine_snap.midi_notes[i];
					if (n.selected) {
						origin_sec = n.start_sec;
						origin_pitch = (int)n.pitch;
						break;
					}
				}
				_drag = DragMode::MidiMove;
				_drag_index = -1;
				_midi_move_origin_sec = origin_sec;
				_midi_move_origin_pitch = origin_pitch;
				_midi_move_preview_sec = origin_sec;
				_midi_move_preview_pitch = origin_pitch;
				_pan_down_x = x;
				_pan_down_y = y;
				_pan_last_x = x;
				SetCapture (_hwnd);
				SetCursor (LoadCursorW (nullptr, IDC_SIZEALL));
				InvalidateRect (_hwnd, nullptr, FALSE);
				return true;
			}
		}
		if (_engine_snap.midi_step_enabled) {
			/* Step mode: empty cell places pitch at playhead and advances. */
			do_step_input_pitch (pitch);
			return true;
		}
		if ((GetKeyState (VK_SHIFT) & 0x8000) != 0) {
			/* Shift+drag empty → marquee box-select (Ctrl keeps additive). */
			_drag = DragMode::MidiMarquee;
			_drag_index = -1;
			_midi_marquee_t0 = time_sec;
			_midi_marquee_p0 = pitch;
			_pan_down_x = x;
			_pan_down_y = y;
			_pan_last_x = x;
			_pan_last_y = y;
			SetCapture (_hwnd);
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		/* Empty cell → start draw (finalize length on mouse-up). */
		_engine.clear_midi_note_selection ();
		_drag = DragMode::MidiDraw;
		_drag_index = -1;
		_midi_draw_start_sec = time_sec;
		_midi_draw_pitch = pitch;
		_pan_down_x = x;
		_pan_down_y = y;
		_pan_last_x = x;
		SetCapture (_hwnd);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int lab = hit_mixer_label (x, y);
	if (lab >= 0) {
		const int mi = midi_mixer_index ();
		if (lab == mi) {
			/* Double-click rename is handled in WM_LBUTTONDBLCLK. */
			return true;
		}
		if (lab < n_audio_lanes ()) {
			if ((GetKeyState (VK_MENU) & 0x8000)
			    && (GetKeyState (VK_SHIFT) & 0x8000)
			    && !(GetKeyState (VK_CONTROL) & 0x8000)
			    && _engine.is_linked () && _engine_snap.session_ok) {
				/* Alt+Shift+click label → toggle Route comment. */
				_focus_audio_track = lab;
				_engine.toggle_track_comment (lab);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_comment_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
				return true;
			}
			if ((GetKeyState (VK_MENU) & 0x8000)
			    && (GetKeyState (VK_SHIFT) & 0x8000)
			    && (GetKeyState (VK_CONTROL) & 0x8000)
			    && _engine.is_linked () && _engine_snap.session_ok) {
				/* Ctrl+Alt+Shift+click label — toggle Route::active (group shares). */
				_focus_audio_track = lab;
				do_toggle_focused_track_active ();
				return true;
			}
			if ((GetKeyState (VK_MENU) & 0x8000)
			    && _engine.is_linked () && _engine_snap.session_ok) {
				/* Alt+click label → toggle polarity invert. */
				_focus_audio_track = lab;
				_engine.toggle_track_phase_invert (lab);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_phase_from_engine ();
				InvalidateRect (_hwnd, nullptr, FALSE);
				return true;
			}
			if (GetKeyState (VK_SHIFT) & 0x8000) {
				do_toggle_group_pick (lab);
				return true;
			}
			_focus_audio_track = lab;
			refresh_automation_lane ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			if (_engine.is_linked () && _engine_snap.session_ok) {
				_engine.select_audio_track (lab);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_group_from_engine ();
			}
			_drag = DragMode::TrackReorderPending;
			_drag_index = lab;
			_pan_down_x = x;
			_pan_down_y = y;
			_pan_last_x = x;
			SetCapture (_hwnd);
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
	}

	int ed_lab = hit_editor_lane_label (x, y);
	if (ed_lab >= 0) {
		_focus_audio_track = ed_lab;
		refresh_automation_lane ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int arm = hit_mixer_arm (x, y);
	if (arm >= 0) {
		if (_engine.is_linked ()) {
			const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
			const bool shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
			if (!_engine.start ()) {
				_engine_snap = _engine.snapshot ();
			} else if (alt && shift) {
				/* Alt+Shift+click Arm → arm every audio track. */
				_engine.arm_all_tracks ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_arm_from_engine ();
			} else if (alt) {
				/* Alt+click Arm → disarm every audio track. */
				_engine.disarm_all_tracks ();
			} else if ((GetKeyState (VK_CONTROL) & 0x8000) != 0
			           && arm < EngineSnapshot::kMeters) {
				/* Ctrl+click Arm → record-safe latch. */
				_engine.toggle_track_record_safe (arm);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_arm_from_engine ();
			} else {
				if (arm == midi_mixer_index ()) {
					_engine.toggle_midi_armed ();
				} else {
					_engine.toggle_track_armed (arm);
				}
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_arm_from_engine ();
			}
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	
	int mon = hit_mixer_mon (x, y);
	if (mon >= 0) {
		if (_engine.is_linked () && _engine.snapshot ().session_ok) {
			_engine.cycle_track_monitor_choice (mon);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_monitor_from_engine ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int mute_i = hit_mixer_mute (x, y);
	if (mute_i >= 0) {
		if (_engine.is_linked () && _engine.snapshot ().session_ok) {
			if ((GetKeyState (VK_CONTROL) & 0x8000) != 0
			    && mute_i != midi_mixer_index ()
			    && mute_i < EngineSnapshot::kMeters) {
				/* Ctrl+click Mute → cycle mute points Main↔AllPoints. */
				_engine.cycle_track_mute_points (mute_i);
			} else if (mute_i == midi_mixer_index ()) {
				_engine.toggle_midi_muted ();
			} else {
				_engine.toggle_track_muted (mute_i);
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_mute_solo_from_engine ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int solo_i = hit_mixer_solo (x, y);
	if (solo_i >= 0) {
		if (_engine.is_linked () && _engine.snapshot ().session_ok) {
			const bool shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
			const bool ctrl = (GetKeyState (VK_CONTROL) & 0x8000) != 0;
			const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
			if (shift && solo_i != midi_mixer_index ()) {
				/* Shift+click Solo → toggle solo-isolate. */
				_engine.toggle_track_solo_isolated (solo_i);
			} else if (ctrl && solo_i != midi_mixer_index ()) {
				/* Ctrl+click Solo → toggle solo-safe. */
				_engine.toggle_track_solo_safe (solo_i);
			} else if (solo_i == midi_mixer_index ()) {
				if (alt) {
					_engine.exclusive_solo_midi ();
				} else {
					_engine.toggle_midi_soloed ();
				}
			} else if (alt) {
				/* Alt+click Solo → exclusive solo. */
				_engine.exclusive_solo_track (solo_i);
			} else {
				_engine.toggle_track_soloed (solo_i);
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			if (shift) {
				sync_mixer_solo_isolate_from_engine ();
			} else if (ctrl) {
				sync_mixer_solo_safe_from_engine ();
			} else {
				sync_mixer_mute_solo_from_engine ();
			}
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	
	int trim_i = hit_mixer_trim (x, y);
	if (trim_i >= 0) {
		_drag = DragMode::Trim;
		_drag_index = trim_i;
		if (_mixers[trim_i].drag_trim_to (x)) {
			_engine.set_track_trim_norm (trim_i, _mixers[trim_i].trim ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_trim_from_engine ();
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int pan_i = hit_mixer_pan (x, y);
	if (pan_i >= 0) {
		const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
		const bool shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
		if (alt && _engine.is_linked () && _engine.snapshot ().session_ok) {
			/* Alt+click pan → center every audio strip. */
			_engine.reset_all_pans ();
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_pan_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		if (shift && _mixers[pan_i].pan_width_ok ()) {
			/* Shift+drag pan → stereo width (−1..+1 via 0..1 iface). */
			_drag = DragMode::PanWidth;
			_drag_index = pan_i;
			if (_mixers[pan_i].drag_pan_width_to (x)) {
				_engine.set_track_pan_width (pan_i, _mixers[pan_i].pan_width ());
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_pan_from_engine ();
			}
		} else {
			_drag = DragMode::Pan;
			_drag_index = pan_i;
			if (_mixers[pan_i].drag_pan_to (x)) {
				_engine.set_track_pan (pan_i, _mixers[pan_i].pan ());
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_pan_from_engine ();
			}
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int send_i = hit_mixer_send (x, y);
	if (send_i >= 0) {
		const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
		if (alt && _engine.is_linked () && _engine.snapshot ().session_ok) {
			/* Alt+click send → zero every aux send. */
			_engine.reset_all_sends ();
		}
		/* Ctrl+click send → mute/unmute (Processor deactivate).
		 * Alt+click remains send-reset-all on tip #38. */
		if ((GetKeyState (VK_CONTROL) & 0x8000) != 0
		    && _engine.is_linked () && _engine_snap.session_ok) {
			_engine.toggle_track_send_muted (send_i);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_send_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		const bool shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
		if (shift && _mixers[send_i].send_pan_ok ()) {
			/* Shift+drag send → aux send pan azimuth. */
			_drag = DragMode::SendPan;
			_drag_index = send_i;
			if (_mixers[send_i].drag_send_pan_to (x)) {
				_engine.set_track_send_pan (send_i, _mixers[send_i].send_pan ());
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_send_from_engine ();
			}
		} else {
			_drag = DragMode::Send;
			_drag_index = send_i;
			if (_mixers[send_i].drag_send_to (x)) {
				_engine.set_track_send (send_i, _mixers[send_i].send ());
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_send_from_engine ();
			}
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	/* Ctrl+click meter → Peak↔K14. Shift+click meter → cycle meter point.
	 * Alt+click → clear every strip's clip latch.
	 * Plain click (when clipped) → clear peak-hold clip latch. */
	int meter_i = hit_mixer_meter (x, y);
	if (meter_i >= 0 && (GetKeyState (VK_CONTROL) & 0x8000) != 0
	    && meter_i < EngineSnapshot::kMeters
	    && _engine.is_linked () && _engine_snap.session_ok
	    && !_mixers[meter_i].is_midi_strip ()) {
		_engine.cycle_track_meter_type (meter_i);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_mixers[meter_i].set_meter_type (_engine_snap.track_meter_type[meter_i]);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (meter_i >= 0 && (GetKeyState (VK_SHIFT) & 0x8000) != 0
	    && meter_i < EngineSnapshot::kMeters
	    && _engine.is_linked () && _engine_snap.session_ok
	    && !_mixers[meter_i].is_midi_strip ()) {
		_engine.cycle_track_meter_point (meter_i);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_mixers[meter_i].set_meter_point (_engine_snap.track_meter_point[meter_i]);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (meter_i >= 0 && (GetKeyState (VK_MENU) & 0x8000) != 0) {
		for (int i = 0; i < kMixerStripsMax; ++i) {
			_mixers[i].clear_meter_clip ();
		}
		if (_engine.is_linked () && _engine.snapshot ().session_ok) {
			_engine.clear_all_meter_clips ();
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	/* Plain click clears clip only on the LED band — body clicks fall through
	 * so the fader pad beside the meter stays usable. */
	if (meter_i >= 0 && _mixers[meter_i].meter_clipped ()
	    && _mixers[meter_i].hit_meter_clip_led (x, y)) {
		_mixers[meter_i].clear_meter_clip ();
		if (meter_i < EngineSnapshot::kMeters && _engine.is_linked ()
		    && _engine.snapshot ().session_ok) {
			_engine.clear_track_meter_clip (meter_i);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int fxpi3 = hit_mixer_fx_param3 (x, y);
	if (fxpi3 >= 0 && _mixers[fxpi3].has_fx () && _mixers[fxpi3].fx_param_count () >= 3) {
		_drag = DragMode::FxParam3;
		_drag_index = fxpi3;
		if (_mixers[fxpi3].drag_fx_param3_to (x)) {
			if (fxpi3 == 1 && _engine_snap.vst3_ok && _engine_snap.vst3_param_count > 2) {
				_engine.set_vst3_param (2, _mixers[fxpi3].fx_param3 ());
			} else {
				_engine.set_track_eq_gain_h_norm (fxpi3, _mixers[fxpi3].fx_param3 ());
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int fxpi2 = hit_mixer_fx_param2 (x, y);
	if (fxpi2 >= 0 && _mixers[fxpi2].has_fx () && _mixers[fxpi2].fx_param_count () >= 2) {
		_drag = DragMode::FxParam2;
		_drag_index = fxpi2;
		if (_mixers[fxpi2].drag_fx_param2_to (x)) {
			if (fxpi2 == 1 && _engine_snap.vst3_ok && _engine_snap.vst3_param_count > 1) {
				_engine.set_vst3_param (1, _mixers[fxpi2].fx_param2 ());
			} else if (fxpi2 == 5 && _engine_snap.track_gate_ok) {
				_engine.set_track_gate_hold_norm (fxpi2, _mixers[fxpi2].fx_param2 ());
			} else if (fxpi2 == 4 && _engine_snap.track_limiter_ok) {
				_engine.set_track_limiter_release_norm (fxpi2, _mixers[fxpi2].fx_param2 ());
			} else if (fxpi2 == 3 && _engine_snap.track_delay_ok) {
				_engine.set_track_delay_feedback_norm (fxpi2, _mixers[fxpi2].fx_param2 ());
			} else if (fxpi2 == 2 && _engine_snap.track_eq_ok) {
				_engine.set_track_eq_gain_m_norm (fxpi2, _mixers[fxpi2].fx_param2 ());
			} else {
				_engine.set_track_comp_ratio_norm (fxpi2, _mixers[fxpi2].fx_param2 ());
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int fxpi = hit_mixer_fx_param (x, y);
	if (fxpi >= 0 && _mixers[fxpi].has_fx ()) {
		_drag = DragMode::FxParam;
		_drag_index = fxpi;
		if (_mixers[fxpi].drag_fx_param_to (x)) {
			if (fxpi == 1 && _engine_snap.vst3_ok) {
				_engine.set_vst3_param (0, _mixers[fxpi].fx_param ());
			} else if (fxpi == 5 && _engine_snap.track_gate_ok) {
				_engine.set_track_gate_threshold_norm (fxpi, _mixers[fxpi].fx_param ());
			} else if (fxpi == 4 && _engine_snap.track_limiter_ok) {
				_engine.set_track_limiter_ceiling_norm (fxpi, _mixers[fxpi].fx_param ());
			} else if (fxpi == 3 && _engine_snap.track_delay_ok) {
				_engine.set_track_delay_time_norm (fxpi, _mixers[fxpi].fx_param ());
			} else if (fxpi == 2 && _engine_snap.track_eq_ok) {
				_engine.set_track_eq_gain_l_norm (fxpi, _mixers[fxpi].fx_param ());
			} else if (fxpi == 1 && _engine_snap.track_comp_ok) {
				_engine.set_track_comp_threshold_norm (fxpi, _mixers[fxpi].fx_param ());
			} else {
				_engine.set_track_fx_param (fxpi, _mixers[fxpi].fx_param ());
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	for (int ui = 0; ui < kTracks; ++ui) {
		if (_mixers[ui].hit_fx_bypass (x, y)) {
			if (_engine.is_linked () && _engine.snapshot ().session_ok) {
				(void)_engine.toggle_vst3_bypass ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_fx_from_engine ();
			}
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		const int ri = _mixers[ui].hit_fx_param_reset (x, y);
		if (ri >= 0) {
			if (_engine.is_linked () && _engine.snapshot ().session_ok) {
				(void)_engine.reset_vst3_param (ri);
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_fx_from_engine ();
			}
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		if (_mixers[ui].hit_fx_unload (x, y)) {
			if (_engine.is_linked () && _engine.snapshot ().session_ok) {
				(void)_engine.unload_vst3 ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_fx_from_engine ();
			}
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
	}

	int fxi = hit_mixer_fx (x, y);
	if (fxi >= 0) {
		if (_engine.is_linked () && _engine.snapshot ().session_ok) {
			const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
			const bool shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
			if (alt && shift) {
				/* Alt+Shift+click FX → activate every shell ACE insert. */
				_engine.set_all_fx_bypassed (false);
			} else if (alt) {
				/* Alt+click FX → bypass every shell ACE insert. */
				_engine.set_all_fx_bypassed (true);
			} else if (fxi == 5) {
				if (_mixers[fxi].has_fx ()) {
					_engine.toggle_track_gate_bypass (fxi);
				} else {
					_engine.insert_track_gate (fxi);
				}
			} else if (fxi == 4) {
				if (_mixers[fxi].has_fx ()) {
					_engine.toggle_track_limiter_bypass (fxi);
				} else {
					_engine.insert_track_limiter (fxi);
				}
			} else if (fxi == 3) {
				if (_mixers[fxi].has_fx ()) {
					_engine.toggle_track_delay_bypass (fxi);
				} else {
					_engine.insert_track_delay (fxi);
				}
			} else if (fxi == 2) {
				if (_mixers[fxi].has_fx ()) {
					_engine.toggle_track_eq_bypass (fxi);
				} else {
					_engine.insert_track_eq (fxi);
				}
			} else if (fxi == 1) {
				if (_engine_snap.vst3_ok && _mixers[fxi].has_fx ()) {
					/* Live VST3 on Audio 2 — click toggles bypass (no Comp SC). */
					_engine.toggle_vst3_bypass ();
				} else if (_mixers[fxi].has_fx ()) {
					if (GetKeyState (VK_SHIFT) & 0x8000) {
						bool on = !_engine_snap.track_comp_sidechain;
						_engine.set_track_comp_sidechain (1, 0, on);
					} else {
						_engine.toggle_track_comp_bypass (fxi);
					}
				} else {
					_engine.insert_track_comp (fxi);
				}
			} else if (_mixers[fxi].has_fx ()) {
				_engine.toggle_track_fx_bypass (fxi);
			} else {
				_engine.insert_track_fx (fxi);
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

		int mi = hit_mixer_fader (x, y);
	if (mi >= 0) {
		const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
		if (alt && _engine.is_linked () && _engine.snapshot ().session_ok) {
			/* Alt+click fader → all strips to unity. */
			_engine.reset_all_faders ();
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_gains_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		_drag = DragMode::Fader;
		_drag_index = mi;
		_mixers[mi].drag_to (y);
		if (mi == midi_mixer_index ()) {
			_engine.set_midi_gain (_mixers[mi].gain ());
		} else {
			_engine.set_track_gain (mi, _mixers[mi].gain ());
			if (mi == 0) {
				_engine_snap.gain = _mixers[0].gain ();
			}
		}
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_SIZENS));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_auto_lane.hit (_auto_lane_bounds, x, y)
	    && _engine.is_linked () && _engine_snap.session_ok) {
		if (GetKeyState (VK_SHIFT) & 0x8000) {
			/* Shift+click auto lane → cycle Gain → Pan → Send. */
			cycle_auto_lane_kind ();
			return true;
		}
		_drag = DragMode::AutoWrite;
		_drag_index = auto_lane_track ();
		_auto_last_write_sec = -1.f;
		if (_auto_lane_kind == 1) {
			_engine.ensure_track_pan_automation (_drag_index);
		} else if (_auto_lane_kind == 2) {
			_engine.ensure_track_send_automation (_drag_index);
		} else {
			_engine.ensure_track_gain_automation (_drag_index);
		}
		write_automation_at (x, y);
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_CROSS));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_midi_cc_lane.hit (_midi_cc_lane_bounds, x, y)
	    && _engine.is_linked () && _engine_snap.session_ok) {
		/* Alt+click clears the active CC/bend/pressure/PolyAT lane. */
		if ((GetKeyState (VK_MENU) & 0x8000) != 0) {
			do_clear_active_midi_cc_lane ();
			return true;
		}
		_drag = DragMode::MidiCcWrite;
		_drag_index = 0;
		_midi_cc_last_write_sec = -1.f;
		if (_midi_cc_number == -1) {
			_engine.ensure_midi_bend_automation ();
		} else if (_midi_cc_number == -2) {
			_engine.ensure_midi_pressure_automation ();
		} else if (_midi_cc_number == -3) {
			_engine.ensure_midi_note_pressure_automation ();
		} else {
			_engine.ensure_midi_cc_automation (_midi_cc_number);
		}
		write_midi_cc_at (x, y);
		SetCapture (_hwnd);
		SetCursor (LoadCursorW (nullptr, IDC_CROSS));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	int lane = hit_editor_lane (x, y);
	if (lane >= 0) {
		const bool shift = (GetKeyState (VK_SHIFT) & 0x8000) != 0;
		const bool ctrl = (GetKeyState (VK_CONTROL) & 0x8000) != 0;
		const bool alt = (GetKeyState (VK_MENU) & 0x8000) != 0;
		if (_engine.is_linked () && _engine_snap.session_ok
		    && (shift || ctrl || alt)) {
			float time_sec = _lanes[lane].strip.time_at_x (_lanes[lane].bounds, x);
			if (shift) {
				_engine.set_loop_start (time_sec);
			} else if (ctrl) {
				_engine.set_loop_end (time_sec);
			} else {
				_engine.add_marker_at (time_sec);
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_transport_loop_from_engine ();
			apply_loop_to_lanes ();
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
		/* Selected region: trim edges → fade wedges → body move → else pan/select. */
		if (_engine.is_linked () && _engine_snap.has_selection
		    && _engine_snap.selection_track == lane) {
			int te = _lanes[lane].strip.hit_trim_edge (
				_lanes[lane].bounds, x, y, _dpi_scale);
			if (te == 0 || te == 1) {
				_drag = (te == 0) ? DragMode::TrimFront : DragMode::TrimEnd;
				_drag_index = lane;
				SetCapture (_hwnd);
				SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
				InvalidateRect (_hwnd, nullptr, FALSE);
				return true;
			}
			int fh = _lanes[lane].strip.hit_fade_handle (
				_lanes[lane].bounds, x, y, _dpi_scale);
			if (fh == 0 || fh == 1) {
				_drag = (fh == 0) ? DragMode::FadeIn : DragMode::FadeOut;
				_drag_index = lane;
				SetCapture (_hwnd);
				SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
				InvalidateRect (_hwnd, nullptr, FALSE);
				return true;
			}
			int env_lane = -1;
			if (_engine_snap.region_envelope_ok
			    && hit_region_envelope_mid (x, y, &env_lane) && env_lane == lane) {
				_drag = DragMode::EnvelopeMid;
				_drag_index = lane;
				float coeff = 0.35f;
				if (envelope_mid_from_y (lane, y, &coeff)) {
					do_set_region_envelope_mid (coeff);
				}
				SetCapture (_hwnd);
				SetCursor (LoadCursorW (nullptr, IDC_SIZENS));
				InvalidateRect (_hwnd, nullptr, FALSE);
				return true;
			}
			if (_lanes[lane].strip.hit_selection_body (
				    _lanes[lane].bounds, x, y, _dpi_scale)) {
				float t = _lanes[lane].strip.time_at_x (_lanes[lane].bounds, x);
				_region_move_grab_offset_sec = t - _engine_snap.selection_start_sec;
				_drag = DragMode::RegionMovePending;
				_drag_index = lane;
				_pan_last_x = x;
				_pan_down_x = x;
				_pan_down_y = y;
				SetCapture (_hwnd);
				return true;
			}
		}
		/* Click selects a region; drag past threshold pans the timeline. */
		_drag = DragMode::WavePending;
		_drag_index = lane;
		_pan_last_x = x;
		_pan_down_x = x;
		_pan_down_y = y;
		SetCapture (_hwnd);
		return true;
	}
	return false;
}

bool
ShellWindow::on_mouse_move (LPARAM lparam)
{
	float x, y;
	client_from_lparam (lparam, &x, &y);
	_mouse_x = x;
	_mouse_y = y;
	if (!_mouse_tracking) {
		TRACKMOUSEEVENT tme = { sizeof (TRACKMOUSEEVENT), TME_LEAVE, _hwnd, 0 };
		if (TrackMouseEvent (&tme)) {
			_mouse_tracking = true;
		}
	}
	if (_transport.update_hover (x, y)) {
		InvalidateRect (_hwnd, nullptr, FALSE);
	}

	if (_menu_bar.open ()) {
		_menu_bar.set_hover (x, y);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (y < _menu_bar_bounds.bottom && _menu_bar.menu_at (x, y) >= 0) {
		_menu_bar.set_hover (x, y);
		InvalidateRect (_hwnd, nullptr, FALSE);
	}

	if (_drag == DragMode::None) {
		return false;
	}

	if (_drag == DragMode::EditorScroll) {
		const float track_h = _editor_scrollbar.bottom - _editor_scrollbar.top;
		const float view = std::max (1.f, _editor_lanes_bot - _editor_lanes_top);
		float thumb_h = std::max (24.f * _dpi_scale,
		                          view * (view / (view + _editor_scroll_max)));
		thumb_h = std::min (thumb_h, track_h);
		const float travel = std::max (1.f, track_h - thumb_h);
		const float dy = y - _pan_last_y;
		_editor_scroll_y += dy * (_editor_scroll_max / travel);
		_editor_scroll_y = std::max (0.f, std::min (_editor_scroll_max, _editor_scroll_y));
		_editor_scroll_focus = _focus_audio_track;
		_pan_last_y = y;
		layout_panes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_drag == DragMode::Fader && _drag_index >= 0 && _drag_index < n_mixer_strips ()) {
		if (_mixers[_drag_index].drag_to (y)) {
			if (_drag_index == midi_mixer_index ()) {
				_engine.set_midi_gain (_mixers[_drag_index].gain ());
			} else {
				_engine.set_track_gain (_drag_index, _mixers[_drag_index].gain ());
				_engine_snap = _engine.snapshot ();
				sync_mixer_gains_from_engine ();
			}
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::FxParam && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_fx_param_to (x)) {
			if (_drag_index == 1 && _engine_snap.vst3_ok) {
				_engine.set_vst3_param (0, _mixers[_drag_index].fx_param ());
			} else if (_drag_index == 5 && _engine_snap.track_gate_ok) {
				_engine.set_track_gate_threshold_norm (
					_drag_index, _mixers[_drag_index].fx_param ());
			} else if (_drag_index == 4 && _engine_snap.track_limiter_ok) {
				_engine.set_track_limiter_ceiling_norm (
					_drag_index, _mixers[_drag_index].fx_param ());
			} else if (_drag_index == 3 && _engine_snap.track_delay_ok) {
				_engine.set_track_delay_time_norm (
					_drag_index, _mixers[_drag_index].fx_param ());
			} else if (_drag_index == 2 && _engine_snap.track_eq_ok) {
				_engine.set_track_eq_gain_l_norm (
					_drag_index, _mixers[_drag_index].fx_param ());
			} else if (_drag_index == 1 && _engine_snap.track_comp_ok) {
				_engine.set_track_comp_threshold_norm (
					_drag_index, _mixers[_drag_index].fx_param ());
			} else {
				_engine.set_track_fx_param (_drag_index, _mixers[_drag_index].fx_param ());
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::FxParam2 && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_fx_param2_to (x)) {
			if (_drag_index == 1 && _engine_snap.vst3_ok && _engine_snap.vst3_param_count > 1) {
				_engine.set_vst3_param (1, _mixers[_drag_index].fx_param2 ());
			} else if (_drag_index == 5 && _engine_snap.track_gate_ok) {
				_engine.set_track_gate_hold_norm (
					_drag_index, _mixers[_drag_index].fx_param2 ());
			} else if (_drag_index == 4 && _engine_snap.track_limiter_ok) {
				_engine.set_track_limiter_release_norm (
					_drag_index, _mixers[_drag_index].fx_param2 ());
			} else if (_drag_index == 3 && _engine_snap.track_delay_ok) {
				_engine.set_track_delay_feedback_norm (
					_drag_index, _mixers[_drag_index].fx_param2 ());
			} else if (_drag_index == 2 && _engine_snap.track_eq_ok) {
				_engine.set_track_eq_gain_m_norm (
					_drag_index, _mixers[_drag_index].fx_param2 ());
			} else {
				_engine.set_track_comp_ratio_norm (
					_drag_index, _mixers[_drag_index].fx_param2 ());
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::FxParam3 && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_fx_param3_to (x)) {
			if (_drag_index == 1 && _engine_snap.vst3_ok && _engine_snap.vst3_param_count > 2) {
				_engine.set_vst3_param (2, _mixers[_drag_index].fx_param3 ());
			} else {
				_engine.set_track_eq_gain_h_norm (
					_drag_index, _mixers[_drag_index].fx_param3 ());
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::Trim && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_trim_to (x)) {
			_engine.set_track_trim_norm (_drag_index, _mixers[_drag_index].trim ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_trim_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::Pan && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_pan_to (x)) {
			_engine.set_track_pan (_drag_index, _mixers[_drag_index].pan ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_pan_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::PanWidth && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_pan_width_to (x)) {
			_engine.set_track_pan_width (
				_drag_index, _mixers[_drag_index].pan_width ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_pan_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::Send && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_send_to (x)) {
			_engine.set_track_send (_drag_index, _mixers[_drag_index].send ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_send_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::AuxReturn) {
		if (_monitor.drag_aux_return_to (y)) {
			do_set_aux_return (_monitor.aux_return ());
		}
		return true;
	}

	if (_drag == DragMode::SendPan && _drag_index >= 0 && _drag_index < kTracks) {
		if (_mixers[_drag_index].drag_send_pan_to (x)) {
			_engine.set_track_send_pan (
				_drag_index, _mixers[_drag_index].send_pan ());
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_send_from_engine ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
		return true;
	}

	if (_drag == DragMode::AutoWrite) {
		write_automation_at (x, y);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_drag == DragMode::MidiCcWrite) {
		write_midi_cc_at (x, y);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_drag == DragMode::EnvelopeMid && _drag_index >= 0 && _drag_index < kTracks) {
		float coeff = 0.35f;
		if (envelope_mid_from_y (_drag_index, y, &coeff)) {
			do_set_region_envelope_mid (coeff);
		}
		return true;
	}
	if ((_drag == DragMode::FadeIn || _drag == DragMode::FadeOut)
	    && _drag_index >= 0 && _drag_index < kTracks
	    && _engine_snap.has_selection) {
		float t = _lanes[_drag_index].strip.time_at_x (_lanes[_drag_index].bounds, x);
		if (_drag == DragMode::FadeIn) {
			float len = t - _engine_snap.selection_start_sec;
			_engine.set_selected_fade_in_sec (len);
		} else {
			float len = _engine_snap.selection_end_sec - t;
			_engine.set_selected_fade_out_sec (len);
		}
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if ((_drag == DragMode::TrimFront || _drag == DragMode::TrimEnd)
	    && _drag_index >= 0 && _drag_index < kTracks
	    && _engine_snap.has_selection) {
		float t = _lanes[_drag_index].strip.time_at_x (_lanes[_drag_index].bounds, x);
		if (_drag == DragMode::TrimFront) {
			_engine.trim_selected_front_to (t);
		} else {
			_engine.trim_selected_end_to (t);
		}
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_drag == DragMode::RegionMovePending && _drag_index >= 0 && _drag_index < kTracks) {
		float dx = x - _pan_down_x;
		float dy = y - _pan_down_y;
		float thresh = 4.f * _dpi_scale;
		if ((dx * dx + dy * dy) >= (thresh * thresh)) {
			_drag = DragMode::RegionMove;
			SetCursor (LoadCursorW (nullptr, IDC_SIZEALL));
		} else {
			return true;
		}
	}

	if (_drag == DragMode::RegionMove && _drag_index >= 0 && _drag_index < kTracks
	    && _engine_snap.has_selection) {
		float t = _lanes[_drag_index].strip.time_at_x (_lanes[_drag_index].bounds, x);
		float new_start = t - _region_move_grab_offset_sec;
		_engine.move_selected_to (new_start);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		apply_region_selection_to_lanes ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_drag == DragMode::TrackReorderPending && _drag_index >= 0) {
		float dx = x - _pan_down_x;
		float dy = y - _pan_down_y;
		float thresh = 6.f * _dpi_scale;
		if ((dx * dx + dy * dy) >= (thresh * thresh)) {
			_drag = DragMode::TrackReorder;
			SetCursor (LoadCursorW (nullptr, IDC_SIZENS));
		}
		return true;
	}

	if (_drag == DragMode::TrackReorder && _drag_index >= 0) {
		_pan_last_x = x;
		_pan_down_y = y; /* reuse as last y for visual only */
		return true;
	}

	if (_drag == DragMode::WavePending && _drag_index >= 0 && _drag_index < kTracks) {
		float dx = x - _pan_down_x;
		float dy = y - _pan_down_y;
		float thresh = 4.f * _dpi_scale;
		if ((dx * dx + dy * dy) >= (thresh * thresh)) {
			_drag = DragMode::WavePan;
			SetCursor (LoadCursorW (nullptr, IDC_SIZEWE));
		} else {
			return true;
		}
	}

	if (_drag == DragMode::WavePan && _drag_index >= 0 && _drag_index < kTracks) {
		const D2D1_RECT_F& r = _lanes[_drag_index].bounds;
		float w = r.right - r.left;
		if (w > 1.f) {
			float delta_widths = (_pan_last_x - x) / w;
			if (delta_widths != 0.f) {
				_lanes[_drag_index].strip.pan_by (delta_widths);
				apply_shared_view (_drag_index);
				refresh_editor_view_after_nav ();
				InvalidateRect (_hwnd, nullptr, FALSE);
			}
		}
		_pan_last_x = x;
		return true;
	}

	if (_drag == DragMode::MidiDraw || _drag == DragMode::MidiMarquee) {
		_pan_last_x = x;
		_pan_last_y = y;
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (_drag == DragMode::MidiVelocity) {
		_pan_last_x = x;
		_midi_vel_drag = midi_velocity_at_y (y);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (_drag == DragMode::MidiTrimStart || _drag == DragMode::MidiTrimEnd) {
		_pan_last_x = x;
		float t = midi_time_at_x (x);
		if (_engine_snap.snap_enabled) {
			t = _engine.snap_time (t);
		}
		if (_drag == DragMode::MidiTrimStart) {
			float end = _midi_trim_end_sec;
			if (t > end - 0.05f) {
				t = end - 0.05f;
			}
			if (t < 0.f) {
				t = 0.f;
			}
			_midi_trim_start_sec = t;
		} else {
			float start = _midi_trim_start_sec;
			if (t < start + 0.05f) {
				t = start + 0.05f;
			}
			_midi_trim_end_sec = t;
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	if (_drag == DragMode::MidiMove) {
		_pan_last_x = x;
		float t = midi_time_at_x (x);
		float grab_t = midi_time_at_x (_pan_down_x);
		float new_start = _midi_move_origin_sec + (t - grab_t);
		if (new_start < 0.f) {
			new_start = 0.f;
		}
		if (_engine_snap.snap_enabled) {
			new_start = _engine.snap_time (new_start);
			if (new_start < 0.f) {
				new_start = 0.f;
			}
		}
		int new_pitch = (int)midi_pitch_at_y (y);
		if (new_pitch < 0) {
			new_pitch = 0;
		}
		if (new_pitch > 127) {
			new_pitch = 127;
		}
		_midi_move_preview_sec = new_start;
		_midi_move_preview_pitch = new_pitch;
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_drag == DragMode::RulerScrub && _engine.is_linked () && _engine_snap.session_ok) {
		_pan_last_x = x;
		float time_sec = _lanes[0].strip.time_at_x (_editor_ruler, x);
		_engine.scrub_to (time_sec);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}

	if (_drag == DragMode::Shuttle) {
		_pan_last_x = x;
		float speed = _transport.shuttle_speed_at_x (x);
		float w = _transport.shuttle_btn ().right - _transport.shuttle_btn ().left;
		float norm = 0.f;
		if (w > 1.f) {
			norm = ((x - _transport.shuttle_btn ().left) / w - 0.5f) * 2.f;
			norm = std::max (-1.f, std::min (1.f, norm));
		}
		_transport.set_shuttle_norm (norm);
		if (_engine.is_linked () && _engine_snap.session_ok) {
			_engine.set_transport_speed ((double)speed);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			_playhead_sec = _engine_snap.transport_sec;
			_transport_playing = _engine_snap.transport_rolling;
			_transport.set_playing (_transport_playing);
			sync_transport_clock_from_engine ();
		}
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	return false;
}

void
ShellWindow::on_mouse_up ()
{
	if (_drag == DragMode::MidiMarquee && _engine.is_linked () && _engine_snap.session_ok) {
		float t1 = midi_time_at_x (_pan_last_x);
		uint8_t p1 = midi_pitch_at_y (_pan_last_y);
		const bool additive = (GetKeyState (VK_CONTROL) & 0x8000) != 0;
		_engine.select_midi_notes_in_rect (_midi_marquee_t0, t1, _midi_marquee_p0, p1,
		                                   additive);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}

	_transport.clear_pressed ();
	if (_drag == DragMode::MidiDraw && _engine.is_linked () && _engine_snap.session_ok) {
		float end_sec = midi_time_at_x (_pan_last_x);
		float start = _midi_draw_start_sec;
		float end = end_sec;
		if (end < start) {
			float tmp = start;
			start = end;
			end = tmp;
		}
		float len = end - start;
		if (len < 0.12f) {
			len = 0.25f; /* click without drag → quarter-ish note */
		}
		_engine.add_midi_note (_midi_draw_pitch, start, len, 100);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
	if (_drag == DragMode::MidiVelocity && _engine.is_linked () && _engine_snap.session_ok) {
		_engine.set_midi_velocity (_midi_vel_drag);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
	if ((_drag == DragMode::MidiTrimStart || _drag == DragMode::MidiTrimEnd)
	    && _engine.is_linked () && _engine_snap.session_ok) {
		if (_drag == DragMode::MidiTrimStart) {
			_engine.set_midi_note_start (_midi_trim_start_sec);
		} else {
			_engine.set_midi_note_end (_midi_trim_end_sec);
		}
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
	if (_drag == DragMode::MidiMove && _engine.is_linked () && _engine_snap.session_ok) {
		const float dt = _midi_move_preview_sec - _midi_move_origin_sec;
		const int dp = _midi_move_preview_pitch - _midi_move_origin_pitch;
		if (std::fabs (dt) > 1.0e-4f || dp != 0) {
			_engine.move_selected_midi_notes (dt, dp);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
	}
	if (_drag == DragMode::WavePending && _drag_index >= 0 && _drag_index < kTracks) {
		int lane = _drag_index;
		float time_sec = _lanes[lane].strip.time_at_x (_lanes[lane].bounds, _pan_down_x);
		if (_engine.is_linked () && _engine_snap.session_ok) {
			if (!_engine.select_region_at (lane, time_sec)) {
				_engine.clear_region_selection ();
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			apply_region_selection_to_lanes ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
	}
	if (_drag == DragMode::TrackReorderPending) {
		/* Click without drag — keep focus; F2 renames. */
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
	if (_drag == DragMode::TrackReorder && _drag_index >= 0
	    && _engine.is_linked () && _engine_snap.session_ok) {
		float drop_x = _pan_last_x;
		float drop_y = _pan_down_y;
		POINT pt;
		if (GetCursorPos (&pt) && ScreenToClient (_hwnd, &pt)) {
			drop_x = (float)pt.x;
			drop_y = (float)pt.y;
		}
		int from = _drag_index;
		int to = hit_mixer_label (drop_x, drop_y);
		if (to < 0) {
			/* Fall back to strip bounds under cursor. */
			const int na = n_audio_lanes ();
			for (int i = 0; i < na; ++i) {
				const D2D1_RECT_F& b = _mixers[i].bounds ();
				if (drop_x >= b.left && drop_x <= b.right
				    && drop_y >= b.top && drop_y <= b.bottom) {
					to = i;
					break;
				}
			}
		}
		if (to >= 0 && to < n_audio_lanes () && to != from) {
			int step = (to > from) ? +1 : -1;
			int cur = from;
			while (cur != to) {
				if (!_engine.move_audio_track (cur, step)) {
					break;
				}
				cur += step;
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			_focus_audio_track = cur;
			sync_track_labels_from_engine ();
			sync_mixer_gains_from_engine ();
			sync_mixer_arm_from_engine ();
			sync_mixer_mute_solo_from_engine ();
			sync_mixer_pan_from_engine ();
			sync_mixer_send_from_engine ();
			apply_region_selection_to_lanes ();
			refresh_automation_lane ();
			layout_panes ();
			InvalidateRect (_hwnd, nullptr, FALSE);
		}
	}
	/* RegionMovePending click (no drag): keep selection — no timeline pan. */
	if (_drag == DragMode::RegionMovePending) {
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
	if (_drag == DragMode::Shuttle && _engine.is_linked () && _engine_snap.session_ok) {
		_engine.stop_shuttle ();
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		_playhead_sec = _engine_snap.transport_sec;
		_transport_playing = false;
		_transport.set_playing (false);
		_transport.set_shuttle_norm (0.f);
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
	if (_drag == DragMode::RulerScrub) {
		sync_transport_clock_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
	}
	if (_drag != DragMode::None) {
		_drag = DragMode::None;
		_drag_index = -1;
		if (GetCapture () == _hwnd) {
			ReleaseCapture ();
		}
	}
}

void
ShellWindow::on_dpi_changed (UINT dpi, const RECT* suggested)
{
	_dpi = dpi ? dpi : 96;
	_dpi_scale = (float)_dpi / 96.f;
	_ui.rebuild_text_formats (_dpi_scale);
	if (suggested) {
		SetWindowPos (_hwnd, nullptr,
		              suggested->left, suggested->top,
		              suggested->right - suggested->left,
		              suggested->bottom - suggested->top,
		              SWP_NOZORDER | SWP_NOACTIVATE);
	}
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::on_resize (UINT width, UINT height)
{
	_width = width ? width : 1;
	_height = height ? height : 1;
	layout_panes ();
	if (_rt) {
		_rt->Resize (D2D1::SizeU (_width, _height));
		if (_brush_bg) {
			_brush_bg->Release ();
			_brush_bg = nullptr;
		}
		ID2D1GradientStopCollection* stops = nullptr;
		D2D1_GRADIENT_STOP gs[2];
		gs[0].position = 0.f; gs[0].color = kBgTop;
		gs[1].position = 1.f; gs[1].color = kBgBottom;
		if (SUCCEEDED (_rt->CreateGradientStopCollection (gs, 2, &stops))) {
			_rt->CreateLinearGradientBrush (
				D2D1::LinearGradientBrushProperties (D2D1::Point2F (0, 0), D2D1::Point2F (0, (float)_height)),
				stops, &_brush_bg);
			stops->Release ();
		}
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::on_tick ()
{
	DWORD now = GetTickCount ();
	if (_last_tick_ms == 0) {
		_last_tick_ms = now;
	}
	float dt = (now - _last_tick_ms) * 0.001f;
	_last_tick_ms = now;

	float t = (now - _start_tick) * 0.001f;
	_pulse = 0.5f + 0.5f * std::sin (t * 1.6f);

	/* Brief splash auto-dismiss (~1.6s); click/Esc/any key also clears. */
	if (_about.splash_visible () && _splash_shown_ms != 0
	    && (now - _splash_shown_ms) >= 1600u) {
		dismiss_splash ();
	}

	if (!_recovery.visible () && _timeout_secs == 0) {
		_engine.autosave_tick (now);
	}

	/* Poll engine often enough for meters while playing. */
	static DWORD last_poll = 0;
	static DWORD last_wave = 0;
	const bool rolling = _transport_playing || _engine_snap.transport_rolling;
	DWORD poll_ms = (rolling || _engine_snap.spectrum_enabled
	                 || _engine_snap.correlation_enabled
	                 || _engine_snap.loudness_enabled) ? 50u : 250u;
	if (now - last_poll > poll_ms) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_arm_from_engine ();
		sync_mixer_mute_solo_from_engine ();
		sync_mixer_pan_from_engine ();
		sync_mixer_send_from_engine ();
		sync_transport_loop_from_engine ();
		sync_transport_punch_from_engine ();
		sync_transport_tempo_from_engine ();
		sync_transport_freeze_from_engine ();
		sync_transport_clock_from_engine ();
		sync_history_from_engine ();
		apply_region_selection_to_lanes ();
		apply_loop_to_lanes ();
		refresh_automation_lane ();
		refresh_midi_cc_lane ();
		_transport.set_snap_enabled (_engine_snap.snap_enabled);
		_transport.set_snap_density (_engine_snap.snap_density);
		_transport.set_midi_step_enabled (_engine_snap.midi_step_enabled);
		_transport.set_midi_chord_quality (_engine_snap.midi_chord_quality);
		_transport.set_midi_arp_enabled (_engine_snap.midi_arp_enabled);
		_transport.set_midi_arp_mode (_engine_snap.midi_arp_mode);
		_transport.set_dsp_load_pct (_engine_snap.dsp_load_pct);
		sync_scale_to_transport ();
		_transport.set_spectrum_enabled (_engine_snap.spectrum_enabled);
		_transport.set_correlation_enabled (_engine_snap.correlation_enabled);
		_transport.set_loudness_enabled (_engine_snap.loudness_enabled);
		if (_engine_snap.snap_enabled) {
			_engine.refresh_grid_lines (
				_lanes[0].strip.view_start_sec (),
				_lanes[0].strip.view_span_sec ());
			_engine_snap = _engine.snapshot ();
		}
		last_poll = now;
	}

	tick_smoke_record (now);

	if (_engine_snap.running && rolling) {
		/* Prefer engine clock (Session transport_sample or processed_samples). */
		_playhead_sec = _engine_snap.transport_sec;
	} else if (_transport_playing) {
		_playhead_sec += dt; /* UI-only fallback */
	}

	WaveStripParams wp;
	wp.engine_running = _engine_snap.running;
	wp.transport_playing = _transport_playing;
	wp.sample_rate = _engine_snap.sample_rate;
	wp.period_frames = _engine_snap.period_frames;
	wp.time_sec = t;
	wp.playhead_sec = _playhead_sec;

	float wave_peaks[WaveStrip::kMaxColumns];
	float wave_rms[WaveStrip::kMaxColumns];

	/* Waveform peakfile refill is heavy — throttle while transport rolls. */
	const bool refresh_waves = (!rolling && (now - last_wave > 200u))
		|| (rolling && (now - last_wave > 250u))
		|| (last_wave == 0);
	if (refresh_waves) {
		last_wave = now;
	}

	const int n_lanes = n_audio_lanes ();
	for (int i = 0; i < n_lanes; ++i) {
		wp.gain = _mixers[i].gain ();
		_lanes[i].strip.update_view (wp);

		if (refresh_waves) {
			bool has_regions = false;
			size_t n = _lanes[i].strip.column_count ();
			if (n > WaveStrip::kMaxColumns) {
				n = WaveStrip::kMaxColumns;
			}
			if (_engine_snap.waves_from_session && n > 0) {
				has_regions = _engine.fill_track_waveform (
					i,
					_lanes[i].strip.view_start_sec (),
					_lanes[i].strip.view_span_sec (),
					wave_peaks,
					wave_rms,
					n);
				_lanes[i].strip.set_waveform (wave_peaks, wave_rms, n, has_regions);
			} else {
				_lanes[i].strip.set_waveform (nullptr, nullptr, 0, false);
			}
		}

		float peak = 0.f;
		bool  clip = false;
		if (_engine_snap.meters_real && i < EngineSnapshot::kMeters) {
			peak = _engine_snap.meter_peak[i];
			clip = _engine_snap.meter_clip[i];
		} else {
			peak = stub_meter_peak (i, dt);
		}
		_mixers[i].set_meter_peak (peak, clip);
		if (i < EngineSnapshot::kMeters) {
			_mixers[i].set_meter_point (_engine_snap.track_meter_point[i]);
			_mixers[i].set_meter_type (_engine_snap.track_meter_type[i]);
		}
		_mixers[i].meter_tick (dt);
	}
	{
		const int mi = midi_mixer_index ();
		if (mi >= 0 && mi < kMixerStripsMax
		    && (_engine_snap.midi_track_ok || _engine_snap.midi_track_count > 0)) {
			_mixers[mi].set_meter_peak (_engine_snap.midi_activity, false);
			_mixers[mi].meter_tick (dt);
		}
	}
	sync_mixer_fx_from_engine ();
	sync_mixer_mute_solo_from_engine ();
	sync_mixer_solo_isolate_from_engine ();
	sync_mixer_solo_safe_from_engine ();
	sync_mixer_comment_from_engine ();
	sync_mixer_phase_from_engine ();
	sync_mixer_trim_from_engine ();
	sync_mixer_listen_from_engine ();
	sync_mixer_monitor_from_engine ();
	sync_mixer_pan_from_engine ();
	sync_mixer_send_from_engine ();
	sync_monitor_from_engine ();

	if (_timeout_secs > 0 && (now - _start_tick) >= (_timeout_secs * 1000u)) {
		PostQuitMessage (0);
		return;
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::draw_editor_ruler ()
{
	const D2D1_RECT_F& rr = _editor_ruler;
	if (rr.right <= rr.left) {
		return;
	}
	_ui.fill_round (rr, 4.f, ui::ColorToken::Control, 0.34f);

	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	float w = rr.right - rr.left;
	const float hair = std::max (1.f, _dpi_scale);

	auto nx = [&] (float t) {
		return rr.left + std::max (0.f, std::min (1.f, (t - view0) / span)) * w;
	};
	auto in_view = [&] (float t) {
		return t >= view0 && t <= view0 + span;
	};

	/* Loop / punch range ribbons. */
	if (_engine_snap.loop_active) {
		float x0 = nx (_engine_snap.loop_start_sec);
		float x1 = nx (_engine_snap.loop_end_sec);
		if (x1 > x0) {
			_ui.fill_round (D2D1::RectF (x0, rr.top + 2.f * _dpi_scale,
			                             x1, rr.bottom - 1.f * _dpi_scale),
			                2.f, ui::ColorToken::Loop, kLoopRulerTintOp);
			_ui.fill_round (D2D1::RectF (x0, rr.top + 2.f * _dpi_scale,
			                             x1, rr.top + 2.f * _dpi_scale + hair),
			                0.f, ui::ColorToken::Loop, 0.85f);
		}
	}
	if (_engine_snap.punch_range_ok) {
		float x0 = nx (_engine_snap.punch_start_sec);
		float x1 = nx (_engine_snap.punch_end_sec);
		if (x1 > x0) {
			float op = _engine_snap.punch_enabled ? kPunchRulerTintOn : kPunchRulerTintOff;
			_ui.fill_round (D2D1::RectF (x0, rr.top + 2.f * _dpi_scale,
			                             x1, rr.bottom - 1.f * _dpi_scale),
			                2.f, ui::ColorToken::Punch, op);
			_ui.fill_round (D2D1::RectF (x0, rr.bottom - 1.f * _dpi_scale - hair,
			                             x1, rr.bottom - 1.f * _dpi_scale),
			                0.f, ui::ColorToken::Punch, 0.85f);
		}
	}

	/* Edge tick caps + In/Out captions so washes are scannable. */
	{
		D2D1_RECT_F tr;
		if (loop_punch_edge_tick_rect (true, true, &tr)) {
			_ui.fill_round (tr, 0.f, ui::ColorToken::Loop, 0.95f);
		}
		if (loop_punch_edge_tick_rect (true, false, &tr)) {
			_ui.fill_round (tr, 0.f, ui::ColorToken::Loop, 0.95f);
		}
		if (loop_punch_edge_tick_rect (false, true, &tr)) {
			_ui.fill_round (tr, 0.f, ui::ColorToken::Punch, 0.95f);
		}
		if (loop_punch_edge_tick_rect (false, false, &tr)) {
			_ui.fill_round (tr, 0.f, ui::ColorToken::Punch, 0.95f);
		}
		D2D1_RECT_F lr;
		if (loop_punch_edge_label_rect (true, true, &lr)) {
			_ui.text_vcenter (L"In", ui::TextRole::Caption, ui::ColorToken::Loop, lr);
		}
		if (loop_punch_edge_label_rect (true, false, &lr)) {
			_ui.text_vcenter (L"Out", ui::TextRole::Caption, ui::ColorToken::Loop, lr);
		}
		if (loop_punch_edge_label_rect (false, true, &lr)) {
			_ui.text_vcenter (L"In", ui::TextRole::Caption, ui::ColorToken::Punch, lr);
		}
		if (loop_punch_edge_label_rect (false, false, &lr)) {
			_ui.text_vcenter (L"Out", ui::TextRole::Caption, ui::ColorToken::Punch, lr);
		}
	}

	/* Adaptive time ticks: pick the first step that leaves room for labels. */
	static const float kSteps[] = { 0.25f, 0.5f, 1.f, 2.f, 5.f, 10.f, 15.f,
	                                30.f, 60.f, 120.f, 300.f, 600.f };
	const float ppsec = w / span;
	float step = 600.f;
	for (float s : kSteps) {
		if (s * ppsec >= 62.f * _dpi_scale) {
			step = s;
			break;
		}
	}
	const float t_end = view0 + span;
	float t = std::floor (view0 / step) * step;
	for (; t <= t_end + step * 0.5f; t += step) {
		if (!in_view (t)) {
			continue;
		}
		float px = nx (t);
		bool whole_min = std::fabs (t - std::floor (t / 60.f + 0.5f) * 60.f) < 0.001f;
		_ui.fill_round (D2D1::RectF (px, rr.bottom - (whole_min ? 6.f : 4.f) * _dpi_scale,
		                             px + hair, rr.bottom - 1.f * _dpi_scale),
		                0.f, ui::ColorToken::GridBar, whole_min ? 0.55f : 0.32f);
		int m = (int)(t / 60.f);
		float s = t - 60.f * (float)m;
		if (s < 0.f) {
			s = 0.f;
		}
		wchar_t lab[24];
		_snwprintf_s (lab, _TRUNCATE, L"%d:%04.1f", m, s);
		_ui.text_vcenter (lab, ui::TextRole::Caption, ui::ColorToken::TextMuted,
		                  D2D1::RectF (px + 3.f * _dpi_scale, rr.top,
		                               px + 50.f * _dpi_scale,
		                               rr.bottom - 3.f * _dpi_scale));
	}
	_ui.fill_round (D2D1::RectF (rr.left, rr.bottom - hair, rr.right, rr.bottom),
	                0.f, ui::ColorToken::Divider, 0.9f);

	/* Markers: stem + flag. */
	for (int i = 0; i < _engine_snap.marker_count; ++i) {
		float tm = _engine_snap.markers[i].sec;
		if (!in_view (tm)) {
			continue;
		}
		float px = nx (tm);
		_ui.fill_round (D2D1::RectF (px, rr.top + 2.f * _dpi_scale,
		                             px + std::max (1.5f, 1.5f * _dpi_scale),
		                             rr.bottom - hair),
		                1.f, ui::ColorToken::Marker, 0.9f);
		_ui.fill_round (D2D1::RectF (px + 1.5f * _dpi_scale, rr.top + 2.f * _dpi_scale,
		                             px + 8.f * _dpi_scale, rr.top + 6.f * _dpi_scale),
		                1.f, ui::ColorToken::Marker, 0.85f);
		const wchar_t* name = _engine_snap.markers[i].name;
		if (name[0]) {
			wchar_t label[32];
			format_marker_ruler_label (name, label, 32, 8);
			_ui.text_vcenter_n (label, (UINT32)wcslen (label),
			                    ui::TextRole::Caption, ui::ColorToken::Marker,
			                    D2D1::RectF (px + 10.f * _dpi_scale, rr.top,
			                                 px + 72.f * _dpi_scale, rr.bottom - 3.f * _dpi_scale));
		}
	}

	/* Snap-density ruler ticks (bar = full accent; subdiv = short body). */
	if (_engine_snap.snap_enabled && _engine_snap.grid_line_count > 0) {
		float rh = rr.bottom - rr.top;
		for (int i = 0; i < _engine_snap.grid_line_count; ++i) {
			float n = (_engine_snap.grid_line_sec[i] - view0) / span;
			if (n < 0.f || n > 1.f) {
				continue;
			}
			float px = rr.left + n * w;
			bool is_bar = _engine_snap.grid_line_bar[i];
			float thick = is_bar ? std::max (1.5f, 2.f * _dpi_scale)
			                     : std::max (1.f, 1.f * _dpi_scale);
			float tick_top = is_bar ? rr.top : (rr.top + rh * 0.45f);
			float op = is_bar ? 0.85f : 0.40f;
			if (!is_bar && _engine_snap.snap_density >= 2 && (i % 2) == 0) {
				op = 0.55f;
			}
			_ui.fill_round (D2D1::RectF (px - thick * 0.5f, tick_top,
			                             px + thick * 0.5f, rr.bottom),
			                0.f,
			                is_bar ? ui::ColorToken::Accent : ui::ColorToken::TextSecondary,
			                op);
		}
	}

	/* Playhead tick + head. */
	if (in_view (_playhead_sec)) {
		float px = nx (_playhead_sec);
		_ui.fill_round (D2D1::RectF (px, rr.top, px + std::max (1.5f, 1.5f * _dpi_scale),
		                             rr.bottom),
		                0.f, ui::ColorToken::Playhead, 0.95f);
		_ui.fill_round (D2D1::RectF (px - 3.f * _dpi_scale, rr.top,
		                             px + 3.f * _dpi_scale, rr.top + 5.f * _dpi_scale),
		                1.5f, ui::ColorToken::Playhead);
	}

	/* Left gutter caption. */
	const D2D1_RECT_F& pane = _editor_pane;
	float gutter_l = pane.left + 12.f * _dpi_scale;
	if (rr.left - gutter_l > 16.f * _dpi_scale) {
		_ui.text_vcenter (L"Time", ui::TextRole::Caption, ui::ColorToken::TextMuted,
		                  D2D1::RectF (gutter_l, rr.top, rr.left - 8.f * _dpi_scale,
		                               rr.bottom),
		                  DWRITE_TEXT_ALIGNMENT_TRAILING);
	}
}

bool
ShellWindow::loop_punch_edge_label_rect (bool is_loop, bool is_start, D2D1_RECT_F* out) const
{
	if (!out) {
		return false;
	}
	const D2D1_RECT_F& rr = _editor_ruler;
	if (rr.right <= rr.left) {
		return false;
	}
	float t0 = 0.f;
	float t1 = 0.f;
	if (is_loop) {
		if (!_engine_snap.loop_active
		    || _engine_snap.loop_end_sec <= _engine_snap.loop_start_sec) {
			return false;
		}
		t0 = _engine_snap.loop_start_sec;
		t1 = _engine_snap.loop_end_sec;
	} else {
		if (!_engine_snap.punch_range_ok
		    || _engine_snap.punch_end_sec <= _engine_snap.punch_start_sec) {
			return false;
		}
		t0 = _engine_snap.punch_start_sec;
		t1 = _engine_snap.punch_end_sec;
	}
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	float w = rr.right - rr.left;
	float sec = is_start ? t0 : t1;
	float n = (sec - view0) / span;
	if (n < 0.f || n > 1.f) {
		return false;
	}
	float px = rr.left + n * w;
	float tw = (is_start ? 18.f : 28.f) * _dpi_scale;
	float pad = 2.f * _dpi_scale;
	if (is_start) {
		*out = D2D1::RectF (px + pad, rr.top, px + pad + tw, rr.bottom);
	} else {
		*out = D2D1::RectF (px - pad - tw, rr.top, px - pad, rr.bottom);
	}
	return out->right > out->left;
}

bool
ShellWindow::loop_punch_edge_tick_rect (bool is_loop, bool is_start, D2D1_RECT_F* out) const
{
	if (!out) {
		return false;
	}
	const D2D1_RECT_F& rr = _editor_ruler;
	if (rr.right <= rr.left) {
		return false;
	}
	float t0 = 0.f;
	float t1 = 0.f;
	if (is_loop) {
		if (!_engine_snap.loop_active
		    || _engine_snap.loop_end_sec <= _engine_snap.loop_start_sec) {
			return false;
		}
		t0 = _engine_snap.loop_start_sec;
		t1 = _engine_snap.loop_end_sec;
	} else {
		if (!_engine_snap.punch_range_ok
		    || _engine_snap.punch_end_sec <= _engine_snap.punch_start_sec) {
			return false;
		}
		t0 = _engine_snap.punch_start_sec;
		t1 = _engine_snap.punch_end_sec;
	}
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	float w = rr.right - rr.left;
	float sec = is_start ? t0 : t1;
	float n = (sec - view0) / span;
	if (n < 0.f || n > 1.f) {
		return false;
	}
	float px = rr.left + n * w;
	float thick = std::max (1.5f, 2.f * _dpi_scale);
	*out = D2D1::RectF (px - thick * 0.5f, rr.top, px + thick * 0.5f, rr.bottom);
	return out->right > out->left;
}

bool
ShellWindow::smoke_loop_punch_edge_labels ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-punch-edge-labels FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	if (!_engine.set_loop_range (1.0f, 3.0f)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-punch-edge-labels FAILED (loop)\n");
		std::fflush (stderr);
		return false;
	}
	if (!_engine.set_punch_range (2.0f, 4.0f)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-punch-edge-labels FAILED (punch)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	fit_shared_range (0.5f, 4.5f);
	layout_panes ();

	D2D1_RECT_F a, b, c, d;
	if (!loop_punch_edge_label_rect (true, true, &a)
	    || !loop_punch_edge_label_rect (true, false, &b)
	    || !loop_punch_edge_label_rect (false, true, &c)
	    || !loop_punch_edge_label_rect (false, false, &d)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-punch-edge-labels FAILED (rects)\n");
		std::fflush (stderr);
		return false;
	}
	const D2D1_RECT_F& rr = _editor_ruler;
	auto inside = [&](const D2D1_RECT_F& r) {
		return r.left >= rr.left - 1.f && r.right <= rr.right + 1.f
		       && r.top >= rr.top - 1.f && r.bottom <= rr.bottom + 1.f
		       && r.right > r.left;
	};
	if (!inside (a) || !inside (b) || !inside (c) || !inside (d)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-punch-edge-labels FAILED (outside ruler)\n");
		std::fflush (stderr);
		return false;
	}
	if (!(a.left < b.left) || !(c.left < d.left)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-punch-edge-labels FAILED (order)\n");
		std::fflush (stderr);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-loop-punch-edge-labels ok "
	              "(loop In/Out · punch In/Out in ruler)\n");
	std::fflush (stderr);
	return true;
}

bool
ShellWindow::smoke_loop_punch_edge_ticks ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-punch-edge-ticks FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	if (!_engine.set_loop_range (1.0f, 3.0f)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-punch-edge-ticks FAILED (loop)\n");
		std::fflush (stderr);
		return false;
	}
	if (!_engine.set_punch_range (2.0f, 4.0f)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-punch-edge-ticks FAILED (punch)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	fit_shared_range (0.5f, 4.5f);
	layout_panes ();

	D2D1_RECT_F a, b, c, d;
	if (!loop_punch_edge_tick_rect (true, true, &a)
	    || !loop_punch_edge_tick_rect (true, false, &b)
	    || !loop_punch_edge_tick_rect (false, true, &c)
	    || !loop_punch_edge_tick_rect (false, false, &d)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-punch-edge-ticks FAILED (rects)\n");
		std::fflush (stderr);
		return false;
	}
	const D2D1_RECT_F& rr = _editor_ruler;
	auto inside = [&](const D2D1_RECT_F& r) {
		return r.left >= rr.left - 1.f && r.right <= rr.right + 1.f
		       && r.top >= rr.top - 1.f && r.bottom <= rr.bottom + 1.f
		       && r.right > r.left
		       && (r.bottom - r.top) >= (rr.bottom - rr.top) * 0.9f
		       && (r.right - r.left) <= 6.f * _dpi_scale;
	};
	if (!inside (a) || !inside (b) || !inside (c) || !inside (d)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-punch-edge-ticks FAILED (shape)\n");
		std::fflush (stderr);
		return false;
	}
	if (!(a.left < b.left) || !(c.left < d.left)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-punch-edge-ticks FAILED (order)\n");
		std::fflush (stderr);
		return false;
	}
	/* Ticks should sit at the wash edges (left of start labels / right of end). */
	D2D1_RECT_F la, lb;
	if (!loop_punch_edge_label_rect (true, true, &la)
	    || !loop_punch_edge_label_rect (true, false, &lb)
	    || !(a.right <= la.left + 1.f)
	    || !(b.left >= lb.right - 1.f)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-punch-edge-ticks FAILED (vs labels)\n");
		std::fflush (stderr);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-loop-punch-edge-ticks ok "
	              "(loop + punch edge caps)\n");
	std::fflush (stderr);
	return true;
}

bool
ShellWindow::smoke_punch_loop_tint ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-punch-loop-tint FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	/* Hue separation: loop amber G high, punch crimson G low. */
	if (!(kLoop.g > 0.65f && kPunch.g < 0.40f && kLoop.r > 0.85f && kPunch.r > 0.85f
	      && (kLoop.g - kPunch.g) > 0.35f)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-punch-loop-tint FAILED "
		              "(colors loop g=%.2f punch g=%.2f)\n",
		              kLoop.g, kPunch.g);
		std::fflush (stderr);
		return false;
	}
	if (!(kLoopRulerTintOp > 0.45f && kPunchRulerTintOn > kPunchRulerTintOff
	      && kPunchRulerTintOn > 0.65f && kLoopLaneTintOp > 0.12f
	      && kPunchLaneTintOn > kPunchLaneTintOff && kPunchLaneTintOn > 0.28f)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-punch-loop-tint FAILED (opacity floor)\n");
		std::fflush (stderr);
		return false;
	}
	if (!_engine.set_loop_range (1.0f, 3.0f) || !_engine.set_punch_range (2.0f, 4.0f)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-punch-loop-tint FAILED (ranges)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (!_engine_snap.loop_active || !_engine_snap.punch_range_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-punch-loop-tint FAILED (snap flags)\n");
		std::fflush (stderr);
		return false;
	}
	fit_shared_range (0.5f, 4.5f);
	layout_panes ();
	/* Lane wash geometry must cover Audio 1. */
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	float n0 = (_engine_snap.loop_start_sec - view0) / span;
	float n1 = (_engine_snap.loop_end_sec - view0) / span;
	n0 = std::max (0.f, std::min (1.f, n0));
	n1 = std::max (0.f, std::min (1.f, n1));
	const D2D1_RECT_F& lb = _lanes[0].bounds;
	float w = lb.right - lb.left;
	if (n1 <= n0 || w < 8.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-punch-loop-tint FAILED (lane geometry)\n");
		std::fflush (stderr);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-punch-loop-tint ok "
	              "(amber≠crimson · ruler %.2f/%.2f · lane loop+punch)\n",
	              kLoopRulerTintOp, kPunchRulerTintOn);
	std::fflush (stderr);
	return true;
}

bool
ShellWindow::smoke_loop_inactive_hatch ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-inactive-hatch FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	if (!(kLoopLaneTintIdle < kLoopLaneTintOp
	      && kLoopLaneTintIdle < 0.14f
	      && kLoopInactiveHatchOp > 0.22f
	      && kLoopInactiveHatchPx >= 4.f
	      && kLoopInactiveHatchPx <= 10.f)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-inactive-hatch FAILED "
		              "(idle=%.2f play=%.2f hatch=%.2f/%.1fpx)\n",
		              kLoopLaneTintIdle, kLoopLaneTintOp,
		              kLoopInactiveHatchOp, kLoopInactiveHatchPx);
		std::fflush (stderr);
		return false;
	}
	if (!_engine.set_loop_range (1.0f, 3.0f)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-inactive-hatch FAILED (range)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (!_engine_snap.loop_active) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-inactive-hatch FAILED (no loop)\n");
		std::fflush (stderr);
		return false;
	}
	/* Ensure play-loop is off so idle hatch path is the live state. */
	if (_engine_snap.loop_playing) {
		(void)_engine.set_loop_play (false);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	if (_engine_snap.loop_playing) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-loop-inactive-hatch FAILED (still playing)\n");
		std::fflush (stderr);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-loop-inactive-hatch ok "
	              "(idle=%.2f hatch=%.2f @%.0fpx)\n",
	              kLoopLaneTintIdle, kLoopInactiveHatchOp, kLoopInactiveHatchPx);
	std::fflush (stderr);
	return true;
}

bool
ShellWindow::smoke_punch_inactive_hatch ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-punch-inactive-hatch FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	if (!(kPunchLaneTintOff < kPunchLaneTintOn
	      && kPunchLaneTintOff < 0.15f
	      && kPunchInactiveHatchOp > 0.24f
	      && kPunchInactiveHatchPx >= 4.f
	      && kPunchInactiveHatchPx <= 10.f)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-punch-inactive-hatch FAILED "
		              "(off=%.2f on=%.2f hatch=%.2f/%.1fpx)\n",
		              kPunchLaneTintOff, kPunchLaneTintOn,
		              kPunchInactiveHatchOp, kPunchInactiveHatchPx);
		std::fflush (stderr);
		return false;
	}
	if (!_engine.set_punch_range (1.0f, 3.0f)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-punch-inactive-hatch FAILED (range)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (!_engine_snap.punch_range_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-punch-inactive-hatch FAILED (no punch)\n");
		std::fflush (stderr);
		return false;
	}
	if (_engine_snap.punch_enabled) {
		(void)_engine.set_punch_enabled (false);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
	}
	if (_engine_snap.punch_enabled) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-punch-inactive-hatch FAILED (still armed)\n");
		std::fflush (stderr);
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-punch-inactive-hatch ok "
	              "(off=%.2f hatch=%.2f @%.0fpx)\n",
	              kPunchLaneTintOff, kPunchInactiveHatchOp, kPunchInactiveHatchPx);
	std::fflush (stderr);
	return true;
}

float
ShellWindow::editor_content_bottom () const
{
	float bot = _lanes[kTracks - 1].bounds.bottom;
	if (_auto_lane_bounds.bottom > bot) {
		bot = _auto_lane_bounds.bottom;
	}
	if (_midi_lane.bottom > bot) {
		bot = _midi_lane.bottom;
	}
	if (_midi_cc_lane_bounds.bottom > bot) {
		bot = _midi_cc_lane_bounds.bottom;
	}
	return bot;
}

bool
ShellWindow::smoke_midi_punch_grid ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-punch-grid FAILED (no session)\n");
		return false;
	}
	layout_panes ();
	if (_midi_lane.bottom <= _midi_lane.top + 8.f
	    || _midi_cc_lane_bounds.bottom <= _midi_cc_lane_bounds.top + 4.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-punch-grid FAILED "
		              "(midi=[%.1f..%.1f] cc=[%.1f..%.1f])\n",
		              _midi_lane.top, _midi_lane.bottom,
		              _midi_cc_lane_bounds.top, _midi_cc_lane_bounds.bottom);
		return false;
	}
	float bot = editor_content_bottom ();
	if (bot < _midi_cc_lane_bounds.bottom - 0.5f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-punch-grid FAILED "
		              "(content_bot=%.1f cc_bot=%.1f)\n",
		              bot, _midi_cc_lane_bounds.bottom);
		return false;
	}
	if (!_engine.set_punch_range (0.5f, 2.5f) || !_engine.set_punch_enabled (true)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-punch-grid FAILED (punch)\n");
		return false;
	}
	if (!_engine.set_loop_start (1.0f) || !_engine.set_loop_end (3.0f)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-punch-grid FAILED (loop)\n");
		return false;
	}
	_engine.set_snap_enabled (true);
	_engine.refresh_grid_lines (_lanes[0].strip.view_start_sec (),
	                            _lanes[0].strip.view_span_sec ());
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (!_engine_snap.punch_range_ok || !_engine_snap.punch_enabled
	    || !_engine_snap.loop_active || !_engine_snap.snap_enabled) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-punch-grid FAILED "
		              "(punch=%d en=%d loop=%d snap=%d)\n",
		              _engine_snap.punch_range_ok ? 1 : 0,
		              _engine_snap.punch_enabled ? 1 : 0,
		              _engine_snap.loop_active ? 1 : 0,
		              _engine_snap.snap_enabled ? 1 : 0);
		return false;
	}
	if (_engine_snap.grid_line_count <= 0) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-punch-grid FAILED (no grid)\n");
		return false;
	}
	/* Overlay draw path is paint-time; geometry coverage is what we can assert. */
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-midi-punch-grid ok "
	              "(content_bot=%.1f midi+cc covered, punch+loop+grid live)\n",
	              bot);
	return true;
}

void
ShellWindow::draw_loop_overlay ()
{
	if (!_engine_snap.loop_active || !_brush_loop) {
		return;
	}
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	float n0 = (_engine_snap.loop_start_sec - view0) / span;
	float n1 = (_engine_snap.loop_end_sec - view0) / span;
	n0 = std::max (0.f, std::min (1.f, n0));
	n1 = std::max (0.f, std::min (1.f, n1));
	if (n1 <= n0) {
		return;
	}
	const bool playing = _engine_snap.loop_playing;
	_brush_loop->SetOpacity (playing ? kLoopLaneTintOp : kLoopLaneTintIdle);
	for (int i = 0; i < kTracks; ++i) {
		const D2D1_RECT_F& lb = _lanes[i].bounds;
		float w = lb.right - lb.left;
		if (w < 4.f) {
			continue;
		}
		float bot = (i == 0 && _auto_lane_bounds.bottom > lb.bottom)
		            ? _auto_lane_bounds.bottom : lb.bottom;
		float x0 = lb.left + n0 * w;
		float x1 = lb.left + n1 * w;
		_rt->FillRectangle (D2D1::RectF (x0, lb.top, x1, bot), _brush_loop);
		if (!playing) {
			/* Soft vertical hatch — range present but play-loop idle. */
			_brush_loop->SetOpacity (kLoopInactiveHatchOp);
			float step = std::max (3.f, kLoopInactiveHatchPx * _dpi_scale);
			float stripe = std::max (1.f, 1.2f * _dpi_scale);
			for (float x = x0; x < x1 - stripe; x += step) {
				_rt->FillRectangle (D2D1::RectF (x, lb.top, x + stripe, bot), _brush_loop);
			}
			_brush_loop->SetOpacity (kLoopLaneTintIdle);
		}
	}
	/* Soft loop wash on MIDI + CC (audio strips already covered above). */
	float left = _lanes[0].bounds.left;
	float right = _lanes[0].bounds.right;
	float w = right - left;
	if (w >= 4.f) {
		float x0 = left + n0 * w;
		float x1 = left + n1 * w;
		if (_midi_lane.bottom > _midi_lane.top + 2.f) {
			_rt->FillRectangle (
				D2D1::RectF (x0, _midi_lane.top, x1, _midi_lane.bottom),
				_brush_loop);
			if (!playing) {
				_brush_loop->SetOpacity (kLoopInactiveHatchOp);
				float step = std::max (3.f, kLoopInactiveHatchPx * _dpi_scale);
				float stripe = std::max (1.f, 1.2f * _dpi_scale);
				for (float x = x0; x < x1 - stripe; x += step) {
					_rt->FillRectangle (
						D2D1::RectF (x, _midi_lane.top, x + stripe, _midi_lane.bottom),
						_brush_loop);
				}
				_brush_loop->SetOpacity (playing ? kLoopLaneTintOp : kLoopLaneTintIdle);
			}
		}
		if (_midi_cc_lane_bounds.bottom > _midi_cc_lane_bounds.top + 2.f) {
			_rt->FillRectangle (
				D2D1::RectF (x0, _midi_cc_lane_bounds.top, x1, _midi_cc_lane_bounds.bottom),
				_brush_loop);
			if (!playing) {
				_brush_loop->SetOpacity (kLoopInactiveHatchOp);
				float step = std::max (3.f, kLoopInactiveHatchPx * _dpi_scale);
				float stripe = std::max (1.f, 1.2f * _dpi_scale);
				for (float x = x0; x < x1 - stripe; x += step) {
					_rt->FillRectangle (
						D2D1::RectF (x, _midi_cc_lane_bounds.top, x + stripe,
						             _midi_cc_lane_bounds.bottom),
						_brush_loop);
				}
				_brush_loop->SetOpacity (playing ? kLoopLaneTintOp : kLoopLaneTintIdle);
			}
		}
	}
	_brush_loop->SetOpacity (1.f);
}

void
ShellWindow::draw_punch_overlay ()
{
	if (!_engine_snap.punch_range_ok || !_brush_punch) {
		return;
	}
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	float n0 = (_engine_snap.punch_start_sec - view0) / span;
	float n1 = (_engine_snap.punch_end_sec - view0) / span;
	n0 = std::max (0.f, std::min (1.f, n0));
	n1 = std::max (0.f, std::min (1.f, n1));
	if (n1 <= n0) {
		return;
	}
	const bool armed = _engine_snap.punch_enabled;
	float op = armed ? kPunchLaneTintOn : kPunchLaneTintOff;
	_brush_punch->SetOpacity (op);
	if (_rt) {
		_rt->PushAxisAlignedClip (
		    D2D1::RectF (_editor_pane.left, _editor_lanes_top,
		                 _editor_pane.right, _editor_lanes_bot),
		    D2D1_ANTIALIAS_MODE_ALIASED);
	}
	for (int i = 0; i < kTracks; ++i) {
		const D2D1_RECT_F& lb = _lanes[i].bounds;
		float w = lb.right - lb.left;
		if (w < 4.f) {
			continue;
		}
		float bot = (i == _auto_lane_track && _auto_lane_bounds.bottom > lb.bottom)
		            ? _auto_lane_bounds.bottom : lb.bottom;
		float x0 = lb.left + n0 * w;
		float x1 = lb.left + n1 * w;
		_rt->FillRectangle (D2D1::RectF (x0, lb.top, x1, bot), _brush_punch);
		if (!armed) {
			/* Soft vertical hatch — range present but punch disarmed. */
			_brush_punch->SetOpacity (kPunchInactiveHatchOp);
			float step = std::max (3.f, kPunchInactiveHatchPx * _dpi_scale);
			float stripe = std::max (1.f, 1.2f * _dpi_scale);
			for (float x = x0; x < x1 - stripe; x += step) {
				_rt->FillRectangle (D2D1::RectF (x, lb.top, x + stripe, bot), _brush_punch);
			}
			_brush_punch->SetOpacity (kPunchLaneTintOff);
		}
	}
	/* Extend punch wash through piano-roll + CC lane. */
	float left = _lanes[0].bounds.left;
	float right = _lanes[0].bounds.right;
	float w = right - left;
	if (w >= 4.f) {
		float x0 = left + n0 * w;
		float x1 = left + n1 * w;
		if (_midi_lane.bottom > _midi_lane.top + 2.f) {
			_rt->FillRectangle (
				D2D1::RectF (x0, _midi_lane.top, x1, _midi_lane.bottom),
				_brush_punch);
			if (!armed) {
				_brush_punch->SetOpacity (kPunchInactiveHatchOp);
				float step = std::max (3.f, kPunchInactiveHatchPx * _dpi_scale);
				float stripe = std::max (1.f, 1.2f * _dpi_scale);
				for (float x = x0; x < x1 - stripe; x += step) {
					_rt->FillRectangle (
						D2D1::RectF (x, _midi_lane.top, x + stripe, _midi_lane.bottom),
						_brush_punch);
				}
				_brush_punch->SetOpacity (kPunchLaneTintOff);
			}
		}
		if (_midi_cc_lane_bounds.bottom > _midi_cc_lane_bounds.top + 2.f) {
			_rt->FillRectangle (
				D2D1::RectF (x0, _midi_cc_lane_bounds.top, x1, _midi_cc_lane_bounds.bottom),
				_brush_punch);
			if (!armed) {
				_brush_punch->SetOpacity (kPunchInactiveHatchOp);
				float step = std::max (3.f, kPunchInactiveHatchPx * _dpi_scale);
				float stripe = std::max (1.f, 1.2f * _dpi_scale);
				for (float x = x0; x < x1 - stripe; x += step) {
					_rt->FillRectangle (
						D2D1::RectF (x, _midi_cc_lane_bounds.top, x + stripe,
						             _midi_cc_lane_bounds.bottom),
						_brush_punch);
				}
				_brush_punch->SetOpacity (kPunchLaneTintOff);
			}
		}
	}
	if (_rt) {
		_rt->PopAxisAlignedClip ();
	}
	_brush_punch->SetOpacity (1.f);
}

void
ShellWindow::draw_editor_grid ()
{
	if (!_engine_snap.snap_enabled || _engine_snap.grid_line_count <= 0) {
		return;
	}
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	/* Vertical span: ruler through Audio + MIDI + CC. */
	float top = _editor_ruler.top;
	float bot = editor_content_bottom ();
	float left = _lanes[0].bounds.left;
	float right = _lanes[0].bounds.right;
	float w = right - left;
	if (w < 8.f || bot <= top) {
		return;
	}

	for (int i = 0; i < _engine_snap.grid_line_count; ++i) {
		float n = (_engine_snap.grid_line_sec[i] - view0) / span;
		if (n < 0.f || n > 1.f) {
			continue;
		}
		float px = left + n * w;
		bool is_bar = _engine_snap.grid_line_bar[i];
		float thick = is_bar ? std::max (1.f, 1.25f * _dpi_scale)
		                     : std::max (1.f, 1.f * _dpi_scale);
		/* Density-aware: bars strongest; even subdivs slightly stronger at 1/2·1/4. */
		float op = is_bar ? 0.22f : 0.11f;
		if (!is_bar && _engine_snap.snap_density >= 2 && (i % 2) == 0) {
			op = 0.18f;
		}
		_ui.fill_round (D2D1::RectF (px, top, px + thick, bot), 0.f,
		                is_bar ? ui::ColorToken::GridBar : ui::ColorToken::GridBeat,
		                op);
	}
}

void
ShellWindow::draw_editor_pane ()
{
	const D2D1_RECT_F& r = _editor_pane;
	_ui.panel (r, 10.f, false);

	const float inset = 12.f * _dpi_scale;
	const float header_h = 32.f * _dpi_scale;
	_ui.fill_round (D2D1::RectF (r.left + 16.f * _dpi_scale, r.top + 11.f * _dpi_scale,
	                             r.left + 19.f * _dpi_scale, r.top + 27.f * _dpi_scale),
	                1.5f, ui::ColorToken::Accent);
	_ui.text_vcenter (L"Editor", ui::TextRole::Section,
	                  ui::ColorToken::TextPrimary,
	                  D2D1::RectF (r.left + 26.f * _dpi_scale, r.top,
	                               r.left + 100.f * _dpi_scale, r.top + header_h));

	wchar_t caption[180];
	int with_audio = 0;
	const int n_lanes_ui = n_audio_lanes ();
	for (int i = 0; i < n_lanes_ui; ++i) {
		if (_lanes[i].strip.has_regions ()) {
			++with_audio;
		}
	}
	if (_engine_snap.waves_from_session) {
		if (_engine_snap.loop_active) {
			_snwprintf_s (caption, _TRUNCATE,
			              L"loop [%.1f–%.1f]%s  ·  marks %d  ·  Shift/Ctrl/Alt+click",
			              _engine_snap.loop_start_sec, _engine_snap.loop_end_sec,
			              _engine_snap.loop_playing ? L" ▶" : L"",
			              _engine_snap.marker_count);
		} else if (_engine_snap.has_selection) {
			_snwprintf_s (caption, _TRUNCATE,
			              L"sel T%d [%.1f–%.1f]  ·  trim/move  S/F  G=snap%s",
			              _engine_snap.selection_track + 1,
			              _engine_snap.selection_start_sec, _engine_snap.selection_end_sec,
			              snap_caption_suffix ());
		} else if (with_audio > 0) {
			_snwprintf_s (caption, _TRUNCATE,
			              L"%d tracks  ·  %d with audio  ·  G=snap%s  ·  marks %d",
			              n_lanes_ui, with_audio,
			              snap_caption_suffix (),
			              _engine_snap.marker_count);
		} else {
			_snwprintf_s (caption, _TRUNCATE,
			              L"%d tracks  ·  empty  ·  [ ]=loop  M=mark  L=loop play",
			              n_lanes_ui);
		}
	} else if (_lanes[0].strip.interacting ()) {
		_snwprintf_s (caption, _TRUNCATE,
		              L"%d tracks  ·  zoom %.2fx  ·  Home follows transport",
		              n_lanes_ui, _lanes[0].strip.zoom ());
	} else {
		_snwprintf_s (caption, _TRUNCATE,
		              L"%d tracks  ·  empty lanes (no Session)",
		              n_lanes_ui);
	}
	_ui.text_vcenter (caption, ui::TextRole::Caption,
	                  ui::ColorToken::TextMuted,
	                  D2D1::RectF (r.left + 102.f * _dpi_scale, r.top,
	                               r.right - inset, r.top + header_h));

	_ui.divider (D2D1::RectF (r.left + 12.f * _dpi_scale, r.top + header_h,
	                          r.right - 12.f * _dpi_scale,
	                          r.top + header_h + std::max (1.f, _dpi_scale)));

	draw_editor_ruler ();

	/* Clip audio lanes to the scroll viewport: lanes may extend past it. */
	if (_rt) {
		_rt->PushAxisAlignedClip (
		    D2D1::RectF (r.left, _editor_lanes_top, r.right, _editor_lanes_bot),
		    D2D1_ANTIALIAS_MODE_ALIASED);
	}

	float label_w = 64.f * _dpi_scale;
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	const int n_lanes_draw = n_audio_lanes ();
	for (int i = 0; i < n_lanes_draw; ++i) {
		const D2D1_RECT_F& lb = _lanes[i].bounds;
		float label_bot = lb.bottom;
		if (i == _auto_lane_track && _auto_lane_bounds.bottom > label_bot) {
			label_bot = _auto_lane_bounds.bottom;
		}
		D2D1_RECT_F label_rc = D2D1::RectF (
			r.left + inset, lb.top, lb.left - 4.f * _dpi_scale, label_bot);
		(void)label_w;
		const bool focused = (i == _focus_audio_track);
		_ui.text_vcenter (_lanes[i].label, ui::TextRole::Section,
		                  focused ? ui::ColorToken::TextPrimary
		                          : ui::ColorToken::TextSecondary,
		                  label_rc);

		_ui.fill_round (lb, 4.f, ui::ColorToken::Control, 0.24f);
		_ui.stroke_round (lb, 4.f, focused ? ui::ColorToken::BorderStrong
		                                   : ui::ColorToken::Border,
		                  1.f, 0.9f);
		if (focused) {
			_ui.fill_round (D2D1::RectF (lb.left, lb.top + 4.f * _dpi_scale,
			                             lb.left + 2.f * _dpi_scale,
			                             lb.bottom - 4.f * _dpi_scale),
			                1.f, ui::ColorToken::Accent, 0.8f);
		}

		_lanes[i].strip.draw (_rt, lb, _brush_peak, _brush_rms, _brush_body,
		                      _brush_playhead, _brush_selection, _dpi_scale);

		/* Marker hairlines across lanes. */
		float lane_w = lb.right - lb.left;
		float mark_bot = (i == _auto_lane_track) ? _auto_lane_bounds.bottom : lb.bottom;
		for (int m = 0; m < _engine_snap.marker_count; ++m) {
			float n = (_engine_snap.markers[m].sec - view0) / span;
			if (n < 0.f || n > 1.f) {
				continue;
			}
			float px = lb.left + n * lane_w;
			_brush_marker->SetOpacity (0.55f);
			_rt->DrawLine (D2D1::Point2F (px, lb.top), D2D1::Point2F (px, mark_bot),
			               _brush_marker, 1.f * _dpi_scale);
			_brush_marker->SetOpacity (1.f);
		}
	}

	draw_region_envelope ();
	draw_automation_lane ();
	if (_rt) {
		_rt->PopAxisAlignedClip ();
	}
	draw_editor_scrollbar ();
	draw_midi_lane ();
	draw_midi_cc_lane ();
	draw_midi_marker_overlay ();
	draw_loop_overlay ();
	draw_punch_overlay ();
	draw_editor_grid ();
}

void
ShellWindow::draw_midi_marker_overlay ()
{
	if (!_brush_marker || _engine_snap.marker_count <= 0) {
		return;
	}
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	float left = _lanes[0].bounds.left;
	float right = _lanes[0].bounds.right;
	float w = right - left;
	if (w < 4.f) {
		return;
	}
	_brush_marker->SetOpacity (0.50f);
	for (int m = 0; m < _engine_snap.marker_count; ++m) {
		float n = (_engine_snap.markers[m].sec - view0) / span;
		if (n < 0.f || n > 1.f) {
			continue;
		}
		float px = left + n * w;
		if (_midi_lane.bottom > _midi_lane.top + 2.f) {
			_rt->DrawLine (D2D1::Point2F (px, _midi_lane.top),
			               D2D1::Point2F (px, _midi_lane.bottom),
			               _brush_marker, 1.f * _dpi_scale);
		}
		if (_midi_cc_lane_bounds.bottom > _midi_cc_lane_bounds.top + 2.f) {
			_rt->DrawLine (D2D1::Point2F (px, _midi_cc_lane_bounds.top),
			               D2D1::Point2F (px, _midi_cc_lane_bounds.bottom),
			               _brush_marker, 1.f * _dpi_scale);
		}
	}
	_brush_marker->SetOpacity (1.f);
}

bool
ShellWindow::smoke_midi_marker_overlay ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-marker-overlay FAILED (no session)\n");
		return false;
	}
	layout_panes ();
	if (_midi_lane.bottom <= _midi_lane.top + 8.f
	    || _midi_cc_lane_bounds.bottom <= _midi_cc_lane_bounds.top + 4.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-midi-marker-overlay FAILED (layout)\n");
		return false;
	}
	if (!_engine.add_marker_at (1.25f, "M-MIDI")) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-marker-overlay FAILED (add)\n");
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (_engine_snap.marker_count < 1) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-marker-overlay FAILED (count)\n");
		return false;
	}
	float view0 = _lanes[0].strip.view_start_sec ();
	float span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
	float left = _lanes[0].bounds.left;
	float w = _lanes[0].bounds.right - left;
	bool found = false;
	float px = 0.f;
	for (int m = 0; m < _engine_snap.marker_count; ++m) {
		if (std::fabs (_engine_snap.markers[m].sec - 1.25f) > 0.08f) {
			continue;
		}
		float n = (_engine_snap.markers[m].sec - view0) / span;
		px = left + n * w;
		found = (n >= 0.f && n <= 1.f && px >= left - 1.f && px <= left + w + 1.f);
		break;
	}
	if (!found) {
		/* Fit so marker is on-screen then recheck. */
		_lanes[0].strip.fit_range (0.f, 4.f, 0.05f);
		for (int i = 1; i < kTracks; ++i) {
			_lanes[i].strip.sync_view_from (_lanes[0].strip);
		}
		layout_panes ();
		view0 = _lanes[0].strip.view_start_sec ();
		span = std::max (0.001f, _lanes[0].strip.view_span_sec ());
		left = _lanes[0].bounds.left;
		w = _lanes[0].bounds.right - left;
		for (int m = 0; m < _engine_snap.marker_count; ++m) {
			if (std::fabs (_engine_snap.markers[m].sec - 1.25f) > 0.08f) {
				continue;
			}
			float n = (_engine_snap.markers[m].sec - view0) / span;
			px = left + n * w;
			found = (n >= 0.f && n <= 1.f);
			break;
		}
	}
	if (!found) {
		std::fprintf (stderr, "airdour_win_shell: smoke-midi-marker-overlay FAILED (x map)\n");
		return false;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-midi-marker-overlay ok "
	              "(marker@1.25s px=%.1f across midi+cc)\n",
	              px);
	return true;
}

void
ShellWindow::draw_midi_lane ()
{
	const D2D1_RECT_F& r = _midi_lane;
	if (r.right <= r.left) {
		return;
	}
	const D2D1_RECT_F piano = midi_piano_rect ();
	const D2D1_RECT_F vel = midi_velocity_rect ();

	float inset = 12.f * _dpi_scale;
	D2D1_RECT_F label_rc = D2D1::RectF (
		_editor_pane.left + inset, r.top, r.left - 4.f * _dpi_scale, r.bottom);
	const wchar_t* midi_lab = (_engine_snap.midi_track_name[0] != L'\0')
		? _engine_snap.midi_track_name : L"MIDI 1";
	_ui.text_vcenter (midi_lab, ui::TextRole::Section,
	                  _engine_snap.midi_track_ok ? ui::ColorToken::TextPrimary
	                                             : ui::ColorToken::TextMuted,
	                  label_rc);

	_ui.fill_round (r, 4.f, ui::ColorToken::Control, 0.16f);
	_ui.stroke_round (r, 4.f, ui::ColorToken::Border, 1.f, 0.8f);

	const int keys = kMidiPitchRows;
	float h = piano.bottom - piano.top;
	float row_h = h / (float)keys;
	const bool step_on = _engine_snap.midi_step_enabled;
	for (int k = 0; k < keys; ++k) {
		float y0 = piano.top + (float)k * row_h;
		uint8_t pitch = (uint8_t)(kMidiPitchHi - k);
		int pc = pitch % 12;
		bool black = (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);
		if (black) {
			_ui.fill_round (D2D1::RectF (piano.left, y0, piano.right, y0 + row_h),
			                0.f, ui::ColorToken::Surface, 0.55f);
		} else if (pc == 0) {
			_ui.fill_round (D2D1::RectF (piano.left, y0, piano.right,
			                             y0 + std::max (1.f, _dpi_scale)),
			                0.f, ui::ColorToken::GridBar, 0.14f);
		}
		/* Step-record held computer-key → accent wash on that pitch row. */
		if (step_on && pitch < 128 && _step_piano_held[pitch]) {
			_ui.fill_round (D2D1::RectF (piano.left, y0, piano.right, y0 + row_h - 0.5f),
			                0.f, ui::ColorToken::Accent, 0.32f);
		}
	}

	/* Audition ghost — full-width wash on the sounding pitch row. */
	if (_engine_snap.midi_audition_pitch >= kMidiPitchLo
	    && _engine_snap.midi_audition_pitch <= kMidiPitchHi) {
		int arow = (int)(kMidiPitchHi - _engine_snap.midi_audition_pitch);
		float ay0 = piano.top + (float)arow * row_h;
		_ui.fill_round (D2D1::RectF (piano.left, ay0, piano.right, ay0 + row_h - 0.5f),
		                0.f, ui::ColorToken::Accent, 0.28f);
	}

	/* Session MIDI notes — height scales with velocity (bottom-aligned in row). */
	const bool trimming = (_drag == DragMode::MidiTrimStart || _drag == DragMode::MidiTrimEnd);
	const bool have_loop = _engine_snap.loop_active
	                       && _engine_snap.loop_end_sec > _engine_snap.loop_start_sec;
	const bool have_punch = _engine_snap.punch_range_ok
	                        && _engine_snap.punch_end_sec > _engine_snap.punch_start_sec;
	bool outside_ranges_sel = false;
	if (have_loop && have_punch) {
		const float L0 = _engine_snap.loop_start_sec;
		const float L1 = _engine_snap.loop_end_sec;
		const float P0 = _engine_snap.punch_start_sec;
		const float P1 = _engine_snap.punch_end_sec;
		for (int i = 0; i < _engine_snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _engine_snap.midi_notes[i];
			if (!n.selected) {
				continue;
			}
			float n1 = n.start_sec + n.length_sec;
			const bool in_loop = (n.start_sec < L1 && n1 > L0);
			const bool in_punch = (n.start_sec < P1 && n1 > P0);
			if (!in_loop && !in_punch) {
				outside_ranges_sel = true;
				break;
			}
		}
	}
	for (int i = 0; i < _engine_snap.midi_note_count; ++i) {
		const EngineMidiNoteSnap& n = _engine_snap.midi_notes[i];
		if (n.pitch < kMidiPitchLo || n.pitch > kMidiPitchHi) {
			continue;
		}
		float t0 = n.start_sec;
		float t1 = n.start_sec + n.length_sec;
		if (trimming && n.selected) {
			t0 = _midi_trim_start_sec;
			t1 = _midi_trim_end_sec;
		}
		float x0 = _lanes[0].strip.x_at_time (r, t0);
		float x1 = _lanes[0].strip.x_at_time (r, t1);
		if (x1 < r.left || x0 > r.right) {
			continue;
		}
		if (x1 - x0 < 2.f) {
			x1 = x0 + 2.f;
		}
		int row = (int)(kMidiPitchHi - n.pitch);
		float row_top = piano.top + (float)row * row_h + 1.f;
		float row_bot = row_top + row_h - 2.f;
		float vmin = 3.f * _dpi_scale;
		float full = std::max (vmin, row_bot - row_top);
		float vh = vmin + (full - vmin) * ((float)n.velocity / 127.f);
		float y1 = row_bot;
		float y0 = y1 - vh;
		float op = n.selected ? kMidiNoteSelOp : kMidiNoteIdleOp;
		if (outside_ranges_sel && !n.selected && have_loop && have_punch) {
			float n1 = n.start_sec + n.length_sec;
			const bool in_loop = (n.start_sec < _engine_snap.loop_end_sec
			                      && n1 > _engine_snap.loop_start_sec);
			const bool in_punch = (n.start_sec < _engine_snap.punch_end_sec
			                       && n1 > _engine_snap.punch_start_sec);
			if (in_loop || in_punch) {
				op = kMidiOutsideRangesInWashOp;
			}
		}
		const ui::ColorToken tone = n.selected ? ui::ColorToken::Accent
		                                       : ui::ColorToken::Midi;
		_ui.fill_round (D2D1::RectF (x0, y0, x1, y1), 2.f, tone, op);
		if (n.selected) {
			_ui.stroke_round (D2D1::RectF (x0, y0, x1, y1), 2.f,
			                  ui::ColorToken::Playhead, 1.f, 0.9f);
		}
	}

	/* Velocity lane under piano-roll. */
	if (vel.bottom > vel.top + 2.f) {
		_ui.fill_round (vel, 3.f, ui::ColorToken::Surface, 0.55f);
		_ui.stroke_round (vel, 3.f, ui::ColorToken::Border, 1.f, 0.7f);
		_ui.text_vcenter (L"Vel", ui::TextRole::Caption, ui::ColorToken::TextMuted,
		                  D2D1::RectF (vel.left + 5.f * _dpi_scale, vel.top,
		                               vel.left + 34.f * _dpi_scale, vel.bottom));
		float vh = vel.bottom - vel.top;
		for (int i = 0; i < _engine_snap.midi_note_count; ++i) {
			const EngineMidiNoteSnap& n = _engine_snap.midi_notes[i];
			float x0 = _lanes[0].strip.x_at_time (r, n.start_sec);
			float x1 = _lanes[0].strip.x_at_time (r, n.start_sec + n.length_sec);
			if (x1 < vel.left || x0 > vel.right) {
				continue;
			}
			if (x1 - x0 < 2.f) {
				x1 = x0 + 2.f;
			}
			int show_v = n.velocity;
			if (_drag == DragMode::MidiVelocity
			    && (n.selected || _engine_snap.midi_selected_note_id < 0)) {
				/* Preview drag value for selected (or all when none selected). */
				bool have_sel = false;
				for (int j = 0; j < _engine_snap.midi_note_count; ++j) {
					if (_engine_snap.midi_notes[j].selected) {
						have_sel = true;
						break;
					}
				}
				if (!have_sel || n.selected) {
					show_v = _midi_vel_drag;
				}
			}
			float bar_h = vh * ((float)show_v / 127.f);
			if (bar_h < 1.f * _dpi_scale && show_v > 0) {
				bar_h = 1.f * _dpi_scale;
			}
			const ui::ColorToken tone = n.selected ? ui::ColorToken::Accent
			                                       : ui::ColorToken::Midi;
			_ui.fill_round (D2D1::RectF (x0, vel.bottom - bar_h, x1, vel.bottom - 1.f),
			                1.f, tone, n.selected ? 0.9f : 0.5f);
		}
		if (_drag == DragMode::MidiVelocity) {
			wchar_t vlab[24];
			_snwprintf_s (vlab, _TRUNCATE, L"v=%d", _midi_vel_drag);
			_ui.text_vcenter (vlab, ui::TextRole::Caption,
			                  ui::ColorToken::Accent,
			                  D2D1::RectF (vel.right - 52.f * _dpi_scale, vel.top,
			                               vel.right - 5.f * _dpi_scale, vel.bottom),
			                  DWRITE_TEXT_ALIGNMENT_TRAILING);
		}
	}

	/* In-progress draw rubber-band. */
	if (_drag == DragMode::MidiDraw) {
		float t0 = _midi_draw_start_sec;
		float t1 = midi_time_at_x (_pan_last_x);
		if (t1 < t0) {
			float tmp = t0;
			t0 = t1;
			t1 = tmp;
		}
		if (t1 - t0 < 0.05f) {
			t1 = t0 + 0.25f;
		}
		float x0 = _lanes[0].strip.x_at_time (r, t0);
		float x1 = _lanes[0].strip.x_at_time (r, t1);
		int row = (int)(kMidiPitchHi - _midi_draw_pitch);
		float y0 = piano.top + (float)row * row_h + 1.f;
		float y1 = y0 + row_h - 2.f;
		const D2D1_RECT_F nr = D2D1::RectF (x0, y0, x1, y1);
		_ui.fill_round (nr, 2.f, ui::ColorToken::Midi, 0.45f);
		_ui.stroke_round (nr, 2.f, ui::ColorToken::Midi, 1.f, 0.85f);
	}

	/* Playhead through piano-roll + velocity (matches audio/CC lanes). */
	if (_brush_playhead) {
		float ph = _lanes[0].strip.playhead_norm ();
		if (ph >= 0.f && ph <= 1.f) {
			float w = r.right - r.left;
			float px = r.left + ph * w;
			float thick = std::max (1.5f, 2.f * _dpi_scale);
			_rt->FillRectangle (
				D2D1::RectF (px - thick * 0.5f, r.top, px + thick * 0.5f, r.bottom),
				_brush_playhead);
		}
	}
	/* Marquee selection rubber-band. */
	if (_drag == DragMode::MidiMarquee && _brush_accent) {
		float t0 = _midi_marquee_t0;
		float t1 = midi_time_at_x (_pan_last_x);
		uint8_t p0 = _midi_marquee_p0;
		uint8_t p1 = midi_pitch_at_y (_pan_last_y);
		if (t1 < t0) {
			float tmp = t0;
			t0 = t1;
			t1 = tmp;
		}
		if (p1 < p0) {
			uint8_t tmp = p0;
			p0 = p1;
			p1 = tmp;
		}
		float x0 = _lanes[0].strip.x_at_time (r, t0);
		float x1 = _lanes[0].strip.x_at_time (r, t1);
		int row0 = (int)(kMidiPitchHi - p1);
		int row1 = (int)(kMidiPitchHi - p0);
		float y0 = piano.top + (float)row0 * row_h;
		float y1 = piano.top + (float)(row1 + 1) * row_h;
		_brush_accent->SetOpacity (0.22f);
		_rt->FillRectangle (D2D1::RectF (x0, y0, x1, y1), _brush_accent);
		_brush_accent->SetOpacity (0.85f);
		_rt->DrawRectangle (D2D1::RectF (x0, y0, x1, y1), _brush_accent,
		                    std::max (1.f, 1.25f * _dpi_scale));
		_brush_accent->SetOpacity (1.f);
	}

	wchar_t midicap[160];
	if (_engine_snap.midi_track_ok) {
		_snwprintf_s (midicap, _TRUNCATE,
		              L"%ls  ·  %d note%s  ·  %ls%s",
		              _engine_snap.midi_option.empty () ? L"MIDI" : _engine_snap.midi_option.c_str (),
		              _engine_snap.midi_note_count,
		              _engine_snap.midi_note_count == 1 ? L"" : L"s",
		              _engine_snap.midi_synth_ok
		                  ? (_engine_snap.midi_synth_name.empty () ? L"synth" : _engine_snap.midi_synth_name.c_str ())
		                  : L"no synth",
		              _engine_snap.midi_synth_to_master ? L"→master" : L"");
	} else {
		_snwprintf_s (midicap, _TRUNCATE, L"no MIDI track");
	}
	_ui.text_vcenter_n (midicap, (UINT32)wcslen (midicap),
	                   ui::TextRole::Caption, ui::ColorToken::TextSecondary,
	                   D2D1::RectF (piano.left + 6.f * _dpi_scale, piano.top,
	                                piano.right - 4.f * _dpi_scale,
	                                piano.top + 14.f * _dpi_scale));
}

void
ShellWindow::show_splash ()
{
	_about.set_splash_visible (true);
	_about.set_about_visible (false);
	_prefs_visible = false;
	_prefs.set_visible (false);
	_help_visible = false;
	_splash_shown_ms = GetTickCount ();
	if (_splash_shown_ms == 0) {
		_splash_shown_ms = 1; /* reserve 0 = inactive */
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::dismiss_splash ()
{
	if (!_about.splash_visible () && _splash_shown_ms == 0) {
		return;
	}
	_about.set_splash_visible (false);
	_splash_shown_ms = 0;
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::toggle_about ()
{
	dismiss_splash ();
	if (_prefs_visible) {
		_prefs_visible = false;
		_prefs.set_visible (false);
	}
	if (_help_visible) {
		_help_visible = false;
	}
	bool on = !_about.about_visible ();
	_about.set_about_visible (on);
	if (on) {
		_about.layout ((float)_width, (float)_height, _dpi_scale);
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::toggle_help ()
{
	dismiss_splash ();
	if (_prefs_visible) {
		_prefs_visible = false;
		_prefs.set_visible (false);
	}
	if (_about.about_visible ()) {
		_about.set_about_visible (false);
	}
	_menu_bar.close ();
	_help_visible = !_help_visible;
	if (_help_visible) {
		_help_scroll = 0;
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::apply_menu_command (MenuCommand cmd)
{
	switch (cmd) {
	case MenuCommand::FileNew:
		do_new_session ();
		break;
	case MenuCommand::FileOpen:
		prompt_open_session ();
		break;
	case MenuCommand::FileSave:
		do_save_session ();
		break;
	case MenuCommand::FileSaveAs:
		do_save_session_as ();
		break;
	case MenuCommand::FileImport:
		prompt_import_audio ();
		break;
	case MenuCommand::FileExportMix:
		prompt_export_wav ();
		break;
	case MenuCommand::FileExportStems:
		do_export_stems ();
		break;
	case MenuCommand::FileRecent:
		toggle_recent_menu ();
		break;
	case MenuCommand::EditUndo:
		do_undo ();
		break;
	case MenuCommand::EditRedo:
		do_redo ();
		break;
	case MenuCommand::EditDelete:
		do_delete_selection ();
		break;
	case MenuCommand::EditSplit:
		do_split_at_playhead ();
		break;
	case MenuCommand::EditCrossfade:
		do_apply_crossfade ();
		break;
	case MenuCommand::EditConsolidate:
		do_consolidate_selection ();
		break;
	case MenuCommand::EditSnap:
		do_toggle_snap ();
		break;
	case MenuCommand::TrackAddAudio:
		do_add_audio_track ();
		break;
	case MenuCommand::TrackAddMidi:
		do_add_midi_track ();
		break;
	case MenuCommand::TrackRemove:
		do_remove_track ();
		break;
	case MenuCommand::TrackLeft:
		do_move_audio_track (-1);
		break;
	case MenuCommand::TrackRight:
		do_move_audio_track (1);
		break;
	case MenuCommand::TrackRename:
		do_rename_focused_track ();
		break;
	case MenuCommand::TrackMute:
		if (_engine.is_linked ()) {
			_engine.toggle_track_muted (_focus_audio_track);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_mute_solo_from_engine ();
		}
		break;
	case MenuCommand::TrackSolo:
		if (_engine.is_linked ()) {
			_engine.toggle_track_soloed (_focus_audio_track);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_mute_solo_from_engine ();
		}
		break;
	case MenuCommand::TrackArm:
		if (_engine.is_linked ()) {
			const int t = _focus_audio_track;
			bool armed = (t >= 0 && t < EngineSnapshot::kMeters) ? _engine_snap.track_armed[t] : false;
			_engine.set_track_armed (t, !armed);
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_arm_from_engine ();
		}
		break;
	case MenuCommand::TrackFx: {
		const int t = _focus_audio_track;
		if (_engine.is_linked () && t >= 0 && t < n_audio_lanes ()) {
			if (_mixers[t].has_fx ()) {
				_engine.toggle_track_fx_bypass (t);
			} else {
				_engine.insert_track_fx (t);
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
		}
		break;
	}
	case MenuCommand::ViewZoomSession:
		do_zoom_to_session ();
		break;
	case MenuCommand::ViewFitRegions:
		do_fit_all_regions ();
		break;
	case MenuCommand::ViewHelp:
		toggle_help ();
		break;
	case MenuCommand::ViewPrefs:
		toggle_prefs ();
		break;
	case MenuCommand::ViewAbout:
		toggle_about ();
		break;
	case MenuCommand::PluginReverb:
	case MenuCommand::PluginComp:
	case MenuCommand::PluginEq:
	case MenuCommand::PluginDelay:
	case MenuCommand::PluginLimiter:
	case MenuCommand::PluginGate:
		if (_engine.is_linked ()) {
			const int t = std::max (0, std::min (_focus_audio_track, n_audio_lanes () - 1));
			switch (cmd) {
			case MenuCommand::PluginReverb:  _engine.insert_track_fx (t); break;
			case MenuCommand::PluginComp:    _engine.insert_track_comp (t); break;
			case MenuCommand::PluginEq:      _engine.insert_track_eq (t); break;
			case MenuCommand::PluginDelay:   _engine.insert_track_delay (t); break;
			case MenuCommand::PluginLimiter: _engine.insert_track_limiter (t); break;
			case MenuCommand::PluginGate:    _engine.insert_track_gate (t); break;
			default: break;
			}
			_engine.poll ();
			_engine_snap = _engine.snapshot ();
			sync_mixer_fx_from_engine ();
		}
		break;
	case MenuCommand::None:
		break;
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

bool
ShellWindow::on_mouse_down_right (LPARAM lparam)
{
	float x, y;
	client_from_lparam (lparam, &x, &y);

	if (_help_visible || _prefs_visible || _about.about_visible ()
	    || _recent.visible () || _recovery.visible () || _about.splash_visible ()) {
		return false;
	}
	_menu_bar.close ();

	static const MenuCommand lane_cmds[] = {
		MenuCommand::EditSplit, MenuCommand::EditCrossfade,
		MenuCommand::EditDelete, MenuCommand::EditConsolidate,
	};
	static const MenuCommand mix_cmds[] = {
		MenuCommand::TrackMute, MenuCommand::TrackSolo,
		MenuCommand::TrackArm, MenuCommand::TrackFx,
	};

	if (hit_editor_lane (x, y) >= 0) {
		_menu_bar.open_context (x, y, lane_cmds, (int)(sizeof (lane_cmds) / sizeof (lane_cmds[0])));
		InvalidateRect (_hwnd, nullptr, FALSE);
		return true;
	}
	const int strips = n_mixer_strips ();
	for (int i = 0; i < strips; ++i) {
		const D2D1_RECT_F& r = _mixers[i].bounds ();
		if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
			_focus_audio_track = std::min (i, n_audio_lanes () - 1);
			_menu_bar.open_context (x, y, mix_cmds, (int)(sizeof (mix_cmds) / sizeof (mix_cmds[0])));
			InvalidateRect (_hwnd, nullptr, FALSE);
			return true;
		}
	}
	return false;
}

bool
ShellWindow::smoke_ui_layout ()
{
	const UINT want_w = 1280;
	const UINT want_h = 720;
	RECT wr = { 0, 0, (LONG)want_w, (LONG)want_h };
	AdjustWindowRect (&wr, WS_OVERLAPPEDWINDOW, FALSE);
	SetWindowPos (_hwnd, nullptr, 0, 0,
	              wr.right - wr.left, wr.bottom - wr.top,
	              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	/* SetWindowPos delivers WM_SIZE synchronously; on_resize (re)lays out. */
	layout_panes ();

	const int rows = _transport.rows ();
	if (rows != 2) {
		std::fprintf (stderr, "airdour_win_shell: smoke-ui-layout FAILED (transport rows=%d at %ux%u)\n",
		              rows, (unsigned)_width, (unsigned)_height);
		std::fflush (stderr);
		return false;
	}
	const size_t nf = _transport.file_button_count ();
	if (nf < 4) {
		std::fprintf (stderr, "airdour_win_shell: smoke-ui-layout FAILED (file buttons=%u)\n",
		              (unsigned)nf);
		std::fflush (stderr);
		return false;
	}
	const float w = (float)_width;
	const float h = (float)_height;
	for (size_t i = 0; i < nf; ++i) {
		D2D1_RECT_F r;
		if (!_transport.file_button_rect (i, &r)) {
			continue;
		}
		if (r.left < 0.f || r.top < 0.f || r.right > w || r.bottom > h) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-ui-layout FAILED (file %u rect %.0f,%.0f-%.0f,%.0f outside %.0fx%.0f)\n",
			              (unsigned)i, r.left, r.top, r.right, r.bottom, w, h);
			std::fflush (stderr);
			return false;
		}
	}
	if (_menu_bar.bounds ().bottom <= _menu_bar.bounds ().top
	    || _menu_bar.bounds ().right > w) {
		std::fprintf (stderr, "airdour_win_shell: smoke-ui-layout FAILED (menu bar bounds)\n");
		std::fflush (stderr);
		return false;
	}

	/* Dropdown hit testing: File title opens a popup whose first row maps to
	 * FileNew. Exercises menu_at/item_at without posting synthetic clicks. */
	const D2D1_RECT_F mb = _menu_bar.bounds ();
	const float title_y = (mb.top + mb.bottom) * 0.5f;
	if (_menu_bar.menu_at (mb.left + 20.f * _dpi_scale, title_y) != 0) {
		std::fprintf (stderr, "airdour_win_shell: smoke-ui-layout FAILED (File title hit test)\n");
		std::fflush (stderr);
		return false;
	}
	D2D1_RECT_F file_rect;
	_menu_bar.menu_title_rect (0, &file_rect);
	_menu_bar.toggle_menu (0);
	if (!_menu_bar.open ()
	    || _menu_bar.item_at (file_rect.left + _ui.dp (12.f), mb.bottom + _ui.dp (13.f))
	       != MenuCommand::FileNew) {
		std::fprintf (stderr, "airdour_win_shell: smoke-ui-layout FAILED (File popup row hit test)\n");
		_menu_bar.close ();
		std::fflush (stderr);
		return false;
	}
	_menu_bar.close ();

	/* Context popup path used by lane / mixer right-clicks. */
	const MenuCommand ctx[] = { MenuCommand::TrackMute, MenuCommand::TrackSolo };
	_menu_bar.open_context (200.f, 300.f, ctx, 2);
	if (!_menu_bar.overlay_contains (210.f, 313.f)
	    || _menu_bar.item_at (210.f, 313.f) != MenuCommand::TrackMute) {
		std::fprintf (stderr, "airdour_win_shell: smoke-ui-layout FAILED (context menu hit test)\n");
		_menu_bar.close ();
		std::fflush (stderr);
		return false;
	}
	_menu_bar.close ();

	std::fprintf (stderr,
	              "airdour_win_shell: smoke-ui-layout ok (transport rows=%d, %u file buttons inside %ux%u, menus ok)\n",
	              rows, (unsigned)nf, (unsigned)_width, (unsigned)_height);
	std::fflush (stderr);
	return true;
}

bool
ShellWindow::smoke_track_capacity ()
{
	if (!_engine.is_linked ()) {
		std::fprintf (stderr, "airdour_win_shell: smoke-track-capacity FAILED (no engine)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	refresh_engine_snap ();
	if (!_engine_snap.session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-track-capacity FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	const int n0 = _engine_snap.audio_track_count;
	const int cap = EngineSnapshot::kMeters;
	int guard = 0;
	while (_engine_snap.audio_track_count < cap && guard++ < cap + 8) {
		if (!_engine.add_audio_track ()) {
			std::fprintf (stderr, "airdour_win_shell: smoke-track-capacity FAILED (add at %d)\n",
			              _engine_snap.audio_track_count);
			std::fflush (stderr);
			return false;
		}
		_engine.poll ();
		refresh_engine_snap ();
	}
	if (_engine_snap.audio_track_count != cap) {
		std::fprintf (stderr, "airdour_win_shell: smoke-track-capacity FAILED (count=%d, want %d)\n",
		              _engine_snap.audio_track_count, cap);
		std::fflush (stderr);
		return false;
	}

	_focus_audio_track = cap - 1;
	_editor_scroll_focus = -1;
	sync_track_labels_from_engine ();
	layout_panes ();
	const float cap_scroll = _editor_scroll_max;
	if (cap_scroll <= 0.f) {
		std::fprintf (stderr, "airdour_win_shell: smoke-track-capacity FAILED (no scroll at %d lanes)\n",
		              cap);
		std::fflush (stderr);
		return false;
	}
	const D2D1_RECT_F& last = _lanes[cap - 1].bounds;
	if (last.bottom > _editor_lanes_bot + 1.f || last.top < _editor_lanes_top - 1.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-track-capacity FAILED (focused lane %.0f..%.0f not in view %.0f..%.0f)\n",
		              last.top, last.bottom, _editor_lanes_top, _editor_lanes_bot);
		std::fflush (stderr);
		return false;
	}
	if (_auto_lane_track != cap - 1
	    || _auto_lane_bounds.bottom <= _auto_lane_bounds.top) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-track-capacity FAILED (auto lane target %d)\n",
		              _auto_lane_track);
		std::fflush (stderr);
		return false;
	}

	/* Automation lane must follow the focused track, not lane 0. */
	_focus_audio_track = cap - 8;
	_editor_scroll_focus = -1;
	layout_panes ();
	const int want = _focus_audio_track;
	const D2D1_RECT_F& wl = _lanes[want].bounds;
	if (_auto_lane_track != want
	    || std::fabs (_auto_lane_bounds.left - wl.left) > 1.f
	    || std::fabs (_auto_lane_bounds.right - wl.right) > 1.f
	    || _auto_lane_bounds.top < wl.bottom - 1.f
	    || _auto_lane_bounds.bottom > _editor_lanes_bot + 1.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-track-capacity FAILED (auto lane for focus %d: target %d lane %.0f..%.0f auto %.0f..%.0f)\n",
		              want, _auto_lane_track, wl.left, wl.right,
		              _auto_lane_bounds.top, _auto_lane_bounds.bottom);
		std::fflush (stderr);
		return false;
	}
	if (hit_editor_lane ((wl.left + wl.right) * 0.5f, (wl.top + wl.bottom) * 0.5f) != want
	    || hit_editor_lane (wl.left + 2.f, _editor_lanes_top - 4.f) >= 0
	    || hit_editor_lane (wl.left + 2.f, _editor_lanes_bot + 4.f) >= 0) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-track-capacity FAILED (lane hit-test focus %d)\n",
		              want);
		std::fflush (stderr);
		return false;
	}

	/* Real input path: wheel over the scrollbar scrolls the lane viewport. */
	_focus_audio_track = cap - 1;
	_editor_scroll_focus = -1;
	layout_panes ();
	const float before_y = _editor_scroll_y;
	POINT sp;
	sp.x = (LONG)((_editor_scrollbar.left + _editor_scrollbar.right) * 0.5f);
	sp.y = (LONG)((_editor_lanes_top + _editor_lanes_bot) * 0.5f);
	ClientToScreen (_hwnd, &sp);
	if (before_y <= 1.f
	    || !on_mouse_wheel (MAKEWPARAM (0, WHEEL_DELTA), MAKELPARAM (sp.x, sp.y))
	    || _editor_scroll_y >= before_y - 1.f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-track-capacity FAILED (wheel scroll %.1f→%.1f)\n",
		              before_y, _editor_scroll_y);
		std::fflush (stderr);
		return false;
	}

	/* Import target: selection wins, else focus; lands on that track only. */
	_engine_snap.selection_track = 3;
	_engine_snap.has_selection = true;
	const bool sel_ok = (import_target_track () == 3);
	_engine_snap.selection_track = -1;
	_engine_snap.has_selection = false;
	_focus_audio_track = 9;
	const bool focus_ok = (import_target_track () == 9);
	if (!sel_ok || !focus_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-track-capacity FAILED (import target sel=%d focus=%d)\n",
		              sel_ok, focus_ok);
		std::fflush (stderr);
		return false;
	}
	if (!_engine.import_test_tone (9, 330.f, 1.f)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-track-capacity FAILED (import tone)\n");
		std::fflush (stderr);
		return false;
	}
	refresh_engine_snap ();
	if (!_engine_snap.track_has_regions[9] || _engine_snap.track_has_regions[8]) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-track-capacity FAILED (import landed on wrong track)\n");
		std::fflush (stderr);
		return false;
	}

	guard = 0;
	while (_engine_snap.audio_track_count > n0 && guard++ < cap + 8) {
		const int last_i = _engine_snap.audio_track_count - 1;
		if (!_engine.remove_audio_track (last_i, false)) {
			std::fprintf (stderr, "airdour_win_shell: smoke-track-capacity FAILED (remove at %d)\n",
			              last_i);
			std::fflush (stderr);
			return false;
		}
		_engine.poll ();
		refresh_engine_snap ();
	}
	if (_engine_snap.audio_track_count != n0) {
		std::fprintf (stderr, "airdour_win_shell: smoke-track-capacity FAILED (shrink → %d, want %d)\n",
		              _engine_snap.audio_track_count, n0);
		std::fflush (stderr);
		return false;
	}
	_focus_audio_track = 0;
	_editor_scroll_focus = -1;
	sync_track_labels_from_engine ();
	sync_mixer_gains_from_engine ();
	sync_mixer_arm_from_engine ();
	sync_mixer_mute_solo_from_engine ();
	sync_mixer_pan_from_engine ();
	sync_mixer_send_from_engine ();
	layout_panes ();
	if (n0 <= 8 && _editor_scroll_max > 0.5f) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-track-capacity FAILED (%d lanes should fit, scroll %.1f)\n",
		              n0, _editor_scroll_max);
		std::fflush (stderr);
		return false;
	}

	std::fprintf (stderr,
	              "airdour_win_shell: smoke-track-capacity ok (%d → %d → %d, scroll %.0f px at %d lanes, import→Audio %d)\n",
	              n0, cap, _engine_snap.audio_track_count, cap_scroll, cap, 10);
	std::fflush (stderr);
	return true;
}

void
ShellWindow::sync_prefs_from_engine ()
{
	_prefs.set_current_backend (_engine_snap.backend.c_str ());
	_prefs.set_pref_backend (_engine_snap.pref_backend.c_str ());
	_prefs.set_current_period (_engine_snap.period_frames, _engine_snap.sample_rate);
	_prefs.set_pref_period (_engine_snap.pref_period_frames);
	_prefs.set_current_devices (_engine_snap.audio_in_name.c_str (), _engine_snap.audio_out_name.c_str ());
	_prefs.set_pref_devices (_engine_snap.pref_in_device.c_str (), _engine_snap.pref_out_device.c_str ());
	_prefs.set_systemic_latency (_engine_snap.systemic_in_frames, _engine_snap.systemic_out_frames);
	_prefs.set_separate_devices (_engine_snap.separate_io_devices);
	_prefs.set_click_default (_engine_snap.pref_click_default);
	_prefs.set_snap_default (_engine_snap.pref_snap_default);
	_prefs.set_vst3_hint (_engine_snap.vst3_path_hint);
	_prefs.set_vst3_catalog (_engine_snap.vst3_scan_count,
	                         _engine_snap.vst3_catalog_name,
	                         _engine_snap.vst3_catalog_count);
	if (_engine_snap.vst3_ok && _engine_snap.vst3_name[0] != L'\0') {
		_prefs.set_vst3_loaded_name (_engine_snap.vst3_name);
	} else {
		_prefs.set_vst3_loaded_name (nullptr);
	}
	_prefs.set_vst3_ok (_engine_snap.vst3_ok);
	_prefs.set_vst3_param_count (_engine_snap.vst3_ok ? _engine_snap.vst3_param_count : 0);
	_prefs.set_vst3_bypassed (_engine_snap.vst3_bypassed);
}

void
ShellWindow::toggle_prefs ()
{
	dismiss_splash ();
	if (_help_visible) {
		_help_visible = false;
	}
	if (_about.about_visible ()) {
		_about.set_about_visible (false);
	}
	_prefs_visible = !_prefs_visible;
	_prefs.set_visible (_prefs_visible);
	if (_prefs_visible) {
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_prefs_from_engine ();
		_prefs.set_status (L"");
		_prefs.layout ((float)_width, (float)_height, _dpi_scale);
	}
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::apply_prefs_vst3_catalog_click (int index)
{
	if (!_engine.is_linked ()) {
		_prefs.set_status (L"No engine");
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	}
	if (_engine.insert_vst3_by_index (index)) {
		_prefs.set_status (L"VST3 inserted");
	} else {
		_prefs.set_status (L"VST3 insert skipped");
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_fx_from_engine ();
	sync_prefs_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

bool
ShellWindow::smoke_vst3_prefs_click ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vst3-prefs-click FAILED (no session)\n");
		return false;
	}
	if (!_engine.rescan_vst3 ()) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vst3-prefs-click FAILED (rescan)\n");
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_prefs_from_engine ();
	_prefs_visible = true;
	_prefs.set_visible (true);
	_prefs.layout ((float)_width, (float)_height, _dpi_scale);

	float cx = 0.f, cy = 0.f;
	if (!_prefs.catalog_row_center (0, &cx, &cy)) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vst3-prefs-click FAILED (row geometry)\n");
		return false;
	}

	if (_engine_snap.vst3_catalog_count <= 0) {
		if (_prefs.hit_vst3_catalog_index (cx, cy) >= 0) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-prefs-click FAILED "
			              "(empty catalog hit index)\n");
			return false;
		}
		/* Direct insert still rejected — mirrors Prefs no-op when empty. */
		if (_engine.insert_vst3_by_index (0)) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-prefs-click FAILED "
			              "(empty insert unexpectedly ok)\n");
			return false;
		}
		_prefs_visible = false;
		_prefs.set_visible (false);
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-prefs-click ok "
		              "(scan-only; empty catalog no hit; set VST3_PATH to insert)\n");
		return true;
	}

	int hit = _prefs.hit_vst3_catalog_index (cx, cy);
	if (hit != 0) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-prefs-click FAILED (hit=%d want=0)\n",
		              hit);
		return false;
	}
	const int catalog_n = _engine_snap.vst3_catalog_count;
	apply_prefs_vst3_catalog_click (0);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	/* Insert may skip instruments — either vst3_ok or honest skip is PASS. */
	if (_engine_snap.vst3_ok) {
		(void)_engine.unload_vst3 ();
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_fx_from_engine ();
	}
	_prefs_visible = false;
	_prefs.set_visible (false);
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-vst3-prefs-click ok "
	              "(catalog=%d row0 hit→insert path)\n",
	              catalog_n);
	return true;
}

bool
ShellWindow::smoke_vst3_mixer_unload ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vst3-mixer-unload FAILED (no session)\n");
		return false;
	}
	layout_panes ();
	sync_mixer_fx_from_engine ();

	if (!_engine_snap.vst3_ok) {
		/* Empty / scan-only: × must not hit; unload is still a safe no-op. */
		if (_mixers[1].fx_can_unload () || _mixers[1].hit_fx_unload (
		        0.5f * (_mixers[1].bounds ().left + _mixers[1].bounds ().right),
		        0.5f * (_mixers[1].bounds ().top + _mixers[1].bounds ().bottom))) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-mixer-unload FAILED "
			              "(unload chrome without VST3)\n");
			return false;
		}
		if (!_engine.unload_vst3 ()) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-mixer-unload FAILED "
			              "(idle unload)\n");
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-mixer-unload ok "
		              "(scan-only; no ×; idle unload; set VST3_PATH to insert)\n");
		return true;
	}

	/* Live VST3: × hit → unload → !vst3_ok. */
	sync_mixer_fx_from_engine ();
	layout_panes ();
	if (!_mixers[1].fx_can_unload () || !_mixers[1].has_fx ()) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-mixer-unload FAILED "
		              "(no unload chrome while vst3_ok)\n");
		return false;
	}
	/* Center of unload chip = right edge of FX row. */
	const D2D1_RECT_F& b = _mixers[1].bounds ();
	float fx_top = b.top + 80.f * _dpi_scale; /* approximate; use hit scan */
	bool hit = false;
	for (float y = b.top; y < b.bottom; y += 2.f * _dpi_scale) {
		for (float x = b.right - 20.f * _dpi_scale; x < b.right; x += 2.f * _dpi_scale) {
			if (_mixers[1].hit_fx_unload (x, y)) {
				hit = true;
				(void)_engine.unload_vst3 ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_fx_from_engine ();
				goto after_hit;
			}
		}
	}
after_hit:
	if (!hit) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-mixer-unload FAILED (× not hittable)\n");
		return false;
	}
	(void)fx_top;
	if (_engine_snap.vst3_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-mixer-unload FAILED (still loaded)\n");
		return false;
	}
	std::fprintf (stderr, "airdour_win_shell: smoke-vst3-mixer-unload ok (× → unload)\n");
	return true;
}

bool
ShellWindow::smoke_vst3_mixer_bypass ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr, "airdour_win_shell: smoke-vst3-mixer-bypass FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	layout_panes ();
	sync_mixer_fx_from_engine ();

	if (!_engine_snap.vst3_ok) {
		/* Empty / scan-only: B chip must not hit; toggle idle is ok. */
		const D2D1_RECT_F& b = _mixers[1].bounds ();
		bool chrome = false;
		for (float y = b.top; y < b.bottom && !chrome; y += 2.f * _dpi_scale) {
			for (float x = b.right - 40.f * _dpi_scale; x < b.right; x += 2.f * _dpi_scale) {
				if (_mixers[1].hit_fx_bypass (x, y)) {
					chrome = true;
					break;
				}
			}
		}
		if (chrome) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-mixer-bypass FAILED "
			              "(B chrome without VST3)\n");
			std::fflush (stderr);
			return false;
		}
		(void)_engine.toggle_vst3_bypass ();
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-mixer-bypass ok "
		              "(scan-only; no B; idle toggle; set VST3_PATH to insert)\n");
		std::fflush (stderr);
		return true;
	}

	sync_mixer_fx_from_engine ();
	layout_panes ();
	if (!_mixers[1].fx_can_unload () || !_mixers[1].has_fx ()) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-mixer-bypass FAILED "
		              "(no B chrome while vst3_ok)\n");
		std::fflush (stderr);
		return false;
	}
	const bool was = _engine_snap.vst3_bypassed;
	const D2D1_RECT_F& b = _mixers[1].bounds ();
	bool hit = false;
	for (float y = b.top; y < b.bottom; y += 2.f * _dpi_scale) {
		for (float x = b.right - 40.f * _dpi_scale; x < b.right; x += 2.f * _dpi_scale) {
			if (_mixers[1].hit_fx_bypass (x, y)) {
				hit = true;
				(void)_engine.toggle_vst3_bypass ();
				_engine.poll ();
				_engine_snap = _engine.snapshot ();
				sync_mixer_fx_from_engine ();
				goto after_bypass_hit;
			}
		}
	}
after_bypass_hit:
	if (!hit) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-mixer-bypass FAILED (B not hittable)\n");
		std::fflush (stderr);
		return false;
	}
	if (_engine_snap.vst3_bypassed == was) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-mixer-bypass FAILED (state)\n");
		std::fflush (stderr);
		return false;
	}
	(void)_engine.toggle_vst3_bypass ();
	std::fprintf (stderr, "airdour_win_shell: smoke-vst3-mixer-bypass ok (B → toggle)\n");
	std::fflush (stderr);
	return true;
}

bool
ShellWindow::smoke_vst3_mixer_param_reset ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-mixer-param-chips FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_fx_from_engine ();
	layout_panes ();

	const D2D1_RECT_F& b = _mixers[1].bounds ();
	const float x0 = b.right - 80.f * _dpi_scale;
	if (!_engine_snap.vst3_ok) {
		bool chrome = false;
		for (float y = b.top; y < b.bottom && !chrome; y += 2.f * _dpi_scale) {
			for (float x = x0; x < b.right; x += 2.f * _dpi_scale) {
				if (_mixers[1].hit_fx_param_reset (x, y) >= 0) {
					chrome = true;
					break;
				}
			}
		}
		if (chrome) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-mixer-param-chips FAILED "
			              "(chrome without VST3)\n");
			std::fflush (stderr);
			return false;
		}
		(void)_engine.reset_vst3_param (0);
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-mixer-param-chips ok "
		              "(scan-only; set VST3_PATH to insert)\n");
		std::fflush (stderr);
		return true;
	}

	if (!_mixers[1].fx_can_unload () || !_mixers[1].has_fx ()) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-mixer-param-chips FAILED "
		              "(no chrome while vst3_ok)\n");
		std::fflush (stderr);
		return false;
	}

	int n = _engine_snap.vst3_param_count;
	if (n < 0) {
		n = 0;
	}
	if (n > EngineSnapshot::kMaxVst3Params) {
		n = EngineSnapshot::kMaxVst3Params;
	}
	bool seen[EngineSnapshot::kMaxVst3Params] = {};
	for (float y = b.top; y < b.bottom; y += 2.f * _dpi_scale) {
		for (float x = x0; x < b.right; x += 2.f * _dpi_scale) {
			const int hit = _mixers[1].hit_fx_param_reset (x, y);
			if (hit >= 0 && hit < n && !seen[hit]) {
				seen[hit] = true;
				(void)_engine.reset_vst3_param (hit);
			}
		}
	}
	for (int i = 0; i < n; ++i) {
		if (!seen[i]) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-mixer-param-chips FAILED (hit %d)\n",
			              i);
			std::fflush (stderr);
			return false;
		}
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_fx_from_engine ();
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-vst3-mixer-param-chips ok (%d → reset)\n",
	              n);
	std::fflush (stderr);
	return true;
}

bool
ShellWindow::smoke_vst3_catalog_highlight ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-catalog-highlight FAILED (no session)\n");
		return false;
	}
	sync_prefs_from_engine ();
	_prefs.layout ((float)_width, (float)_height, _dpi_scale);

	if (_engine_snap.vst3_catalog_count <= 0) {
		if (_prefs.vst3_loaded_catalog_index () >= 0) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-catalog-highlight FAILED "
			              "(highlight on empty catalog)\n");
			return false;
		}
		/* Synthetic catalog + loaded name exercises highlight without plugins. */
		wchar_t names[PrefsPanel::kMaxCatalog][PrefsPanel::kCatalogNameLen];
		for (int i = 0; i < PrefsPanel::kMaxCatalog; ++i) {
			names[i][0] = L'\0';
		}
		wcsncpy_s (names[0], PrefsPanel::kCatalogNameLen, L"AlphaFX", _TRUNCATE);
		wcsncpy_s (names[1], PrefsPanel::kCatalogNameLen, L"BetaFX", _TRUNCATE);
		_prefs.set_vst3_catalog (2, names, 2);
		_prefs.set_vst3_loaded_name (L"BetaFX");
		if (_prefs.vst3_loaded_catalog_index () != 1) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-catalog-highlight FAILED "
			              "(want index 1 got %d)\n",
			              _prefs.vst3_loaded_catalog_index ());
			return false;
		}
		_prefs.set_vst3_loaded_name (nullptr);
		if (_prefs.vst3_loaded_catalog_index () >= 0) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-catalog-highlight FAILED "
			              "(clear failed)\n");
			return false;
		}
		sync_prefs_from_engine (); /* restore real empty catalog */
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-catalog-highlight ok "
		              "(scan-only; synthetic row1 highlight)\n");
		return true;
	}

	/* Plugins present: if vst3_ok, loaded name should match a catalog row. */
	sync_prefs_from_engine ();
	if (_engine_snap.vst3_ok) {
		int hi = _prefs.vst3_loaded_catalog_index ();
		if (hi < 0) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-catalog-highlight FAILED "
			              "(live insert not in catalog)\n");
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-catalog-highlight ok "
		              "(live row=%d)\n",
		              hi);
		return true;
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-vst3-catalog-highlight ok "
	              "(catalog=%d no live insert)\n",
	              _engine_snap.vst3_catalog_count);
	return true;
}

bool
ShellWindow::hit_status_vst3_unload (float x, float y) const
{
	if (!_engine_snap.vst3_ok) {
		return false;
	}
	const D2D1_RECT_F& r = _status_vst3_unload_btn;
	return r.right > r.left && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

bool
ShellWindow::hit_status_vst3_bypass (float x, float y) const
{
	if (!_engine_snap.vst3_ok) {
		return false;
	}
	const D2D1_RECT_F& r = _status_vst3_bypass_btn;
	return r.right > r.left && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

int
ShellWindow::hit_status_vst3_param_reset (float x, float y) const
{
	if (!_engine_snap.vst3_ok) {
		return -1;
	}
	int n = _engine_snap.vst3_param_count;
	if (n < 0) {
		n = 0;
	}
	if (n > EngineSnapshot::kMaxVst3Params) {
		n = EngineSnapshot::kMaxVst3Params;
	}
	for (int i = 0; i < n; ++i) {
		const D2D1_RECT_F& r = _status_vst3_param_reset_btn[i];
		if (r.right > r.left && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
			return i;
		}
	}
	return -1;
}

bool
ShellWindow::status_vst3_param_reset_center (int index, float* out_x, float* out_y) const
{
	if (!out_x || !out_y || index < 0 || index >= EngineSnapshot::kMaxVst3Params) {
		return false;
	}
	if (index >= _engine_snap.vst3_param_count) {
		return false;
	}
	const D2D1_RECT_F& r = _status_vst3_param_reset_btn[index];
	if (r.right <= r.left) {
		return false;
	}
	*out_x = 0.5f * (r.left + r.right);
	*out_y = 0.5f * (r.top + r.bottom);
	return true;
}

bool
ShellWindow::status_vst3_unload_center (float* out_x, float* out_y) const
{
	if (!out_x || !out_y) {
		return false;
	}
	const D2D1_RECT_F& r = _status_vst3_unload_btn;
	if (r.right <= r.left) {
		return false;
	}
	*out_x = 0.5f * (r.left + r.right);
	*out_y = 0.5f * (r.top + r.bottom);
	return true;
}

bool
ShellWindow::status_vst3_bypass_center (float* out_x, float* out_y) const
{
	if (!out_x || !out_y) {
		return false;
	}
	const D2D1_RECT_F& r = _status_vst3_bypass_btn;
	if (r.right <= r.left) {
		return false;
	}
	*out_x = 0.5f * (r.left + r.right);
	*out_y = 0.5f * (r.top + r.bottom);
	return true;
}

bool
ShellWindow::smoke_vst3_status_unload ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-status-unload FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	layout_panes ();
	/* Force a paint pass layout of the × chip (rects set in on_paint). */
	InvalidateRect (_hwnd, nullptr, FALSE);
	UpdateWindow (_hwnd);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();

	if (!_engine_snap.vst3_ok) {
		/* Empty / scan-only: no × chrome; idle unload still ok. */
		float sx = 0.f;
		float sy = 0.f;
		if (status_vst3_unload_center (&sx, &sy) || hit_status_vst3_unload (sx, sy)) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-status-unload FAILED "
			              "(× chrome without VST3)\n");
			std::fflush (stderr);
			return false;
		}
		if (!_engine.unload_vst3 ()) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-status-unload FAILED "
			              "(idle unload)\n");
			std::fflush (stderr);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-status-unload ok "
		              "(scan-only; no ×; idle unload; set VST3_PATH to insert)\n");
		std::fflush (stderr);
		return true;
	}

	float sx = 0.f;
	float sy = 0.f;
	if (!status_vst3_unload_center (&sx, &sy) || !hit_status_vst3_unload (sx, sy)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-status-unload FAILED (× not hittable)\n");
		std::fflush (stderr);
		return false;
	}
	if (!_engine.unload_vst3 ()) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-status-unload FAILED (unload)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (_engine_snap.vst3_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-status-unload FAILED (still loaded)\n");
		std::fflush (stderr);
		return false;
	}
	std::fprintf (stderr, "airdour_win_shell: smoke-vst3-status-unload ok (× → unload)\n");
	std::fflush (stderr);
	return true;
}

bool
ShellWindow::smoke_vst3_status_bypass ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-status-bypass FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
	UpdateWindow (_hwnd);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();

	if (!_engine_snap.vst3_ok) {
		float sx = 0.f;
		float sy = 0.f;
		if (status_vst3_bypass_center (&sx, &sy) || hit_status_vst3_bypass (sx, sy)) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-status-bypass FAILED "
			              "(BYP chrome without VST3)\n");
			std::fflush (stderr);
			return false;
		}
		(void)_engine.toggle_vst3_bypass ();
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-status-bypass ok "
		              "(scan-only; no BYP; idle toggle; set VST3_PATH to insert)\n");
		std::fflush (stderr);
		return true;
	}

	float sx = 0.f;
	float sy = 0.f;
	if (!status_vst3_bypass_center (&sx, &sy) || !hit_status_vst3_bypass (sx, sy)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-status-bypass FAILED (BYP not hittable)\n");
		std::fflush (stderr);
		return false;
	}
	const bool was = _engine_snap.vst3_bypassed;
	if (!_engine.toggle_vst3_bypass ()) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-status-bypass FAILED (toggle)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (_engine_snap.vst3_bypassed == was) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-status-bypass FAILED (state)\n");
		std::fflush (stderr);
		return false;
	}
	(void)_engine.toggle_vst3_bypass ();
	std::fprintf (stderr, "airdour_win_shell: smoke-vst3-status-bypass ok (BYP → toggle)\n");
	std::fflush (stderr);
	return true;
}

bool
ShellWindow::smoke_vst3_status_param_reset ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-status-param-reset FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	layout_panes ();
	InvalidateRect (_hwnd, nullptr, FALSE);
	UpdateWindow (_hwnd);

	const int slots = EngineSnapshot::kMaxVst3Params;
	if (!_engine_snap.vst3_ok) {
		for (int i = 0; i < slots; ++i) {
			float sx = 0.f;
			float sy = 0.f;
			if (status_vst3_param_reset_center (i, &sx, &sy)
			    || hit_status_vst3_param_reset (sx, sy) == i) {
				std::fprintf (stderr,
				              "airdour_win_shell: smoke-vst3-status-param-reset FAILED "
				              "(chrome without VST3)\n");
				std::fflush (stderr);
				return false;
			}
		}
		(void)_engine.reset_vst3_param (0);
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-status-param-reset ok "
		              "(scan-only; set VST3_PATH to insert)\n");
		std::fflush (stderr);
		return true;
	}

	int n = _engine_snap.vst3_param_count;
	if (n < 0) {
		n = 0;
	}
	if (n > slots) {
		n = slots;
	}
	for (int i = 0; i < n; ++i) {
		float sx = 0.f;
		float sy = 0.f;
		if (!status_vst3_param_reset_center (i, &sx, &sy)
		    || hit_status_vst3_param_reset (sx, sy) != i) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-status-param-reset FAILED "
			              "(idx %d not hittable)\n",
			              i);
			std::fflush (stderr);
			return false;
		}
		const float cur = _engine_snap.vst3_param_norm[i];
		(void)_engine.set_vst3_param (i, (cur < 0.5f) ? 0.85f : 0.15f);
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		if (!_engine.reset_vst3_param (i)) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-status-param-reset FAILED (reset %d)\n",
			              i);
			std::fflush (stderr);
			return false;
		}
	}
	for (int i = n; i < slots; ++i) {
		float sx = 0.f;
		float sy = 0.f;
		if (status_vst3_param_reset_center (i, &sx, &sy)) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-status-param-reset FAILED "
			              "(extra idx %d)\n",
			              i);
			std::fflush (stderr);
			return false;
		}
	}
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-vst3-status-param-reset ok (%d → default)\n",
	              n);
	std::fflush (stderr);
	return true;
}

bool
ShellWindow::smoke_vst3_prefs_bypass ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-prefs-bypass FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	sync_prefs_from_engine ();
	_prefs.set_visible (true);
	_prefs_visible = true;
	_prefs.layout ((float)_width, (float)_height, _dpi_scale);

	float bx = 0.f;
	float by = 0.f;
	if (!_prefs.vst3_bypass_btn_center (&bx, &by)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-prefs-bypass FAILED (layout)\n");
		std::fflush (stderr);
		return false;
	}
	if (_prefs.hit_test (bx, by) != PrefsHit::Vst3Bypass) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-prefs-bypass FAILED (hit)\n");
		std::fflush (stderr);
		return false;
	}

	apply_prefs_hit (PrefsHit::Vst3Bypass);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();

	if (!_engine_snap.vst3_ok) {
		/* FENIX empty: toggle is a no-op; chrome still hittable. */
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-prefs-bypass ok "
		              "(scan-only; Bypass chrome hittable)\n");
		std::fflush (stderr);
		_prefs_visible = false;
		_prefs.set_visible (false);
		return true;
	}

	const bool was = _engine_snap.vst3_bypassed;
	if (!_engine.toggle_vst3_bypass ()) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-prefs-bypass FAILED (toggle)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (_engine_snap.vst3_bypassed == was) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-prefs-bypass FAILED (state)\n");
		std::fflush (stderr);
		return false;
	}
	(void)_engine.toggle_vst3_bypass ();
	_prefs_visible = false;
	_prefs.set_visible (false);
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-vst3-prefs-bypass ok (live toggle)\n");
	std::fflush (stderr);
	return true;
}

bool
ShellWindow::smoke_vst3_prefs_param_reset ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-prefs-param-reset FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_prefs_from_engine ();
	_prefs.set_visible (true);
	_prefs_visible = true;
	_prefs.layout ((float)_width, (float)_height, _dpi_scale);

	const int slots = EngineSnapshot::kMaxVst3Params;
	if (!_engine_snap.vst3_ok) {
		for (int i = 0; i < slots; ++i) {
			float rx = 0.f;
			float ry = 0.f;
			if (_prefs.vst3_param_reset_btn_center (i, &rx, &ry)
			    || _prefs.hit_vst3_param_reset (rx, ry) == i) {
				std::fprintf (stderr,
				              "airdour_win_shell: smoke-vst3-prefs-param-reset FAILED "
				              "(Reset chrome without VST3)\n");
				std::fflush (stderr);
				_prefs_visible = false;
				_prefs.set_visible (false);
				return false;
			}
		}
		(void)_engine.reset_vst3_param (0);
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-prefs-param-reset ok "
		              "(scan-only; set VST3_PATH to insert)\n");
		std::fflush (stderr);
		_prefs_visible = false;
		_prefs.set_visible (false);
		return true;
	}

	int n = _engine_snap.vst3_param_count;
	if (n < 0) {
		n = 0;
	}
	if (n > slots) {
		n = slots;
	}
	for (int i = 0; i < n; ++i) {
		float rx = 0.f;
		float ry = 0.f;
		if (!_prefs.vst3_param_reset_btn_center (i, &rx, &ry)
		    || _prefs.hit_vst3_param_reset (rx, ry) != i) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-prefs-param-reset FAILED (idx %d)\n",
			              i);
			std::fflush (stderr);
			_prefs_visible = false;
			_prefs.set_visible (false);
			return false;
		}
		const float cur = _engine_snap.vst3_param_norm[i];
		(void)_engine.set_vst3_param (i, (cur < 0.5f) ? 0.85f : 0.15f);
		apply_vst3_param_reset (i);
	}
	for (int i = n; i < slots; ++i) {
		float rx = 0.f;
		float ry = 0.f;
		if (_prefs.vst3_param_reset_btn_center (i, &rx, &ry)) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-prefs-param-reset FAILED (extra %d)\n",
			              i);
			std::fflush (stderr);
			_prefs_visible = false;
			_prefs.set_visible (false);
			return false;
		}
	}
	_prefs_visible = false;
	_prefs.set_visible (false);
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-vst3-prefs-param-reset ok (%d → default)\n",
	              n);
	std::fflush (stderr);
	return true;
}

bool
ShellWindow::smoke_vst3_prefs_unload ()
{
	if (!_engine.is_linked () || !_engine.snapshot ().session_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-prefs-unload FAILED (no session)\n");
		std::fflush (stderr);
		return false;
	}
	sync_prefs_from_engine ();
	_prefs.set_visible (true);
	_prefs_visible = true;
	_prefs.layout ((float)_width, (float)_height, _dpi_scale);

	float ux = 0.f;
	float uy = 0.f;
	if (!_engine_snap.vst3_ok) {
		/* Empty / scan-only: no Unload chrome; idle unload still ok. */
		if (_prefs.vst3_unload_btn_center (&ux, &uy)
		    || _prefs.hit_test (ux, uy) == PrefsHit::Vst3Unload) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-prefs-unload FAILED "
			              "(Unload chrome without VST3)\n");
			std::fflush (stderr);
			_prefs_visible = false;
			_prefs.set_visible (false);
			return false;
		}
		if (!_engine.unload_vst3 ()) {
			std::fprintf (stderr,
			              "airdour_win_shell: smoke-vst3-prefs-unload FAILED "
			              "(idle unload)\n");
			std::fflush (stderr);
			_prefs_visible = false;
			_prefs.set_visible (false);
			return false;
		}
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-prefs-unload ok "
		              "(scan-only; no Unload; idle unload; set VST3_PATH to insert)\n");
		std::fflush (stderr);
		_prefs_visible = false;
		_prefs.set_visible (false);
		return true;
	}

	if (!_prefs.vst3_unload_btn_center (&ux, &uy)) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-prefs-unload FAILED (layout)\n");
		std::fflush (stderr);
		return false;
	}
	if (_prefs.hit_test (ux, uy) != PrefsHit::Vst3Unload) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-prefs-unload FAILED (hit)\n");
		std::fflush (stderr);
		return false;
	}

	apply_prefs_hit (PrefsHit::Vst3Unload);
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	if (_engine_snap.vst3_ok) {
		std::fprintf (stderr,
		              "airdour_win_shell: smoke-vst3-prefs-unload FAILED (still loaded)\n");
		std::fflush (stderr);
		return false;
	}
	_prefs_visible = false;
	_prefs.set_visible (false);
	std::fprintf (stderr,
	              "airdour_win_shell: smoke-vst3-prefs-unload ok (Unload → clear)\n");
	std::fflush (stderr);
	return true;
}

void
ShellWindow::apply_vst3_param_reset (int index)
{
	if (index < 0 || index >= EngineSnapshot::kMaxVst3Params) {
		return;
	}
	if (_engine.reset_vst3_param (index)) {
		wchar_t st[48];
		_snwprintf_s (st, _TRUNCATE, L"VST3 param%d reset", index);
		_prefs.set_status (st);
	} else {
		_prefs.set_status (L"VST3 reset (none loaded)");
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_mixer_fx_from_engine ();
	sync_prefs_from_engine ();
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::apply_prefs_hit (PrefsHit hit)
{
	switch (hit) {
	case PrefsHit::Close:
		_prefs_visible = false;
		_prefs.set_visible (false);
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	case PrefsHit::BackendDummy:
		_engine.set_pref_backend ("Dummy");
		_prefs.set_status (L"Backend change applies on relaunch");
		break;
	case PrefsHit::BackendWasapi:
		_engine.set_pref_backend ("WASAPI");
		_prefs.set_status (L"Backend change applies on relaunch");
		break;
	case PrefsHit::PeriodAuto:
		_engine.set_pref_period_frames (0);
		break;
	case PrefsHit::Period480:
		_engine.set_pref_period_frames (480);
		break;
	case PrefsHit::Period512:
		_engine.set_pref_period_frames (512);
		break;
	case PrefsHit::ClickToggle:
		_engine.set_pref_click_default (!_engine.snapshot ().pref_click_default);
		break;
	case PrefsHit::SnapToggle:
		_engine.set_pref_snap_default (!_engine.snapshot ().pref_snap_default);
		break;
	case PrefsHit::Vst3Rescan:
		if (_engine.rescan_vst3 ()) {
			_prefs.set_status (L"VST3 rescanned");
		} else {
			_prefs.set_status (L"VST3 rescan failed");
		}
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_fx_from_engine ();
		sync_prefs_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	case PrefsHit::Vst3Bypass:
		if (_engine.toggle_vst3_bypass ()) {
			_prefs.set_status (_engine.snapshot ().vst3_bypassed
			                       ? L"VST3 bypassed"
			                       : L"VST3 active");
		} else {
			_prefs.set_status (L"VST3 bypass (none loaded)");
		}
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_fx_from_engine ();
		sync_prefs_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	case PrefsHit::Vst3Unload:
		if (_engine.unload_vst3 ()) {
			_prefs.set_status (L"VST3 unloaded");
		} else {
			_prefs.set_status (L"VST3 unload failed");
		}
		_engine.poll ();
		_engine_snap = _engine.snapshot ();
		sync_mixer_fx_from_engine ();
		sync_prefs_from_engine ();
		InvalidateRect (_hwnd, nullptr, FALSE);
		return;
	case PrefsHit::OutDevicePrev:
		_engine.cycle_audio_device (true, -1);
		break;
	case PrefsHit::OutDeviceNext:
		_engine.cycle_audio_device (true, +1);
		break;
	case PrefsHit::InDevicePrev:
		_engine.cycle_audio_device (false, -1);
		break;
	case PrefsHit::InDeviceNext:
		_engine.cycle_audio_device (false, +1);
		break;
	case PrefsHit::RestartAudio:
		if (_engine.restart_audio ()) {
			_prefs.set_status (L"Audio restarted");
		} else {
			_prefs.set_status (L"Restart failed");
		}
		break;
	case PrefsHit::Save:
		if (_engine.save_shell_prefs ()) {
			_prefs.set_status (L"Saved");
		} else {
			_prefs.set_status (L"Save failed");
		}
		break;
	case PrefsHit::None:
	default:
		return;
	}
	_engine.poll ();
	_engine_snap = _engine.snapshot ();
	sync_prefs_from_engine ();
	sync_transport_tempo_from_engine ();
	_transport.set_snap_enabled (_engine_snap.snap_enabled);
	_transport.set_snap_density (_engine_snap.snap_density);
	InvalidateRect (_hwnd, nullptr, FALSE);
}

void
ShellWindow::draw_help_overlay ()
{
	if (!_rt || !_ui.ready ()) {
		return;
	}

	struct Row { const wchar_t* key; const wchar_t* action; };
	static const Row kRows[] = {
		{ nullptr, L"SESSION" },
		{ L"Space", L"Play / Stop" },
		{ L"Esc", L"Close help / panel · clear selection" },
		{ L"F1  /  ?", L"Toggle this help" },
		{ L"F3", L"About Airdour" },
		{ L"Ctrl+,", L"Preferences" },
		{ L"Ctrl+Z / Y", L"Undo / Redo" },
		{ L"Ctrl+N", L"New session (folder + name)" },
		{ L"Ctrl+S / Shift+S", L"Save / Save As (copy media)" },
		{ L"Ctrl+O / Ctrl+Shift+O", L"Open folder / Recent sessions menu" },
		{ L"Ctrl+E", L"Export master WAV" },
		{ L"Alt+Shift+E", L"Export per-track stem WAV files" },
		{ L"Alt+E", L"Export selection time range WAV" },
		{ L"C / K", L"Count-in · Click (metronome)" },
		{ L"− / +", L"Tempo −1 / +1 BPM" },
		{ L"G / Shift+G", L"Snap on/off · cycle density" },
		{ L"Home / 0", L"Follow transport (reset view)" },
		{ L"Shift+Home / End", L"Locate to selection start / end" },
		{ L"Ctrl+Home / End", L"Locate session start / end" },
		{ nullptr, L"EDITING" },
		{ L"S / F / Del", L"Split · Fade · Delete selection" },
		{ L"Tab / Shift+Tab", L"Cycle region selection" },
		{ L"Shift+F2", L"Rename selected region" },
		{ L"F2", L"Rename focused track" },
		{ L"Ctrl+D", L"Duplicate selected region after itself" },
		{ L"Ctrl+←/→", L"Nudge selected region (snap unit; Shift=4×)" },
		{ L"Ctrl+] / [", L"Raise / lower region · Shift = top/bottom" },
		{ L"Ctrl+Alt+Shift+]", L"Cycle region layer top↔bottom" },
		{ L"Ctrl+L", L"Lock / unlock selected region (blocks move/trim)" },
		{ L"Ctrl+Alt+L", L"Position-lock selected region (blocks move)" },
		{ L"Ctrl+M", L"Mute / unmute selected region" },
		{ L"Ctrl+Shift+↑/↓", L"Region gain ±1 dB" },
		{ L"Alt+O", L"Toggle region opaque / transparent" },
		{ L"Alt+[ / ]", L"Fade-in ±10ms · Alt+Shift = fade-out" },
		{ L"Shift+F", L"Cycle fade shapes (Linear/Power/Fast/Slow/Sym)" },
		{ L"Ctrl+F", L"Toggle fade in/out active" },
		{ L"Shift+E", L"Region gain envelope (unity→dip→unity)" },
		{ L"Ctrl+Alt+↑/↓", L"Nudge region envelope mid gain" },
		{ nullptr, L"AUDIO TOOLS" },
		{ L"T / Shift+T", L"Stretch 1.5× · compress 0.75× (RubberBand)" },
		{ L"R / Shift+R", L"Pitch +1 / −1 semitone (duration held)" },
		{ L"Alt+Shift+T / dbl-click trim", L"Reset strip preamp trim to 0 dB" },
		{ L"Alt+Shift+R / Alt+dbl-click strip", L"Reset strip fader·pan·send" },
		{ L"V", L"Reverse selected audio region" },
		{ L"N", L"Normalize selection to −1 dBFS" },
		{ L"I", L"Strip silence (−40 dBFS threshold)" },
		{ L"Alt+Shift+I / Alt+click label", L"Toggle strip polarity invert (Ø)" },
		{ L"B", L"Consolidate / bounce selection in place" },
		{ L"Alt+B / Alt+click FX", L"Bypass all ACE inserts" },
		{ L"Alt+Shift+B / Alt+Shift+click FX", L"Activate all ACE inserts" },
		{ L"Z", L"Freeze / unfreeze focused audio track" },
		{ L"G / Shift+G", L"Snap on/off · cycle density" },
		{ L"Status snap chip", L"Show density · click to cycle" },
		{ nullptr, L"MARKERS & RANGES" },
		{ L"M / Shift+M", L"Add / remove marker" },
		{ L"Ctrl+Alt+←/→", L"Previous / next marker" },
		{ L"Ctrl+Alt+,/.", L"Previous / next cue marker" },
		{ L"Ctrl+Shift+M", L"Lock / unlock marker at playhead" },
		{ L"Ctrl+Alt+M", L"Toggle CD flag on marker at playhead" },
		{ L"Alt+Shift+S", L"Rename nearest section marker" },
		{ L"Ctrl+Shift+R", L"Range marker ← selection" },
		{ L"Ctrl+Alt+Y", L"Region sync at playhead (Shift=clear)" },
		{ L"Ctrl+Alt+Shift+C", L"Cycle scene marker MIDI channel 0..15" },
		{ L"Ctrl+Shift+P", L"Punch range ← selection" },
		{ L"Ctrl+Shift+L", L"Loop range ← selection" },
		{ L"Ctrl+Shift+K", L"Skip range ← selection" },
		{ L"L / P", L"Loop · Punch" },
		{ L"[ ]  Shift+[ ]", L"Loop start/end · Punch start/end" },
		{ L"Ctrl+[ / ]", L"Locate loop start / end" },
		{ L"Ctrl+Alt+[ / ]", L"Locate punch in / out" },
		{ L"Alt+, / .", L"Nudge playhead by snap unit" },
		{ nullptr, L"VIEW & ZOOM" },
		{ L"A / Shift+A / E", L"Fit all regions · Fit session · Fit selection" },
		{ L"Ctrl+Alt+P", L"Zoom editor to punch range" },
		{ L"Ctrl+Alt+L", L"Zoom editor to loop range" },
		{ L"Ctrl+Alt+E", L"Zoom editor to selection" },
		{ L"Ctrl+Alt+Home", L"Zoom editor to session extent" },
		{ L"Ctrl+Alt+A", L"Zoom editor to all regions" },
		{ L"Ctrl+Alt+0", L"Center view on playhead (keep zoom)" },
		{ L"Ctrl+Shift+0", L"Scroll playhead into view (keep zoom)" },
		{ L"Ctrl+Alt+±", L"Zoom around playhead" },
		{ L"Ctrl+±", L"Zoom in / out (center)" },
		{ L"Wheel / Shift+Wheel", L"Zoom · horizontal scroll" },
		{ L"← →  Shift+←→", L"Scroll editor · larger step" },
		{ L"Alt+Shift+W", L"Toggle narrow mixer strips" },
		{ L"Alt+C", L"Cycle focused track color" },
		{ nullptr, L"MIDI" },
		{ L"Q / Qnt", L"Quantize MIDI notes to snap grid" },
		{ L"Ctrl+Alt+Q", L"Legato MIDI notes (abut ends)" },
		{ L"Shift+Q", L"Force MIDI note length to snap unit" },
		{ L"Shift+click note", L"Toggle MIDI note multi-select" },
		{ L"Shift+drag empty", L"MIDI marquee box-select (Ctrl=add)" },
		{ L"Ctrl+Shift+A", L"Select all MIDI notes" },
		{ L"Ctrl+Shift+I", L"Invert MIDI note selection" },
		{ L"Ctrl+Alt+Shift+P", L"Select MIDI notes of same pitch" },
		{ L"Ctrl+Alt+Shift+V", L"Select MIDI notes of same velocity" },
		{ L"Ctrl+Alt+Shift+O", L"Select MIDI notes of same pitch-class" },
		{ L"Ctrl+Alt+Shift+L", L"Select MIDI notes of same length" },
		{ L"Ctrl+Alt+Shift+N", L"Select MIDI notes of same channel" },
		{ L"Ctrl+Alt+Shift+I", L"Select MIDI notes in loop range" },
		{ L"Ctrl+Alt+Shift+U", L"Select MIDI notes in punch range" },
		{ L"Ctrl+Alt+Shift+X", L"Select MIDI notes outside loop" },
		{ L"Ctrl+Alt+Shift+Y", L"Select MIDI notes outside punch" },
		{ L"Ctrl+Alt+Shift+W", L"Select MIDI notes outside loop∪punch" },
		{ L"Ctrl+Alt+Shift+J", L"Select MIDI notes in loop∩punch" },
		{ L". / ,  Shift", L"MIDI transpose ±1 · ±12 (T+/T−)" },
		{ L"↑ / ↓  Shift", L"MIDI velocity ±1 · ±8 (V+/V−)" },
		{ L"Ctrl+Alt+V / Alt+V+", L"Force MIDI velocity 100 (Alt+V− → 64)" },
		{ L"Alt+←/→  Shift", L"MIDI note length ± (L+/L−; snap unit)" },
		{ L"Ctrl+C / X / V", L"MIDI note copy · cut · paste (at playhead)" },
		{ L"H / Shift+H", L"MIDI humanize · 2× strength (Hum)" },
		{ L"F4 / Step", L"MIDI step-record · Z/Q piano · ←→ rest" },
		{ L"F5 / Chd", L"MIDI chord Off→Maj→Min · multi-key latch" },
		{ L"F6 / Arp", L"MIDI arp on/off · Shift=Up/Dn/UD · grid rate" },
		{ L"F7 / Scl", L"Scale lock · Shift=maj/min · Ctrl=root" },
		{ L"F8", L"MIDI auto lane · CC7·1·11·64·Bend·Press·PolyAT" },
		{ L"F9 / Panic", L"MIDI panic · kill hanging notes + reset controllers" },
		{ L"P+ / P−  B+ / B−", L"MIDI Program Change ± · Bank Select ±" },
		{ nullptr, L"MIXER" },
		{ L"Alt+Shift+M / U", L"Mute / Solo all audio tracks + MIDI" },
		{ L"Alt+M", L"Clear all mutes (audio + MIDI)" },
		{ L"Alt+U", L"Clear all solos (audio + MIDI)" },
		{ L"Alt+Shift+A / Shift+click auto", L"Cycle Gain→Pan→Send automation lane" },
		{ L"Alt+Shift+Y", L"Cycle auto Off→Play→Write→Touch (current lane)" },
		{ L"Alt+Shift+L", L"Cycle Solo → PFL → AFL (listen mode)" },
		{ L"Mon / Alt+Shift+O", L"Cycle monitor Auto → In → Disk" },
		{ L"Ctrl+Alt+Shift+,/.", L"Cycle VST3 catalog insert on Audio 2" },
		{ L"Alt+E", L"Export selection time range WAV" },
		{ L"C / K", L"Count-in · Click (metronome)" },
		{ L"Alt+Shift+K / Boost", L"Cycle monitor solo boost 0/+3/+6 dB" },
		{ L"Alt+Shift+D / Shift+click Dim", L"Cycle monitor dim −20/−12/−6 dB" },
		{ L"Alt+Shift+H / Shift+click Click", L"Cycle click gain −12/−6/0/+6 dB" },
		{ L"Alt+Shift+N / Alt+Shift+click label", L"Toggle track comment (Cm)" },
		{ L"MLim (Monitor)", L"Master ACE Limiter bypass toggle" },
		{ L"+A / Shift+click +A", L"Add mono audio · stereo Equal-Power" },
		{ L"Home / 0", L"Follow transport (reset view)" },
		{ L"Dbl-click FX param", L"Reset FX/VST3 knob to insert default" },
		{ L"Alt+↑ / Alt+↓", L"Reorder audio track" },
		{ L"Alt+Shift+E", L"Toggle track active (group shares)" },
		{ L"U / Shift+U", L"Apply / clear track group (mute·solo·gain·arm)" },
		{ L"Alt+Shift+G / Alt+Grp", L"Toggle group relative gain" },
		{ L"Alt+Shift+C", L"Cycle track group color (G bar)" },
		{ L"Alt+Shift+←/→ · wheel", L"Scroll mixer strips when overflowing" },
		{ L"Ctrl+Alt+H", L"Toggle Comp sidechain (Audio 1→2)" },
		{ L"Y / Shift+Y", L"Spectrum on/off · Master mix" },
		{ L"X / Shift+X", L"Phase corr / gonio · Master L/R" },
		{ L"J / Shift+J", L"LUFS ≈ on/off · Master mix" },
		{ L"Alt+Shift+V", L"VCA assign Audio 1 (status line)" },
		{ nullptr, L"MOUSE" },
		{ L"Click track label", L"Focus track · gain auto lane follows" },
		{ L"Shift+click Solo", L"Toggle solo-isolate (Si)" },
		{ L"Ctrl+click Solo", L"Toggle solo-safe (Ss)" },
		{ L"Alt+click Solo", L"Exclusive solo (clear other solos)" },
		{ L"Alt+click pan", L"Center all track pans" },
		{ L"Alt+click send", L"Zero all aux sends" },
		{ L"Alt+click fader", L"Reset all faders to unity" },
		{ L"Alt+click Arm", L"Disarm all audio tracks" },
		{ L"Alt+Shift+click Arm", L"Arm all audio tracks" },
		{ L"Alt+click meter", L"Clear all PeakMeter clip latches" },
		{ L"Shift+click meter", L"Cycle meter tap In/Pre/Post/Out" },
		{ L"Ctrl+click meter", L"Cycle meter type Peak↔K14" },
		{ L"Ctrl+click Send", L"Mute / unmute Aux send (Sm)" },
		{ L"Dbl-click FX param", L"Reset FX knob to insert default" },
		{ L"Dbl-click fader/pan", L"Reset fader → unity · pan → center" },
		{ L"Dbl-click send", L"Reset aux send → 0 (off)" },
	};

	const float s = _dpi_scale;
	const size_t total = sizeof (kRows) / sizeof (kRows[0]);
	int n_bindings = 0;
	for (size_t i = 0; i < total; ++i) {
		if (kRows[i].key) {
			++n_bindings;
		}
	}

	/* Dim the shell; the card lists bindings the shell actually handles. */
	_ui.modal_scrim (D2D1::RectF (0.f, 0.f, (float)_width, (float)_height), 0.66f);

	const float pad = 20.f * s;
	const float card_w = std::min ((float)_width - pad * 2.f, 1120.f * s);
	const float card_h = std::min ((float)_height - pad * 2.f, 720.f * s);
	const float cx = ((float)_width - card_w) * 0.5f;
	const float cy = ((float)_height - card_h) * 0.5f;
	const D2D1_RECT_F card = D2D1::RectF (cx, cy, cx + card_w, cy + card_h);
	const float radius = ui::theme ().metrics.radius_lg;
	_ui.shadow (card, radius + 2.f);
	_ui.fill_round (card, radius, ui::ColorToken::Card);
	_ui.stroke_round (card, radius, ui::ColorToken::BorderStrong, 1.f);

	/* Header: accent tab, section title, binding count. */
	const float inset = 18.f * s;
	const float header_h = 46.f * s;
	const float tab_w = 3.f * s;
	const float tab_h = 18.f * s;
	_ui.fill_round (D2D1::RectF (card.left + inset, card.top + (header_h - tab_h) * 0.5f,
	                             card.left + inset + tab_w, card.top + (header_h + tab_h) * 0.5f),
	                1.5f, ui::ColorToken::Accent);
	_ui.text_vcenter (L"Shortcuts", ui::TextRole::Section, ui::ColorToken::TextPrimary,
	                  D2D1::RectF (card.left + inset + _ui.dp (12.f), card.top,
	                               card.right - inset, card.top + header_h));
	wchar_t meta[64];
	_snwprintf_s (meta, _TRUNCATE, L"%d bindings", n_bindings);
	_ui.text_vcenter (meta, ui::TextRole::Caption, ui::ColorToken::TextMuted,
	                  D2D1::RectF (card.left + inset, card.top, card.right - inset,
	                               card.top + header_h),
	                  DWRITE_TEXT_ALIGNMENT_TRAILING);
	_ui.divider (D2D1::RectF (card.left + inset, card.top + header_h,
	                          card.right - inset, card.top + header_h + std::max (1.f, s)));

	const float footer_h = 30.f * s;
	const float wall_top = card.top + header_h;
	const float wall_bot = card.bottom - footer_h;

	/* Two key/action columns when wide; one column on small windows. */
	const float inner_l = card.left + inset;
	const float inner_r = card.right - inset - 6.f * s;
	const int n_cols = (inner_r - inner_l) >= 860.f * s ? 2 : 1;
	const float col_gap = 26.f * s;
	const float col_w = ((inner_r - inner_l) - col_gap * (n_cols - 1)) / (float)n_cols;
	const float key_w = std::min (246.f * s, col_w * 0.55f);
	const float row_h = 18.f * s;
	const float sec_h = 25.f * s;

	auto flow = [&] (int start, bool draw) -> int {
		int idx = start;
		for (int c = 0; c < n_cols && idx < (int)total; ++c) {
			const float x = inner_l + c * (col_w + col_gap);
			float y = wall_top + 3.f * s;
			while (idx < (int)total) {
				const bool is_sec = (kRows[idx].key == nullptr);
				const float h = is_sec ? sec_h : row_h;
				if (y + h > wall_bot - 2.f * s) {
					break;
				}
				/* Never orphan a section header at a column foot. */
				if (is_sec && y + sec_h + row_h > wall_bot - 2.f * s) {
					break;
				}
				if (draw) {
					if (is_sec) {
						const float my = y + sec_h * 0.5f;
						_ui.fill_round (D2D1::RectF (x, my - 1.f * s, x + 9.f * s, my + 1.f * s),
						                1.f, ui::ColorToken::Accent, 0.75f);
						_ui.text_vcenter (kRows[idx].action, ui::TextRole::Caption,
						                  ui::ColorToken::TextMuted,
						                  D2D1::RectF (x + _ui.dp (14.f), y, x + col_w, y + sec_h));
					} else {
						_ui.text_vcenter (kRows[idx].key, ui::TextRole::Mono,
						                  ui::ColorToken::TextPrimary,
						                  D2D1::RectF (x, y, x + key_w - 6.f * s, y + row_h));
						_ui.text_vcenter (kRows[idx].action, ui::TextRole::Caption,
						                  ui::ColorToken::TextSecondary,
						                  D2D1::RectF (x + key_w, y, x + col_w, y + row_h));
					}
				}
				y += h;
				++idx;
			}
			if (draw && c + 1 < n_cols) {
				const float dx = x + col_w + col_gap * 0.5f;
				_ui.divider (D2D1::RectF (dx, wall_top + 6.f * s, dx + std::max (1.f, s),
				                          wall_bot - 6.f * s));
			}
		}
		return idx;
	};

	/* Largest start offset whose rows still fit all columns. */
	int scroll_max = 0;
	{
		int lo = 0;
		int hi = (int)total;
		while (lo < hi) {
			const int mid = (lo + hi) / 2;
			if (flow (mid, false) >= (int)total) {
				hi = mid;
			} else {
				lo = mid + 1;
			}
		}
		scroll_max = lo;
	}
	_help_scroll_max = scroll_max;
	_help_scroll = std::max (0, std::min (scroll_max, _help_scroll));

	_rt->PushAxisAlignedClip (D2D1::RectF (card.left, wall_top, card.right, wall_bot),
	                          D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
	flow (_help_scroll, true);
	_rt->PopAxisAlignedClip ();

	/* Thin scroll indicator when the table continues past this page. */
	if (scroll_max > 0) {
		const float bar_x = card.right - inset + 1.f * s;
		const float bar_w = 3.f * s;
		const float track_top = wall_top + 6.f * s;
		const float track_bot = wall_bot - 6.f * s;
		const float track_h = std::max (1.f, track_bot - track_top);
		const float page_rows = (float)(flow (0, false) - 0);
		const float thumb_h = std::min (track_h,
		                                std::max (22.f * s,
		                                          track_h * page_rows / (float)total));
		const float t = (float)_help_scroll / (float)scroll_max;
		const float thumb_y = track_top + (track_h - thumb_h) * t;
		_ui.fill_round (D2D1::RectF (bar_x, track_top, bar_x + bar_w, track_bot),
		                1.5f, ui::ColorToken::Control, 0.9f);
		_ui.fill_round (D2D1::RectF (bar_x, thumb_y, bar_x + bar_w, thumb_y + thumb_h),
		                1.5f, ui::ColorToken::BorderStrong);
	}

	_ui.divider (D2D1::RectF (card.left + inset, wall_bot, card.right - inset,
	                          wall_bot + std::max (1.f, s)));
	const wchar_t* foot = (scroll_max > 0)
	                      ? L"Click anywhere or Esc to close  ·  scroll for more"
	                      : L"Click anywhere or Esc to close";
	_ui.text_vcenter (foot, ui::TextRole::Caption, ui::ColorToken::TextMuted,
	                  D2D1::RectF (inner_l, wall_bot, card.right - inset, card.bottom));
	if (scroll_max > 0) {
		_ui.text_vcenter (L"wheel  ·  PgUp / PgDn", ui::TextRole::Caption,
		                  ui::ColorToken::TextSecondary,
		                  D2D1::RectF (inner_l, wall_bot, card.right - inset, card.bottom),
		                  DWRITE_TEXT_ALIGNMENT_TRAILING);
	}
}

void
ShellWindow::draw_mixer_pane ()
{
	const D2D1_RECT_F& r = _mixer_pane;
	_ui.panel (r, 10.f, false);

	const float inset = 12.f * _dpi_scale;
	const float header_h = 32.f * _dpi_scale;
	_ui.fill_round (D2D1::RectF (r.left + 16.f * _dpi_scale, r.top + 11.f * _dpi_scale,
	                             r.left + 19.f * _dpi_scale, r.top + 27.f * _dpi_scale),
	                1.5f, ui::ColorToken::Accent);
	_ui.text_vcenter (L"Mixer", ui::TextRole::Section,
	                  ui::ColorToken::TextPrimary,
	                  D2D1::RectF (r.left + 26.f * _dpi_scale, r.top,
	                               r.left + 96.f * _dpi_scale, r.top + header_h));

	wchar_t hint[160];
	if (_engine_snap.track_eq_ok && !_engine_snap.track_eq_name.empty ()) {
		_snwprintf_s (hint, _TRUNCATE,
		              L"PeakMeter  ·  %s%s  ·  L/M/H gain (click FX=bypass)",
		              _engine_snap.track_eq_name.c_str (),
		              _engine_snap.track_eq_bypassed ? L" BYP" : L"");
	} else if (_engine_snap.track_fx_ok && !_engine_snap.track_fx_name.empty ()) {
		_snwprintf_s (hint, _TRUNCATE,
		              L"Meters: PeakMeter  ·  %s%s  ·  Blend %.0f%% (click FX=bypass)",
		              _engine_snap.track_fx_name.c_str (),
		              _engine_snap.track_fx_bypassed ? L" BYP" : L"",
		              _engine_snap.track_fx_param * 100.f);
	} else if (_engine_snap.meters_real) {
		wcsncpy_s (hint, L"PeakMeter  ·  hold/clip  ·  M/S  ·  pan  ·  send", _TRUNCATE);
	} else if (_transport_playing) {
		wcsncpy_s (hint, L"Meters: play stub (no Session)", _TRUNCATE);
	} else {
		wcsncpy_s (hint, L"M/S · faders  ·  idle until Play", _TRUNCATE);
	}
	const float scroll_w = (_mixer_scroll_max > 1.f) ? 66.f * _dpi_scale : 0.f;
	_ui.text_vcenter (hint, ui::TextRole::Caption, ui::ColorToken::TextMuted,
	                  D2D1::RectF (r.left + 98.f * _dpi_scale, r.top,
	                               r.right - inset - scroll_w, r.top + header_h));
	if (_mixer_scroll_max > 1.f) {
		wchar_t scr[32];
		_snwprintf_s (scr, _TRUNCATE, L"◀ ▶ %.0f", _mixer_scroll_x);
		_ui.text_vcenter (scr, ui::TextRole::Caption, ui::ColorToken::TextSecondary,
		                  D2D1::RectF (r.right - inset - scroll_w, r.top,
		                               r.right - inset, r.top + header_h),
		                  DWRITE_TEXT_ALIGNMENT_TRAILING);
	}
	_ui.divider (D2D1::RectF (r.left + 12.f * _dpi_scale, r.top + header_h,
	                          r.right - 12.f * _dpi_scale,
	                          r.top + header_h + std::max (1.f, _dpi_scale)));

	const int n_mix = n_mixer_strips ();
	_rt->PushAxisAlignedClip (_mixer_strips_clip, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
	for (int i = 0; i < n_mix; ++i) {
		_mixers[i].draw (_ui);
	}
	_rt->PopAxisAlignedClip ();
	_history.draw (_ui);
	_monitor.draw (_ui);
}

void
ShellWindow::on_paint ()
{
	if (!ensure_device_resources ()) {
		return;
	}

	_rt->BeginDraw ();
	_rt->SetTransform (D2D1::Matrix3x2F::Identity ());

	D2D1_SIZE_F sz = _rt->GetSize ();
	_rt->FillRectangle (D2D1::RectF (0, 0, sz.width, sz.height), _brush_bg);

	/* Brand bar: translucent studio chrome, wordmark and live state chip. */
	const float brand_pad = 24.f * _dpi_scale;
	_ui.fill_round (_brand_bar, 0.f, ui::ColorToken::Surface, 0.62f);
	_ui.divider (D2D1::RectF (_brand_bar.left, _brand_bar.bottom - _ui.dp (1.f),
	                          _brand_bar.right, _brand_bar.bottom));

	const float post_h = 22.f * _dpi_scale;
	const float post_y = (_brand_bar.top + _brand_bar.bottom - post_h) * 0.5f;
	_ui.fill_round (D2D1::RectF (brand_pad, post_y,
	                             brand_pad + _ui.dp (4.f), post_y + post_h),
	                2.f, ui::ColorToken::Accent, 0.96f + 0.04f * _pulse);
	_ui.text_vcenter (L"Airdour", ui::TextRole::Brand, ui::ColorToken::TextPrimary,
	                  D2D1::RectF (brand_pad + _ui.dp (14.f), _brand_bar.top,
	                               _brand_bar.right - _ui.dp (130.f), _brand_bar.bottom));

	{
		const wchar_t* state_label = _transport.recording () ? L"REC"
		                            : (_transport_playing ? L"PLAY" : L"STOP");
		const ui::ColorToken state_color = _transport.recording () ? ui::ColorToken::Record
		                                 : (_transport_playing ? ui::ColorToken::Accent
		                                                       : ui::ColorToken::TextMuted);
		const float pill_w = 78.f * _dpi_scale;
		const float pill_h = 22.f * _dpi_scale;
		const float pill_x = _brand_bar.right - brand_pad - pill_w;
		const float pill_y = (_brand_bar.top + _brand_bar.bottom - pill_h) * 0.5f;
		const D2D1_RECT_F pill = D2D1::RectF (pill_x, pill_y,
		                                      pill_x + pill_w, pill_y + pill_h);
		const float pill_rad = pill_h * 0.5f / _dpi_scale;
		_ui.fill_round (pill, pill_rad, ui::ColorToken::Control, 0.85f);
		_ui.stroke_round (pill, pill_rad, ui::ColorToken::Border, 1.f);
		const float dot = 6.f * _dpi_scale;
		const float dot_x = pill_x + 12.f * _dpi_scale;
		_ui.fill_round (D2D1::RectF (dot_x, pill_y + (pill_h - dot) * 0.5f,
		                             dot_x + dot, pill_y + (pill_h + dot) * 0.5f),
		                dot * 0.5f / _dpi_scale, state_color);
		_ui.text_vcenter (state_label, ui::TextRole::Caption,
		                  ui::ColorToken::TextSecondary,
		                  D2D1::RectF (dot_x + dot + _ui.dp (7.f), pill_y,
		                               pill.right - _ui.dp (6.f), pill.bottom));
	}

	sync_transport_clock_from_engine ();
	_menu_bar.draw (_ui);
	_transport.draw (_ui, _playhead_sec);

	draw_editor_pane ();
	draw_mixer_pane ();
	if (_engine_snap.spectrum_enabled) {
		_spectrum.set_bands (_engine_snap.spectrum_band, EngineSnapshot::kSpectrumBands);
		_spectrum.set_label (_engine_snap.spectrum_label);
		_spectrum.set_visible (true);
		_spectrum.draw (_rt, _brush_brand, _brush_body, _brush_accent,
		               _brush_pane, _brush_pane_edge, _fmt_meta, _dpi_scale);
	}
	if (_engine_snap.correlation_enabled) {
		_correlation.set_correlation (_engine_snap.correlation);
		_correlation.set_gonio (_engine_snap.gonio_x, _engine_snap.gonio_y,
		                        EngineSnapshot::kGonioPts);
		_correlation.set_label (_engine_snap.correlation_label);
		_correlation.set_visible (true);
		_correlation.draw (_rt, _brush_brand, _brush_body, _brush_accent,
		                   _brush_pane, _brush_pane_edge, _fmt_meta, _dpi_scale);
	}
	if (_engine_snap.loudness_enabled) {
		_loudness.set_values (_engine_snap.loudness_momentary,
		                      _engine_snap.loudness_integrated);
		_loudness.set_label (_engine_snap.loudness_label);
		_loudness.set_visible (true);
		_loudness.draw (_rt, _brush_brand, _brush_body, _brush_accent,
		                _brush_pane, _brush_pane_edge, _fmt_meta, _dpi_scale);
	}
	if (_help_visible) {
		draw_help_overlay ();
	}
	if (_about.about_visible ()) {
		_about.layout ((float)_width, (float)_height, _dpi_scale);
		_about.draw_about (_ui);
	}
	if (_prefs_visible) {
		_prefs.layout ((float)_width, (float)_height, _dpi_scale);
		_prefs.draw (_ui);
	}
	if (_recent.visible ()) {
		_recent.layout_under (_transport.open_btn (), (float)_width, (float)_height, _dpi_scale);
		_recent.draw (_ui);
	}
	if (_recovery.visible ()) {
		_recovery.layout ((float)_width, (float)_height, _dpi_scale);
		_recovery.draw (_ui);
	}
	if (_about.splash_visible ()) {
		_about.draw_splash (_rt, _brush_brand, _brush_body, _brush_accent, _brush_pane,
		                    _fmt_brand, _fmt_meta, _dpi_scale, _pulse);
	}

	wchar_t status[512];
	float dsp = _engine_snap.dsp_load_pct;
	if (dsp < 0.f) {
		dsp = 0.f;
	}
	if (dsp > 100.f) {
		dsp = 100.f;
	}
	wchar_t vca_frag[96] = L"";
	if (_engine_snap.vca_ok) {
		_snwprintf_s (vca_frag, _TRUNCATE, L"  ·  VCA 1 · slaves=%d · g=%.2f",
		              _engine_snap.vca_slave_count, _engine_snap.vca_gain);
	}
	wchar_t vst_frag[160] = L"";
	if (_engine_snap.vst3_ok) {
		if (_engine_snap.vst3_param_count > 0) {
			_snwprintf_s (vst_frag, _TRUNCATE,
			              L"  ·  VST3 %s%s · %s=%.2f",
			              _engine_snap.vst3_name,
			              _engine_snap.vst3_bypassed ? L" BYP" : L"",
			              _engine_snap.vst3_param_name[0],
			              _engine_snap.vst3_param_norm[0]);
		} else {
			_snwprintf_s (vst_frag, _TRUNCATE, L"  ·  VST3 %s%s",
			              _engine_snap.vst3_name,
			              _engine_snap.vst3_bypassed ? L" BYP" : L"");
		}
	}
	wchar_t sess_frag[192] = L"";
	if (_engine_snap.session_ok) {
		std::wstring base = _engine_snap.session_path;
		const size_t slash = base.find_last_of (L"\\/");
		if (slash != std::wstring::npos) {
			base = base.substr (slash + 1);
		}
		_snwprintf_s (sess_frag, _TRUNCATE, L"  ·  %s (%d tracks)",
		              base.c_str (), _engine_snap.audio_track_count);
	}
	wchar_t dev_frag[192] = L"";
	if (_engine_snap.running && !_engine_snap.audio_out_name.empty ()) {
		const double out_ms = _engine_snap.sample_rate
		                      ? (1000.0 * (double)_engine_snap.systemic_out_frames
		                         / (double)_engine_snap.sample_rate)
		                      : 0.0;
		_snwprintf_s (dev_frag, _TRUNCATE, L"  ·  %s  ·  out %.1f ms",
		              _engine_snap.audio_out_name.c_str (), out_ms);
	}
	wchar_t hist_frag[192] = L"";
	if (_engine_snap.undo_depth || _engine_snap.redo_depth) {
		_snwprintf_s (hist_frag, _TRUNCATE, L"  ·  undo %s (%u)  ·  redo %s (%u)",
		              _engine_snap.next_undo_name[0] ? _engine_snap.next_undo_name : L"—",
		              _engine_snap.undo_depth,
		              _engine_snap.next_redo_name[0] ? _engine_snap.next_redo_name : L"—",
		              _engine_snap.redo_depth);
	}
	if (_engine_snap.running) {
		_snwprintf_s (status, _TRUNCATE,
		              L"Engine  %s  ·  %u Hz  ·  period %u  ·  DSP %.1f%%  ·  cb %lluµs avg  ·  xruns %llu%s%s%s%s%s  ·  %s",
		              _engine_snap.backend.c_str (),
		              _engine_snap.sample_rate,
		              _engine_snap.period_frames,
		              dsp,
		              (unsigned long long)_engine_snap.callback_avg_us,
		              (unsigned long long)_engine_snap.xrun_count,
		              vca_frag,
		              vst_frag,
		              sess_frag,
		              dev_frag,
		              hist_frag,
		              _engine_snap.detail.c_str ());
	} else if (_engine_snap.available) {
		_snwprintf_s (status, _TRUNCATE,
		              L"Engine idle  ·  DSP %.1f%%  ·  %d tracks%s%s%s%s  ·  %s",
		              dsp, n_audio_lanes (), vca_frag, vst_frag, sess_frag, hist_frag,
		              _engine_snap.detail.c_str ());
	} else {
		_snwprintf_s (status, _TRUNCATE,
		              L"UI shell  ·  %d tracks%s%s  ·  %s",
		              n_audio_lanes (), vca_frag, vst_frag, _engine_snap.detail.c_str ());
	}

	/* Status bar: muted telemetry strip with a compact DSP load meter. */
	const float status_pad = 20.f * _dpi_scale;
	_ui.fill_round (_status_bar, 0.f, ui::ColorToken::Surface, 0.72f);
	_ui.divider (D2D1::RectF (_status_bar.left, _status_bar.top,
	                          _status_bar.right, _status_bar.top + _ui.dp (1.f)));

	const float meter_total = 156.f * _dpi_scale;
	const float label_w = 58.f * _dpi_scale;
	const float meter_gap = _ui.dp (8.f);
	const float track_w = meter_total - label_w - meter_gap;
	const float track_right = _status_bar.right - status_pad;
	const float track_left = track_right - track_w;
	const float unload_w = 18.f * _dpi_scale;
	const float bypass_w = 28.f * _dpi_scale;
	const float reset_w = 28.f * _dpi_scale;
	const float reset_n_w = 16.f * _dpi_scale;
	const float unload_gap = 6.f * _dpi_scale;
	const float gap = 8.f * _dpi_scale;
	float mid_y = (_status_bar.top + _status_bar.bottom) * 0.5f;
	float text_right = track_left - _ui.dp (18.f);
	_status_vst3_bypass_btn = D2D1::RectF (0.f, 0.f, 0.f, 0.f);
	for (int i = 0; i < EngineSnapshot::kMaxVst3Params; ++i) {
		_status_vst3_param_reset_btn[i] = D2D1::RectF (0.f, 0.f, 0.f, 0.f);
	}
	_status_vst3_unload_btn = D2D1::RectF (0.f, 0.f, 0.f, 0.f);
	_status_snap_btn = D2D1::RectF (0.f, 0.f, 0.f, 0.f);
	float right_cursor = track_left - gap;
	if (_engine_snap.vst3_ok) {
		float bh = 16.f * _dpi_scale;
		_status_vst3_unload_btn = D2D1::RectF (right_cursor - unload_w, mid_y - bh * 0.5f,
		                                       right_cursor, mid_y + bh * 0.5f);
		int n = _engine_snap.vst3_param_count;
		if (n < 0) {
			n = 0;
		}
		if (n > EngineSnapshot::kMaxVst3Params) {
			n = EngineSnapshot::kMaxVst3Params;
		}
		float edge = _status_vst3_unload_btn.left;
		for (int i = n - 1; i >= 0; --i) {
			const float ww = (i == 0) ? reset_w : reset_n_w;
			_status_vst3_param_reset_btn[i] = D2D1::RectF (
				edge - unload_gap - ww,
				mid_y - bh * 0.5f,
				edge - unload_gap,
				mid_y + bh * 0.5f);
			edge = _status_vst3_param_reset_btn[i].left;
		}
		_status_vst3_bypass_btn = D2D1::RectF (
			edge - unload_gap - bypass_w,
			mid_y - bh * 0.5f,
			edge - unload_gap,
			mid_y + bh * 0.5f);
		right_cursor = _status_vst3_bypass_btn.left - unload_gap;
		text_right = right_cursor;
	}
	if (_engine_snap.snap_enabled) {
		float bh = 14.f * _dpi_scale;
		float chip_w = 44.f * _dpi_scale;
		_status_snap_btn = D2D1::RectF (right_cursor - chip_w, mid_y - bh * 0.5f,
		                               right_cursor, mid_y + bh * 0.5f);
		text_right = _status_snap_btn.left - gap;
	}

	_ui.text_vcenter_n (status, (UINT32)wcslen (status), ui::TextRole::Mono,
	                    ui::ColorToken::TextSecondary,
	                    D2D1::RectF (_status_bar.left + status_pad, _status_bar.top,
	                                 text_right, _status_bar.bottom));

	if (_engine_snap.vst3_ok
	    && _status_vst3_unload_btn.right > _status_vst3_unload_btn.left) {
		if (_status_vst3_bypass_btn.right > _status_vst3_bypass_btn.left) {
			if (_engine_snap.vst3_bypassed) {
				_ui.fill_round (_status_vst3_bypass_btn, 3.f, ui::ColorToken::Accent, 0.45f);
			} else {
				_ui.fill_round (_status_vst3_bypass_btn, 3.f, ui::ColorToken::Control, 0.8f);
			}
			_ui.stroke_round (_status_vst3_bypass_btn, 3.f, ui::ColorToken::Accent, 1.f);
			_ui.text_vcenter (L"BYP", ui::TextRole::Caption, ui::ColorToken::AccentInk,
			                  _status_vst3_bypass_btn, DWRITE_TEXT_ALIGNMENT_CENTER);
		}
		for (int i = 0; i < EngineSnapshot::kMaxVst3Params; ++i) {
			const D2D1_RECT_F& r = _status_vst3_param_reset_btn[i];
			if (r.right <= r.left) {
				continue;
			}
			_ui.fill_round (r, 3.f, ui::ColorToken::Control, 0.8f);
			_ui.stroke_round (r, 3.f, ui::ColorToken::Accent, 1.f);
			if (i == 0) {
				_ui.text_vcenter (L"RST", ui::TextRole::Caption, ui::ColorToken::AccentInk,
				                  r, DWRITE_TEXT_ALIGNMENT_CENTER);
			} else {
				const wchar_t lab[2] = { (wchar_t)(L'0' + i), L'\0' };
				_ui.text_vcenter (lab, ui::TextRole::Caption, ui::ColorToken::AccentInk,
				                  r, DWRITE_TEXT_ALIGNMENT_CENTER);
			}
		}
		_ui.fill_round (_status_vst3_unload_btn, 3.f, ui::ColorToken::Control, 0.8f);
		_ui.stroke_round (_status_vst3_unload_btn, 3.f, ui::ColorToken::Accent, 1.f);
		_ui.text_vcenter (L"\u00d7", ui::TextRole::Caption, ui::ColorToken::AccentInk,
		                  _status_vst3_unload_btn, DWRITE_TEXT_ALIGNMENT_CENTER);
	}
	if (_status_snap_btn.right > _status_snap_btn.left) {
		_ui.fill_round (_status_snap_btn, 3.f, ui::ColorToken::Control, 0.8f);
		_ui.stroke_round (_status_snap_btn, 3.f, ui::ColorToken::Accent, 1.f);
		const wchar_t* lab = _transport.snap_btn_label ();
		_ui.text_vcenter (lab, ui::TextRole::Caption, ui::ColorToken::AccentInk,
		                  _status_snap_btn, DWRITE_TEXT_ALIGNMENT_CENTER);
	}

	if (_engine_snap.available) {
		const float bar_h = 6.f * _dpi_scale;
		const float bar_rad = bar_h * 0.5f / _dpi_scale;
		const D2D1_RECT_F track = D2D1::RectF (track_left, mid_y - bar_h * 0.5f,
		                                       track_right, mid_y + bar_h * 0.5f);
		_ui.fill_round (track, bar_rad, ui::ColorToken::Control);
		_ui.stroke_round (track, bar_rad, ui::ColorToken::Border, 1.f, 0.8f);
		const float fill_right = track.left + (track.right - track.left) * (dsp / 100.f);
		if (fill_right > track.left + 1.f) {
			_ui.fill_round (D2D1::RectF (track.left, track.top,
			                             fill_right, track.bottom),
			                bar_rad,
			                dsp >= 85.f ? ui::ColorToken::MeterHot : ui::ColorToken::Accent,
			                0.95f);
		}
		wchar_t dsp_lab[24];
		_snwprintf_s (dsp_lab, _TRUNCATE, L"DSP %.0f%%", dsp);
		_ui.text_vcenter (dsp_lab, ui::TextRole::Caption,
		                  dsp >= 85.f ? ui::ColorToken::MeterHot
		                              : ui::ColorToken::TextMuted,
		                  D2D1::RectF (track_left - meter_gap - label_w,
		                               _status_bar.top, track_left - meter_gap,
		                               _status_bar.bottom),
		                  DWRITE_TEXT_ALIGNMENT_TRAILING);
	}

	_transport.draw_overlay (_ui);
	_menu_bar.draw_overlay (_ui);

	if (_drag == DragMode::None && !_help_visible && !_prefs_visible
	    && !_menu_bar.open ()
	    && !_about.about_visible () && !_recent.visible ()
	    && !_recovery.visible () && !_about.splash_visible ()) {
		wchar_t tip[96];
		if (_transport.hover_tooltip (tip, 96)) {
			_ui.tooltip (tip, D2D1::Point2F (_mouse_x, _mouse_y),
			             D2D1::RectF (0.f, 0.f, (float)_width, (float)_height));
		}
	}

	HRESULT hr = _rt->EndDraw ();
	if (hr == D2DERR_RECREATE_TARGET) {
		discard_device_resources ();
	}
}

int
ShellWindow::run_message_loop (unsigned timeout_secs)
{
	_timeout_secs = timeout_secs;
	_start_tick = GetTickCount ();
	_last_tick_ms = _start_tick;
	_running = true;
	_engine_snap = _engine.snapshot ();

	MSG msg = {};
	while (_running && GetMessageW (&msg, nullptr, 0, 0) > 0) {
		TranslateMessage (&msg);
		DispatchMessageW (&msg);
	}

	/* Session/Graph/Butler and often backend->stop() hang under MSVC.
	 * Unattended --timeout smoke must leave immediately without joins.
	 * ExitProcess (not TerminateProcess) so CRT flushes redirected stderr. */
	if (timeout_secs > 0) {
		const unsigned exit_code = _smoke_failed ? 1u : 0u;
		std::fwprintf (stderr, L"airdour_win_shell: timeout smoke — ExitProcess(%u)\n", exit_code);
		std::fflush (stderr);
		std::fflush (stdout);
		ExitProcess (exit_code);
	}

	_engine.stop ();
	return (int)msg.wParam;
}

